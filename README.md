# WiFi and Bluetooth on the Banana Pi M64 under FreeBSD (AP6212 / BCM43430)

**The onboard WiFi of the Banana Pi M64 works under FreeBSD.** The AP6212
module (Broadcom BCM43430, SDIO) associates to a WPA2-PSK access point and
passes traffic, on FreeBSD 15.1 / arm64 with an Allwinner A64.

At the time of writing this did not work out of the box. Getting there needed
six fixes to FreeBSD's own SDIO stack and LinuxKPI compatibility layer, none
of which are specific to this board, plus a netdev-to-ifnet bridge that did
not exist. The patches and the analysis are here.

```
cfg80211_connect_done: wlan0 status=0
wlan0: link state changed to UP

3 packets transmitted, 3 packets received, 0.0% packet loss
round-trip min/avg/max/stddev = 37.449/56.469/89.951/23.748 ms
```

Confirmed from the wired side of the same LAN, so the frames really do cross
the air and the access point:

```
# tcpdump -i br0 -n -e ether host <wlan0 mac>
<wlan0 mac> > Broadcast, ethertype ARP (0x0806), length 60:
    Request who-has 192.168.1.1 tell 192.168.1.201
```

## Native driver: bwfm(4), no LinuxKPI

The LinuxKPI/brcmfmac route above proved the hardware. The same chip now also
works with a **native FreeBSD driver ported from OpenBSD's bwfm(4)** (ISC),
on net80211 and the stock MMCCAM `sdiob(4)` stack, with no Linux
compatibility layer and no wpa_supplicant:

```
# kldload if_bwfm            # bwfm0: chip 43430 rev 1 ... address 08:e9:f6:..
# ifconfig wlan0 create wlandev bwfm0
# ifconfig wlan0 up scan
SSID/MESH ID    BSSID              CHAN RATE    S:N     INT CAPS
MyNetwork        xx:xx:xx:xx:xx:xx    2   54M  -42:-95   100 EP   RSN WPS HTCAP WME
# ifconfig wlan0 ssid MyNetwork wpakey 'pass phrase' up    # firmware 4-way handshake
# dhclient wlan0
```

or in rc.conf (with `kld_list="if_bwfm"`, or hot attach through devd):

```
wlans_bwfm0="wlan0"
ifconfig_wlan0="ssid MyNetwork wpakey MyPassphrase DHCP"   # no spaces in rc.conf
```

`patches/native-bwfm/` (apply in order on FreeBSD 15.1 with `options MMCCAM`):

| Patch | |
| --- | --- |
| `0001` | OpenBSD `bwfm.c`, `bwfmreg.h`, `bwfmvar.h`, `if_bwfm_sdio.[ch]` imported unmodified (openbsd/src f3dc682c, 2026-10-11). |
| `0002` | The FreeBSD port: newbus attachment to `sdiob(4)`, polled card interrupt, firmware(9), net80211 glue (scan offload, firmware join, 802.3 data path); `IEEE80211_IOC_WPAPSK` in net80211 (PSK stored in the vap, taken from `iv_spare`, ABI unchanged); `ifconfig wpakey`/`-wpakey` (PBKDF2 in userland). |
| `0003` | Robustness fixes found on hardware; `bwfm` added to devd's WiFi driver list. |
| `0004` | Access point mode (`wlanmode hostap`, WPA2-PSK/CCMP with the firmware authenticator); `ifconfig wpakey` applied immediately so an AP never comes up open. Confirmed on hardware 2026-10-12: an Android phone joined `bzdos-test` (WPA2-PSK, CCMP, firmware authenticator), completed the handshake, got a DHCP lease from the LAN router through `bridge0` (vtnet0 + wlan0) and reached the internet. On this board bridging needed two fixes in the bzdk hypervisor's virtual Ethernet (bzdk 4b182f5: pass the client's source MAC through; 02db693: EMAC address filter off, FCS stripped). |
| `0005` | The boot never waits on WiFi: chip bring-up and firmware download run in a task (attach waits at most `hw.bwfm.attach_wait`, 10 s), every bus wait is bounded, a card that stops answering is declared dead instead of being polled for minutes, and a chip left running by a warm reset is quiesced and woken first. Verified over six warm reboots and forced guest panics with the firmware running. |

Proven on the board: attach and firmware boot, scan, WPA2-PSK join through
the firmware supplicant, DHCP, ping over wlan0, down/up reconnect, wrong-key
detection, and the rc.conf path (devd -> `wlans_bwfm0` -> `ifconfig_wlan0`).
Firmware download takes about 18 s, because the MMCCAM SDIO bus stays at its
enumeration clock; the boot is not held up by it.
Not yet: the host-supplicant path (`wpa_supplicant -D bsd` gets message 1 of
the handshake but the access point never accepts message 2).

## Bluetooth: bcmbt(4), ng_h4(4), bcmbtattach

The Bluetooth half of the same AP6212 module (BCM43438A1, an HCI controller
on `uart1`) works too: brought up at boot, `hccontrol inquiry` finds a
laptop, `remote_name_request` returns its name, and `l2ping` gets 5/5 replies
over an ACL link. FreeBSD had no UART Bluetooth transport since `ng_h4` was
removed in 79a100e28e3c; `patches/bluetooth/0001` revives it, rewritten on
the MPSAFE tty hook API.

