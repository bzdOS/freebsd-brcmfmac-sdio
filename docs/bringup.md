# Bring-up

## Building

The driver and the 802.11 compat layer are separate modules; the kernel build
does not cover either.

```sh
# kernel (contains linux_netdev.c, sdiob.c, aw_mmc.c)
make TARGET=arm64 TARGET_ARCH=aarch64 KERNCONF=<conf> buildkernel

# brcmutil.ko + if_brcmfmac.ko
cd sys/modules/brcm80211 && make

# linuxkpi_wlan.ko  (contains linux_80211.c)
cd sys/modules/linuxkpi_wlan && make
```

**`linux_80211.c` is not part of the kernel build.** It lives in
`linuxkpi_wlan.ko`, which is pulled in automatically by `MODULE_DEPEND` when
`if_brcmfmac.ko` is loaded — and is therefore invisible to
`kldstat | grep brcm`. Editing that file and rebuilding the kernel is a silent
no-op that reports success.

A stock `/boot/kernel/linuxkpi_wlan.ko` also shadows a freshly built one in
`/boot/modules/` during dependency resolution, because the auto-loaded
dependency is resolved by name rather than by the path given to `kldload`.
Install to **both** locations.

## Connecting

```sh
kldload /boot/modules/brcmutil.ko
kldload /boot/modules/if_brcmfmac.ko
ifconfig wlan0 up

sysctl compat.linuxkpi.80211.connect_ssid=<SSID>
sysctl compat.linuxkpi.80211.connect_pmk=$(tools/wpa_psk.py <SSID>)
sysctl compat.linuxkpi.80211.connect_privacy=1
sysctl compat.linuxkpi.80211.connect_go=wlan0
```

A successful join logs:

```
cfg80211_connect_done: wlan0 status=0
wlan0: link state changed to UP
```

`connect_pmk` takes the derived 256-bit key, not a passphrase.
`tools/wpa_psk.py` does the PBKDF2-HMAC-SHA1 derivation (4096 rounds, SSID as
salt) offline, which is what wpa_supplicant does before handing a PMK to the
Linux kernel for PSK offload. The passphrase never has to enter a sysctl, a
shell history, or a log.

## Firmware

`/boot/firmware/` needs `brcmfmac43430-sdio.bin`, `.txt` (NVRAM) and
`.clm_blob`. Note that the driver looks under `brcm/` first and falls back;
the fallback path is the one that works here.

## Reading a failed association

The firmware event sequence for a good join is:

```
E_LINK      flags=1        link up
E_SET_SSID  status=0       join accepted
E_PSK_SUP   status=6       WLC_SUP_KEYED — 4-way handshake done in firmware
```

`E_PSK_SUP` reaching 6 is the one that matters: it means the PSK was correct
and keys are installed. If association looks successful but no traffic flows,
that event distinguishes "wrong key" from "the host is dropping frames".
