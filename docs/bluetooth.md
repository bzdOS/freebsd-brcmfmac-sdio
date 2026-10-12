# Bluetooth: AP6212 (BCM43438A1) on uart1

The AP6212 is a WiFi + Bluetooth combo module. The WiFi half sits on SDIO
(the rest of this repository); the Bluetooth half is a separate HCI
controller on the A64's `uart1`, with RTS/CTS. FreeBSD had no way to use it:
there is no UART Bluetooth transport since `ng_h4` was removed in
79a100e28e3c (2021), only USB `ubt(4)`, and nothing claims the
`brcm,bcm43438-bt` devicetree node. This adds both, plus the userland piece
that loads the controller's firmware.

## Pieces

| | Where | Does |
| --- | --- | --- |
| `ng_h4(4)` | `patches/bluetooth/0001` | Netgraph node: H4 transport between a tty and `ng_hci(4)`'s `drv` hook. A revival of the removed node, rewritten on the MPSAFE tty hook API (`ttyhook_register(9)`, as `ng_tty(4)` uses) instead of the old line discipline. Module + man page. |
| `aw_rtc` fanout | `patches/bluetooth/0002` | Registers the RTC's gated 32 kHz output (`<&rtc 1>`, X32KFOUT) as a clock. Upstream registers ids 0 and 2 only, so the DT's `lpo` clock cannot resolve. The same clock is the WiFi pwrseq's `ext_clock`. |
| `bcmbt(4)` | `patches/bluetooth/0003` | Claims `brcm,bcm43438-bt` and relatives under a UART node: enables the `lpo` clock, `vbat`/`vddio` supplies, `shutdown-gpios` (BT_REG_ON) and `device-wakeup-gpios`. Exposes `dev.bcmbt.N.power` (0 off, 1 on, 2 power-cycle) and `dev.bcmbt.N.tty`. Module + man page. |
| `bcmbtattach` | `bluetooth/bcmbtattach/` | Userland, the same split as `iwmbtfw(8)` for USB: power-cycles through bcmbt, HCI_Reset, loads the `.hcd` patchram (vendor Download_Minidriver 0xfc2e, then each record as an HCI command), resets again, binds the tty to an `ng_h4` node, builds the hci/l2cap/btsocket graph, then holds the tty open. |
| `rc.d/bcmbt` | `bluetooth/rc.d/` | Loads the modules, runs `bcmbtattach`, initialises `ng_hci` (what `/etc/rc.d/bluetooth` does for `ubt` devices, which is all it knows about). |

No devicetree change is needed: the stock `sun50i-a64-bananapi-m64.dts`
already describes the controller under `&uart1` (lpo = `<&rtc 1>`, vbat
`dldo2`, vddio `dldo4`, BT_REG_ON PL4, device-wakeup PL6, host-wakeup PL5).

## Building

Apply the series to a FreeBSD source tree:

```sh
cd /usr/src
for p in /path/to/patches/bluetooth/0*.patch; do git am "$p"; done   # or git apply
```

`0002` changes `aw_rtc`, which is part of the kernel, so rebuild and install
the kernel. `ng_h4.ko` and `bcmbt.ko` are hooked into the module build
(`sys/modules/netgraph/bluetooth/Makefile`, and `sys/modules/Makefile` for
FDT platforms) and come with `buildkernel`; on their own:

```sh
make -C sys/modules/netgraph/bluetooth/h4
make -C sys/modules/bcmbt
```

`bcmbtattach` builds on the target (or with a cross toolchain) from its own
directory with `make`. It carries a copy of `ng_h4.h` from patch `0001`, so it
does not need the patched headers installed in `/usr/include`.

## Firmware