```
# kldload bcmbt ng_h4 ng_hci ng_l2cap ng_btsocket
# bcmbtattach -n h4bt0 -f /boot/firmware/bcm43438a1-ap6212.hcd
# hccontrol -n h4bt0hci inquiry
# l2ping -a <bdaddr>
```

| Piece | |
| --- | --- |
| `patches/bluetooth/0001` | `ng_h4(4)`: H4 UART transport node for `ng_hci(4)`, module and man page. |
| `patches/bluetooth/0002` | `aw_rtc`: register the gated 32 kHz fanout (`<&rtc 1>`), the module's LPO clock. |
| `patches/bluetooth/0003` | `bcmbt(4)`: claims `brcm,bcm43438-bt` under the UART node and powers the controller (LPO clock, supplies, BT_REG_ON). |
| `bluetooth/bcmbtattach/` | Userland: power-cycle, patchram load, bind the tty to `ng_h4`, build the hci/l2cap graph. |
| `bluetooth/rc.d/bcmbt` | Bring-up at boot. |

It needs the module's own patchram, `bcm43438a1.hcd` for the AP6212 (from
Armbian's firmware repository, not included here); the Raspberry Pi
`BCM43430A1.hcd` does not work. The link stays at 115200 baud, and nothing
above L2CAP has been verified. Build, firmware, use and the full list of
what is and is not verified: `docs/bluetooth.md`.

## Hardware and software

| | |
| --- | --- |
| Board | Banana Pi M64 (Allwinner A64, arm64) |
| WiFi | AP6212 module, Broadcom BCM43430/1, SDIO on `mmc1` |
| Bluetooth | same module, BCM43438A1, `uart1` with RTS/CTS |
| OS | FreeBSD 15.1-RC3 aarch64, `options MMCCAM` |
| Firmware | `brcmfmac43430-sdio.bin`, version 7.45.98.118 (Cypress) |

## Contents

| Path | |
| --- | --- |
| `patches/upstream/` | Six patches against stock FreeBSD. Each fixes a defect that exists independently of this hardware, is still present in upstream `main`, and applies there unmodified. |
| `patches/native-bwfm/` | Native bwfm(4) driver (no LinuxKPI), net80211 PSK ioctl, `ifconfig wpakey`. See below. |
| `patches/bluetooth/` | UART Bluetooth: `ng_h4(4)`, `aw_rtc` 32 kHz fanout, `bcmbt(4)`. Apply in order to FreeBSD 15.1 or `main`. |
| `bluetooth/` | `bcmbtattach` (patchram loader and tty binder) and `rc.d/bcmbt`. |
| `patches/wip/` | The netdev↔ifnet bridge, the connect control path, and fixes to this project's own SDIO shim. Needed to make it work; not in a shape to send upstream. |
| `docs/root-causes.md` | Every blocker, with the evidence that identified it. |
| `docs/bringup.md` | Build, install and connect. |
| `docs/bluetooth.md` | Bluetooth: build, firmware, use, status. |
| `tools/wpa_psk.py` | Offline PMK derivation, so a passphrase never has to enter a sysctl. |

## What was actually broken

Nothing in the radio, the firmware, or the brcmfmac driver's own logic. The
obstacles were in the layers underneath, and several were stubs that returned
a plausible-looking constant instead of doing the work, which fails silently
and a long way from the cause.

- **`eth_type_trans()` returned a fixed ethertype without looking at the
  frame.** `brcmf_fweh_process_skb()` drops anything that is not
  `ETH_P_LINK_CTL` as its first test, so **every firmware event was
  discarded**. The SDIO layer was reading them off the chip correctly and
  throwing them away, and association could never complete.
- **`skb_header_cloned()` returned `true` unconditionally** while
  `pskb_expand_head()` is a stub returning `-ENXIO`. brcmfmac's transmit path
  consults both, so **every outgoing frame was dropped**, visible only as an
  incremented `tx_dropped`.
- **`alloc_netdev()` never zeroed the driver's private area**, so
  `netdev_priv()` returned recycled heap. A pointer assigned only on some
  paths read back as garbage that passed a `NULL` check.
- **`aw_mmc(4)` armed the controller's automatic CMD12 for SDIO CMD53**,
  which has no stop command. Every multi-block transfer failed, making
  firmware download impossible. This affects any SDIO device on an Allwinner
  host, not just WiFi.

One diagnostic note worth repeating: the SDIO symptoms looked exactly like
marginal bus timing. Retry counts, inter-attempt delays, power-on settle time
and bus clock were each varied independently and **none of them changed the
symptom at all**. That insensitivity is what eventually pointed at a protocol
error rather than a timing one.

The rest, with traces, is in `docs/root-causes.md`.

## Status of the patches

The six in `patches/upstream/` were each re-checked against upstream `main`:
the defect is still present there and the patch applies unmodified. They have
not been submitted to FreeBSD. `patches/wip/` is deliberately held back, being
new infrastructure rather than fixes.

This bring-up was done with heavy use of an AI assistant. The diagnoses and
fixes were validated on hardware across many test cycles, and the failures
quoted in the documentation are real traces from that work.
