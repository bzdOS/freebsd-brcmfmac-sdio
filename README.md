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
