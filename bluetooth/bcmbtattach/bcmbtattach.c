/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Andrey Bodrov <ap.bodrov@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * bcmbtattach -- bring up a UART-attached Broadcom Bluetooth controller
 * (BCM43430A1, the Bluetooth half of the AP6212 on the Banana Pi M64) and
 * hand it to the Bluetooth stack through ng_h4.
 *
 * FreeBSD has no in-kernel Broadcom UART HCI driver, so the controller is
 * brought up from userland, the same shape as usr.sbin/bluetooth/iwmbtfw:
 *
 *   1. power:    dev.bcmbt.0.power=2 (bcmbt(4)), or with -g, BT_REG_ON (the
 *                DT's shutdown-gpios) low, then high through /dev/gpiocN
 *   2. tty:      raw 8N1, 115200, RTS/CTS
 *   3. firmware: HCI_Reset, vendor Download_Minidriver (0xfc2e), every
 *                record of the .hcd patchram as an HCI command, then a
 *                second HCI_Reset once the chip has jumped to the patched RAM
 *   4. attach:   an ng_h4 node named <name>, bound to this tty, and the usual
 *                hci/l2cap/btsocket graph (/etc/rc.d/bluetooth only knows
 *                ubt* devices)
 *   5. stay:     keep the tty open for as long as the node should live
 *
 * With bcmbt(4) loaded the kernel enables the LPO (32 kHz) clock and the
 * vbat/vddio rails. With -g instead, they must already be on (on the AP6212
 * they are shared with the WiFi half and on once the SDIO side is powered).
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgpio.h>
#include <netgraph.h>
#include <netgraph/ng_message.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "ng_h4.h"

#define HCI_CMD_PKT        0x01
#define HCI_EVT_PKT        0x04
#define EVT_CMD_COMPLETE   0x0e
#define EVT_CMD_STATUS     0x0f

#define OP_RESET           0x0c03
#define OP_READ_NAME       0x0c14
#define OP_READ_VERSION    0x1001
#define OP_READ_BDADDR     0x1009
#define OP_DL_MINIDRIVER   0xfc2e
#define OP_UPDATE_BAUD     0xfc18

static int verbose;
static int noflow;

/*
 * purpose:     Put the HCI UART in raw 8N1 at `speed`, RTS/CTS unless -N.
 * input:       fd -- open tty; speed -- termios speed
 * output:      none (exits on error)
 * sideEffects: changes the tty's termios; flushes both queues
 */
static void
settty(int fd, speed_t speed)
{
	struct termios t;

	if (tcgetattr(fd, &t) != 0)
		err(1, "tcgetattr");
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	if (!noflow)
		t.c_cflag |= CRTSCTS;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetspeed(&t, speed);
	if (tcsetattr(fd, TCSANOW, &t) != 0)
		err(1, "tcsetattr");
	tcflush(fd, TCIOFLUSH);
}

/*
 * purpose:     Read exactly n bytes, waiting at most timeout_ms for each chunk.
 * input:       fd, buf/n -- destination, timeout_ms
 * output:      0 when n bytes arrived, -1 on timeout or read error
 * sideEffects: consumes bytes from the tty
 */
static int
readn(int fd, uint8_t *buf, size_t n, int timeout_ms)
{
	size_t got = 0;

	while (got < n) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t r;

		if (poll(&p, 1, timeout_ms) <= 0)
			return (-1);
		r = read(fd, buf + got, n - got);
		if (r < 0 && errno != EAGAIN)
			return (-1);
		if (r > 0)
			got += (size_t)r;
	}
	return (0);
}

/*
 * purpose:     Send one HCI command and wait for its Command Complete (or a failing Command Status).
 * input:       fd; op/par/plen -- the command; ret/retlen -- optional return-parameter buffer; timeout_ms
 * output:      status byte (0 = success), -1 on timeout
 * sideEffects: writes the command, consumes and skips unrelated events
 */