The controller boots from ROM with no usable radio firmware; it needs a
Broadcom patchram (`.hcd`) at every power-up. **The patchram must match the
module, not just the chip.** The Raspberry Pi `BCM43430A1.hcd` (same
BCM43430A1, LMP subversion 0x2209) loads all its records and then the
controller never answers again: it is built for a different reference clock.
The AP6212 one reports itself as `BCM43438A1 26MHz AP6212A1_CL1` and is
`ap6212/bcm43438a1.hcd` in the Armbian firmware repository
(<https://github.com/armbian/firmware>), 33376 bytes, MD5
`a2d5ce5416f432ae75c48f1758c52004`.

It is not included here: Broadcom firmware is not ours to redistribute.

## Installing and using

```sh
install -m 555 bcmbt.ko ng_h4.ko /boot/modules/ && kldxref /boot/modules
install -m 555 bcmbtattach /usr/local/sbin/
install -m 555 rc.d/bcmbt /usr/local/etc/rc.d/
install -m 444 bcm43438a1.hcd /boot/firmware/bcm43438a1-ap6212.hcd
sysrc bcmbt_enable=YES
service bcmbt start
```

`rc.conf` knobs: `bcmbt_firmware` (default
`/boot/firmware/bcm43438a1-ap6212.hcd`), `bcmbt_node` (`h4bt0`), `bcmbt_scan`
(`write_scan_enable` value, default 3: inquiry and page scan),
`bcmbt_flags` (extra `bcmbtattach` flags, e.g. `-d /dev/cuau2`).

By hand:

```sh
kldload bcmbt ng_h4 ng_hci ng_l2cap ng_btsocket
bcmbtattach -n h4bt0 -f /boot/firmware/bcm43438a1-ap6212.hcd
hccontrol -n h4bt0hci reset
hccontrol -n h4bt0hci read_bd_addr
hccontrol -n h4bt0hci read_local_supported_features
hccontrol -n h4bt0hci read_buffer_size
ngctl msg h4bt0hci: init          # only after read_bd_addr, see below
hccontrol -n h4bt0hci inquiry
hccontrol -n h4bt0hci remote_name_request <bdaddr>
l2ping -a <bdaddr>
```

`bcmbtattach` flags: `-f` patchram (required), `-n` node name, `-d` tty
(default: what `dev.bcmbt.0.tty` says, else `/dev/cuau1`), `-F` stay in the
foreground, `-N` no RTS/CTS, `-v` verbose. Without `bcmbt(4)` it can drive
BT_REG_ON itself with `-g /dev/gpiocN -p <pin> [-w <wake pin>|-1]`; the clock
and supplies then have to be on already.

## Status

Tested on a Banana Pi M64, FreeBSD 15.1 arm64 (running as a guest of the
bzdk hypervisor with `uart1` passed through; nothing here depends on that):

- `bcmbt` attaches at boot, enables the 32 kHz fanout and the supplies; the
  controller answers HCI_Reset from ROM, takes the patchram and answers
  again from RAM.
- `hccontrol` reset, `read_bd_addr`, `read_local_supported_features`,
  `read_buffer_size`, `write_scan_enable` work over `ng_h4`.
- `inquiry` finds a laptop, `remote_name_request` returns its name, and
  `l2ping` gets 5/5 echo replies (12-34 ms) over an ACL link.
- The whole sequence runs at boot from `rc.d/bcmbt`.

Not done or not verified:

- **The link stays at 115200 baud.** The vendor Update_UART_Baud_Rate
  (0xfc18) switch is not implemented, so throughput is limited; fine for
  HCI control and L2CAP, not for audio or bulk transfer.
- Nothing above L2CAP echo has been verified: pairing (`hcsecd`), `sdpd`,
  RFCOMM, HID, SCO audio, Bluetooth LE.
- `host-wakeup-gpios` is not used and the controller's UART low-power mode
  is never entered; `device-wakeup` is simply held asserted.
- `ng_h4` has one transmit queue (no separate command/ACL/SCO priority as in
  `ng_ubt`), and its control messages have no ASCII form for `ngctl`; they
  are sent in binary (`bcmbtattach` does this).
- The receive path that grows a frame into an mbuf chain (ACL frames larger
  than a cluster) has not been exercised with oversized frames, and detaching
  while the tty disappears underneath the node has not been tested.
- `bcmbtattach` only looks at `dev.bcmbt.0`; one controller per system.
- Not tested on any other board or Broadcom UART controller, and the
  `bcmbtattach(8)` page referenced by the man pages does not exist yet.
- `ng_h4` and `bcmbt` have not been submitted to FreeBSD. The series applies
  unmodified to 15.1 (releng/15.1, 15.1-RELEASE-p1) and to `main` as of
  2026-10-04.

## Things that cost time

- **Wrong patchram** (see above): the Raspberry Pi file is accepted record
  by record and then the controller goes silent.
- **No 32 kHz LPO** without the `aw_rtc` change: the controller's firmware
  does not answer without it. The gate bit is in the always-on RTC
  domain and survives a warm reset, so a board can appear to work after a
  reboot from an image that never enables it.
- **`ngctl msg <node>hci: init` must come after `read_bd_addr`.** Otherwise
  `ng_hci` stays in state 0x1 and L2CAP returns `ENXIO`.
- **FreeBSD's `timeout(1)` kills its descendants.** Starting `bcmbtattach`
  under it kills the daemonised process on exit; the tty closes and the
  node loses its link.
