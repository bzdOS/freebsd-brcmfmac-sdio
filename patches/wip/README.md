# Work in progress

Apply **after** the whole `../upstream/` series; these diffs are generated on
top of it.

| File | What |
| --- | --- |
| `netdev-ifnet-bridge.diff` | `struct net_device` backed by a real `IFT_ETHER` ifnet: `register_netdev`, `netif_rx`, transmit, ioctl, carrier and queue state. |
| `cfg80211-connect-path.diff` | The `compat.linuxkpi.80211.connect_*` sysctls and real `cfg80211_connect_done()` / `cfg80211_disconnected()`. |
| `driver-and-groundwork.diff` | brcmfmac/brcmutil FreeBSD adaptations, plus earlier OF/device groundwork this work sits on. |
| `linux_mmc_bsd.c.new`, `clk.h.new` | New files (not diffs); drop into `sys/compat/linuxkpi/common/src/` and `.../include/linux/`. |

See `../../docs/upstreaming.md` for why these are held back.