static int
hci_cmd(int fd, uint16_t op, const uint8_t *par, uint8_t plen,
    uint8_t *ret, size_t *retlen, int timeout_ms)
{
	uint8_t hdr[4] = { HCI_CMD_PKT, op & 0xff, op >> 8, plen };
	uint8_t ev[2 + 255];

	if (write(fd, hdr, 4) != 4 || (plen && write(fd, par, plen) != plen))
		err(1, "write");
	for (;;) {
		uint8_t ind;

		if (readn(fd, &ind, 1, timeout_ms) != 0)
			return (-1);
		if (ind != HCI_EVT_PKT) {
			if (verbose)
				fprintf(stderr, "stray byte 0x%02x\n", ind);
			continue;
		}
		if (readn(fd, ev, 2, timeout_ms) != 0 ||
		    readn(fd, ev + 2, ev[1], timeout_ms) != 0)
			return (-1);
		if (ev[0] == EVT_CMD_COMPLETE && ev[1] >= 4 &&
		    (ev[3] | (ev[4] << 8)) == op) {
			if (ret && retlen) {
				size_t n = ev[1] - 4u;
				if (n > *retlen)
					n = *retlen;
				memcpy(ret, ev + 6, n);
				*retlen = n;
			}
			return (ev[5]);
		}
		if (ev[0] == EVT_CMD_STATUS && ev[1] >= 4 &&
		    (ev[4] | (ev[5] << 8)) == op && ev[2] != 0)
			return (ev[2]);
		if (verbose)
			fprintf(stderr, "skip event 0x%02x len %u\n", ev[0], ev[1]);
	}
}

/*
 * purpose:     Print the controller's version, name and BD_ADDR.
 * input:       fd; when -- label for the output lines
 * output:      none
 * sideEffects: three HCI commands on the tty; stdout
 */
static void
report(int fd, const char *when)
{
	uint8_t r[255];
	size_t n = sizeof(r);

	if (hci_cmd(fd, OP_READ_VERSION, NULL, 0, r, &n, 1000) == 0 && n >= 8)
		printf("%s: HCI %u rev 0x%04x, LMP %u subver 0x%04x, mfr %u\n", when,
		    r[0], r[1] | (r[2] << 8), r[3], r[6] | (r[7] << 8), r[4] | (r[5] << 8));
	n = sizeof(r) - 1;
	if (hci_cmd(fd, OP_READ_NAME, NULL, 0, r, &n, 1000) == 0) {
		r[n] = 0;
		printf("%s: name \"%s\"\n", when, (char *)r);
	}
	n = sizeof(r);
	if (hci_cmd(fd, OP_READ_BDADDR, NULL, 0, r, &n, 1000) == 0 && n >= 6)
		printf("%s: bdaddr %02x:%02x:%02x:%02x:%02x:%02x\n", when,
		    r[5], r[4], r[3], r[2], r[1], r[0]);
}

/*
 * purpose:     Load a Broadcom .hcd patchram: Download_Minidriver, then every record as an HCI command.
 * input:       fd; path -- the .hcd file
 * output:      none (exits on any failed record)
 * sideEffects: reprograms the controller; it reboots into RAM after the last record
 */
static void
patchram(int fd, const char *path)
{
	uint8_t buf[3 + 255];
	int hcd, n = 0;

	if ((hcd = open(path, O_RDONLY)) < 0)
		err(1, "%s", path);
	if (hci_cmd(fd, OP_DL_MINIDRIVER, NULL, 0, NULL, NULL, 2000) != 0)
		errx(1, "Download_Minidriver failed");
	usleep(50000);
	for (;;) {
		ssize_t r = read(hcd, buf, 3);
		uint16_t op;
		int st;

		if (r == 0)
			break;
		if (r != 3 || read(hcd, buf + 3, buf[2]) != buf[2])
			errx(1, "%s: truncated record %d", path, n);
		op = buf[0] | (buf[1] << 8);
		st = hci_cmd(fd, op, buf + 3, buf[2], NULL, NULL, 2000);
		if (st != 0)
			errx(1, "patchram record %d (op 0x%04x) failed: %d", n, op, st);
		n++;
	}
	close(hcd);
	printf("patchram: %d records\n", n);
	usleep(500000);         /* the last record (Launch_RAM) reboots the core */
}

