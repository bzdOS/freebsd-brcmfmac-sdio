/*
 * ng_h4.h
 */

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Andrey Bodrov <ap.bodrov@gmail.com>
 * All rights reserved.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * ng_h4(4) is a Netgraph node that turns an open tty (attached via the
 * modern ttyhook_register(9) API, the same mechanism ng_tty(4) uses) into
 * a Bluetooth H4 UART transport.  It reassembles the H4 byte stream into
 * HCI command/event/ACL/SCO frames on receive, and prepends nothing extra
 * on transmit -- frames arriving on its single hook are expected to
 * already carry the H4 packet indicator byte as mtod(m, uint8_t *)[0],
 * exactly the form ng_hci(4) already builds them in before handing them
 * to a driver's "drv"-side peer hook (see ng_hci_ulpi.c).
 *
 * Nothing here is controller-specific: power sequencing, firmware download
 * and baud rate changes are the job of whoever opens the tty (for Broadcom
 * UART controllers, bcmbt(4) and bcmbtattach(8)).
 */

#ifndef _NETGRAPH_NG_H4_H_
#define _NETGRAPH_NG_H4_H_

/* Node type name and type cookie */
#define NG_H4_NODE_TYPE		"h4"
#define NGM_H4_COOKIE		1474837201

/* The node has exactly one hook, connected to ng_hci(4)'s "drv" hook */
#define NG_H4_HOOK		"hook"

/* Debug levels, same scale as NG_UBT_*_LEVEL */
#define NG_H4_ALERT_LEVEL	1
#define NG_H4_ERR_LEVEL		2
#define NG_H4_WARN_LEVEL	3
#define NG_H4_INFO_LEVEL	4

/**************************************************************************
 **************************************************************************
 **                    H4 node command/event parameters
 **************************************************************************
 **************************************************************************/

/*
 * Bind the node to an already-open tty file descriptor belonging to
 * process "pid".  Modeled directly on ng_tty(4)'s NGM_TTY_SET_TTY -- the
 * caller (e.g. a small setup program, or hcsecd/bluetooth-config once
 * taught about this node) opens the UART device, then sends this message
 * naming its own pid and that fd.  May only be done once per node; the
 * node has no way to probe/open the tty itself.
 */
#define NGM_H4_NODE_SET_TTY	1
typedef struct {
	int32_t		pid;
	int32_t		fd;
} ng_h4_node_set_tty_ep;

#define NGM_H4_NODE_SET_DEBUG	2	/* set debug level */
#define NGM_H4_NODE_GET_DEBUG	3	/* get debug level */
typedef u_int16_t		ng_h4_node_debug_ep;

#define NGM_H4_NODE_GET_STAT	4	/* get statistic */
typedef struct {
	u_int32_t	pckts_recv;	/* # of HCI frames from tty */
	u_int32_t	bytes_recv;	/* # of bytes received from tty */
	u_int32_t	pckts_sent;	/* # of HCI frames sent to tty */
	u_int32_t	bytes_sent;	/* # of bytes sent to tty */
	u_int32_t	ierrors;	/* framing / RX allocation errors */
	u_int32_t	oerrors;	/* dropped/rejected outgoing frames */
} ng_h4_node_stat_ep;

#define NGM_H4_NODE_RESET_STAT	5	/* reset statistic */

#endif /* ndef _NETGRAPH_NG_H4_H_ */
