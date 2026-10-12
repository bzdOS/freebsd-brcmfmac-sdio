# WiFi on the Banana Pi M64 under FreeBSD (brcmfmac / BCM43430 SDIO)

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
| `0004` | Access point mode (`wlanmode hostap`, WPA2-PSK/CCMP with the firmware authenticator); `ifconfig wpakey` applied immediately so an AP never comes up open. The BSS starts and stations associate; a full WPA2 client handshake through the firmware authenticator is not confirmed yet. |
| `0005` | The boot never waits on WiFi: chip bring-up and firmware download run in a task (attach waits at most `hw.bwfm.attach_wait`, 10 s), every bus wait is bounded, a card that stops answering is declared dead instead of being polled for minutes, and a chip left running by a warm reset is quiesced and woken first. Verified over six warm reboots and forced guest panics with the firmware running. |

Proven on the board: attach and firmware boot, scan, WPA2-PSK join through
the firmware supplicant, DHCP, ping over wlan0, down/up reconnect, wrong-key
detection, and the rc.conf path (devd -> `wlans_bwfm0` -> `ifconfig_wlan0`).
Firmware download takes about 18 s, because the MMCCAM SDIO bus stays at its
enumeration clock; the boot is not held up by it.
Not yet: the host-supplicant path (`wpa_supplicant -D bsd` gets message 1 of
the handshake but the access point never accepts message 2), hostap mode.

## Hardware and software

| | |
| --- | --- |
| Board | Banana Pi M64 (Allwinner A64, arm64) |
| WiFi | AP6212 module, Broadcom BCM43430/1, SDIO on `mmc1` |
| OS | FreeBSD 15.1-RC3 aarch64, `options MMCCAM` |
| Firmware | `brcmfmac43430-sdio.bin`, version 7.45.98.118 (Cypress) |

## Contents

| Path | |
| --- | --- |
| `patches/upstream/` | Six patches against stock FreeBSD. Each fixes a defect that exists independently of this hardware, is still present in upstream `main`, and applies there unmodified. |
| `patches/native-bwfm/` | Native bwfm(4) driver (no LinuxKPI), net80211 PSK ioctl, `ifconfig wpakey`. See below. |
| `patches/wip/` | The netdev↔ifnet bridge, the connect control path, and fixes to this project's own SDIO shim. Needed to make it work; not in a shape to send upstream. |
| `docs/root-causes.md` | Every blocker, with the evidence that identified it. |
| `docs/bringup.md` | Build, install and connect. |
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