/*
 * purpose:     Assert device-wakeup and pulse BT_REG_ON low then high.
 * input:       gpioc -- /dev/gpiocN; pin -- BT_REG_ON; wake -- device-wakeup pin or -1
 * output:      none (exits on error)
 * sideEffects: drives two GPIO outputs; the controller restarts from ROM
 */
static void
power_cycle(const char *gpioc, int pin, int wake)
{
	gpio_handle_t h = gpio_open_device(gpioc);

	if (h == GPIO_INVALID_HANDLE)
		err(1, "%s", gpioc);
	/* device-wakeup-gpios held asserted: once the patchram runs, the chip
	 * may enter its UART low-power mode and ignore the line otherwise. */
	if (wake >= 0 && (gpio_pin_output(h, wake) != 0 || gpio_pin_high(h, wake) != 0))
		err(1, "gpio wake %d", wake);
	if (gpio_pin_output(h, pin) != 0)
		err(1, "gpio output %d", pin);
	gpio_pin_low(h, pin);
	usleep(50000);
	gpio_pin_high(h, pin);
	usleep(200000);
	gpio_close(h);
}

/*
 * purpose:     Power-cycle through the bcmbt(4) kernel driver, if it is there.
 * input:       tty/ttylen -- receives "/dev/<dev.bcmbt.0.tty>" when the
 *              driver knows its UART
 * output:      1 if the driver did it, 0 if there is no driver
 * sideEffects: writes dev.bcmbt.0.power=2 (off, then on with LPO, supplies
 *              and wake line handled in the kernel)
 */
static int
driver_power_cycle(char *tty, size_t ttylen)
{
	int cycle = 2;
	char name[16];
	size_t len = sizeof(name) - 1;

	if (sysctlbyname("dev.bcmbt.0.power", NULL, NULL, &cycle, sizeof(cycle)) != 0)
		return (0);
	usleep(200000);
	memset(name, 0, sizeof(name));
	if (sysctlbyname("dev.bcmbt.0.tty", name, &len, NULL, 0) == 0 && name[0])
		snprintf(tty, ttylen, "/dev/%s", name);
	return (1);
}

/*
 * purpose:     Create an ng_h4 node named `name` and bind it to the tty.
 * input:       fd -- the open tty (this process keeps it); name -- node name
 * output:      none (exits on error)
 * sideEffects: new netgraph node; the tty is hooked by ng_h4 from here on
 */
static void
ng_attach(int fd, const char *name)
{
	struct ngm_mkpeer mp;
	struct ngm_name nm;
	struct ngm_rmhook rh;
	ng_h4_node_set_tty_ep tty = { .pid = getpid(), .fd = fd };
	char path[64];
	int cs, ds;

	if (NgMkSockNode(NULL, &cs, &ds) < 0)
		err(1, "NgMkSockNode");
	memset(&mp, 0, sizeof(mp));
	strlcpy(mp.type, NG_H4_NODE_TYPE, sizeof(mp.type));
	strlcpy(mp.ourhook, "tmp", sizeof(mp.ourhook));
	strlcpy(mp.peerhook, NG_H4_HOOK, sizeof(mp.peerhook));
	if (NgSendMsg(cs, ".", NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0)
		err(1, "mkpeer h4 (is ng_h4 loaded?)");
	memset(&nm, 0, sizeof(nm));
	strlcpy(nm.name, name, sizeof(nm.name));
	if (NgSendMsg(cs, "tmp", NGM_GENERIC_COOKIE, NGM_NAME, &nm, sizeof(nm)) < 0)
		err(1, "name %s", name);
	snprintf(path, sizeof(path), "%s:", name);
	if (NgSendMsg(cs, path, NGM_H4_COOKIE, NGM_H4_NODE_SET_TTY, &tty, sizeof(tty)) < 0)
		err(1, "SET_TTY");
	memset(&rh, 0, sizeof(rh));
	strlcpy(rh.ourhook, "tmp", sizeof(rh.ourhook));
	(void)NgSendMsg(cs, ".", NGM_GENERIC_COOKIE, NGM_RMHOOK, &rh, sizeof(rh));
	close(cs);
	close(ds);
}

/*
 * purpose:     Build the hci/l2cap/btsocket graph /etc/rc.d/bluetooth builds for ubt devices.
 * input:       d -- the h4 node name
 * output:      none (exits on error)
 * sideEffects: creates <d>hci and <d>l2cap nodes and btsocket hooks via ngctl
 */
static void
ng_graph(const char *d)
{
	char cmd[1024];

	/* the same graph /etc/rc.d/bluetooth builds for a ubt device */
	snprintf(cmd, sizeof(cmd),
	    "ngctl mkpeer %1$s: hci hook drv && "
	    "ngctl name %1$s:hook %1$shci && "
	    "ngctl mkpeer %1$shci: l2cap acl hci && "
	    "ngctl name %1$shci:acl %1$sl2cap && "
	    "ngctl connect %1$shci: btsock_hci_raw: raw %1$sraw && "
	    "ngctl connect %1$sl2cap: btsock_l2c_raw: ctl %1$sctl && "
	    "ngctl connect %1$sl2cap: btsock_l2c: l2c %1$sl2c", d);
	if (system(cmd) != 0)
		errx(1, "netgraph setup failed (ng_hci, ng_l2cap, ng_btsocket loaded?)");
}

/*
 * purpose:     Print usage and exit.
 * input:       none
 * output:      does not return
 * sideEffects: stderr
 */
static void
usage(void)
{
	fprintf(stderr, "usage: bcmbtattach [-v] [-F] [-N] [-d tty] [-g gpioc] [-p pin] "
	    "[-w wake-pin|-1] [-n name] -f patchram.hcd\n");
	exit(1);
}

/*
 * purpose:     Power up, load firmware, attach to the stack, then hold the tty.
 * input:       command line, see usage()
 * output:      exit status 1 on failure; on success never returns (daemon)
 * sideEffects: GPIO, tty, netgraph; daemonizes unless -F
 */
int
main(int argc, char **argv)
{
	const char *tty = NULL, *gpioc = NULL, *fw = NULL, *name = "h4bt0";
	char drvtty[32] = "/dev/cuau1";
	int pin = 4, wake = 6, fg = 0, ch, fd;

	while ((ch = getopt(argc, argv, "vFNd:g:p:w:n:f:")) != -1) {
		switch (ch) {
		case 'v': verbose = 1; break;
		case 'F': fg = 1; break;
		case 'N': noflow = 1; break;
		case 'd': tty = optarg; break;
		case 'g': gpioc = optarg; break;
		case 'p': pin = atoi(optarg); break;
		case 'w': wake = atoi(optarg); break;
		case 'n': name = optarg; break;
		case 'f': fw = optarg; break;
		default: usage();
		}
	}
	if (fw == NULL)
		usage();

	/* The kernel driver (bcmbt) owns power, the LPO clock and the supplies;
	 * raw GPIO is only for a system without it, and only when asked. */
	if (gpioc != NULL)
		power_cycle(gpioc, pin, wake);
	else if (!driver_power_cycle(drvtty, sizeof(drvtty)))
		errx(1, "no bcmbt driver (kldload bcmbt), and no -g gpioc given");
	if (tty == NULL)
		tty = drvtty;
	if ((fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)
		err(1, "%s", tty);
	settty(fd, B115200);
	if (hci_cmd(fd, OP_RESET, NULL, 0, NULL, NULL, 1000) != 0)
		errx(1, "no answer to HCI_Reset on %s", tty);
	report(fd, "rom");
	patchram(fd, fw);
	{
		int i, st = -1;

		for (i = 0; i < 5 && st != 0; i++) {
			tcflush(fd, TCIOFLUSH);
			st = hci_cmd(fd, OP_RESET, NULL, 0, NULL, NULL, 1000);
			if (verbose)
				fprintf(stderr, "post-patchram HCI_Reset try %d: %d\n", i, st);
		}
		if (st != 0)
			errx(1, "no answer to HCI_Reset after the patchram");
	}
	report(fd, "patched");

	ng_attach(fd, name);
	ng_graph(name);
	printf("attached: %s -> %shci -> %sl2cap\n", name, name, name);
	fflush(stdout);

	if (!fg && daemon(0, 0) != 0)
		err(1, "daemon");
	for (;;)
		pause();        /* the tty stays hooked while this holds it open */
}
