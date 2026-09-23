# Root causes

Every blocker between a silent card and a working link, in the order each one
stopped being the thing in the way. All were found on hardware; none were in
brcmfmac's own logic.

## 1. `sdio_readl()`/`sdio_writel()` emulated 32-bit access with four CMD52s

A Broadcom SDIO device encodes a 32-bit backplane access by setting
`SBSDIO_SB_ACCESS_2_4B_FLAG` in the function-1 address, and then expects a
single 32-bit CMD53. Four byte-wide CMD52s are a protocol violation Linux
never emits. The chip stopped answering **every** subsequent command, function
0 included.

This presented as `AW_MMC_INT_RESP_TIMEOUT` at addresses that looked
unrelated and shifted between runs (`0x1000a`, `0xc044`, `0x2`, `0x110`).
Because the symptom looks exactly like marginal timing, a great deal of effort was spent on CMD52 retry counts, inter-attempt delays, power-on settle time and
bus clock speed. **None of it moved the needle, because none of it was the
problem.** If a symptom is completely insensitive to four independent timing
parameters, stop tuning and go looking for a protocol error.

## 2. `sdio_readsb()`/`sdio_writesb()` used the wrong CMD53 form

In Linux these are the fixed-address (FIFO) variants. They were forwarded to
the incrementing `memcpy` helpers, so reads walked the function's address
space instead of draining its FIFO. brcmfmac uses `sdio_readsb()` for every
frame it receives.

## 3. `SDIO_*_EXTENDED()` dispatched on the wrong device

All extended (CMD53) calls in the shim used `func->bsd_func->dev`, the child,
rather than its parent bridge, so the kobj method never arrived. The direct
CMD52 path in `sdio_subr.c` already did this correctly, which is what made the
discrepancy findable.

## 4. `aw_mmc` armed auto-CMD12 for SDIO CMD53

The controller's automatic stop command was enabled for anything carrying
`MMC_DATA_MULTI`. Multi-block SD/eMMC transfers do issue
`STOP_TRANSMISSION`; SDIO block-mode CMD53 sets the same flag and has no stop
command, and an SDIO card never answers CMD12.

Every successful multi-block CMD53 therefore still completed with
`rint = DATA_OVER|AUTO_STOP_DONE|RESP_TIMEOUT`, making the 419 KB firmware
download impossible. This is a stock FreeBSD bug affecting all Allwinner SDIO,
independent of WiFi.

## 5. The shared CCB in `sdiob.c` was not serialised

Every transaction shares one `sc->ccb`, and `cam_periph_runccb()` drops the
periph mutex while it sleeps. A second caller re-issued the CCB that was still
in flight:

```
panic: camq_remove: Attempt to remove out-of-bounds index -3 from queue ... of size 1
```

## 6. The CCCR interrupt poller shared `taskqueue_thread`

That queue has a single thread, and the firmware-ready callback blocks on it
for seconds waiting for a reply that only the poller can notice. Deadlock by
construction, reported as `brcmf_sdio_bus_rxctl: resumed on timeout` and
`dongle is not responding: -60`. The poller now has its own thread, the
analogue of Linux's `sdio_irq_thread`.

## 7. `alloc_netdev()` did not zero the driver private area

`linuxkpi_alloc_netdev()` allocates `sizeof(*ndev) + len` and then memsets
only `sizeof(*ndev)`. The trailing bytes — exactly what `netdev_priv()`
returns — were never initialised. Linux uses `kvzalloc()` for the whole
allocation and drivers depend on it.

`struct brcmf_if::fws_desc` is assigned only when `brcmf_fws_add_interface()`
runs to completion. With recycled heap there instead of `NULL` (memory that
happened to contain instruction bytes: `x19 = 0xf940126017fffff4`), the `NULL`
check passed and the following store faulted. It only surfaced once events
started being delivered, which is to say immediately after bug 8 was fixed.

## 8. `eth_type_trans()` was a stub returning a constant

It ignored the frame and returned a fixed `ETHERTYPE_8023`, so `skb->protocol`
was always wrong and the Ethernet header was never pulled.

`brcmf_fweh_process_skb()` begins with

```c
if (skb->protocol != cpu_to_be16(ETH_P_LINK_CTL))
        return;
```

so **every firmware event was silently discarded**. This is why association
never completed and no completion callback ever ran, while the SDIO trace
plainly showed function-2 frames being read off the chip. Diagnosing it needed
the observation that RX at the bus level was healthy and the loss had to be
above it.

## 9. `skb_header_cloned()` returned `true` unconditionally

Together with `pskb_expand_head()` — still a stub returning `-ENXIO` — this
made brcmfmac's transmit path drop every frame:

```c
if (skb_headroom(skb) < drvr->hdrlen || skb_header_cloned(skb))
        ret = pskb_expand_head(...);   /* always -ENXIO */
```

Visible only as an incremented `tx_dropped`. This KPI has no `skb_clone()` at
all, so `false` is the truthful answer.

## 10. `eth_hw_addr_set()` was a no-op

`dev->dev_addr` stayed zeroed, so the interface had no MAC address.

## 11. There was no netdev→ifnet bridge at all

`register_netdev()`, `netif_rx()`, the carrier helpers and the queue helpers
were all `pr_debug("TODO")` and returned success. A fullmac driver's probe
therefore "succeeded" and produced no interface anybody could see, with no
error logged.

Built one: plain `IFT_ETHER` plus `ether_ifattach()`, skb↔mbuf copy,
`if_input()`, carrier through `if_link_state_change()`. Plain Ethernet is the
correct shape here — a fullmac part presents an already-Ethernet-framed
netdev, and net80211 is not involved.

`IFF_DRV_RUNNING` has to be maintained around `ndo_open`/`ndo_stop`: without
it `ether_output()` answers `ENETDOWN` and nothing is ever handed to the
driver. That produced `ping: sendto: Network is down` with the interface
showing `UP`.

## 12. Anything touching `net/if.c` needs `CURVNET_SET()`

The thread that registers the netdev is a driver workqueue thread with no
`curvnet`, and VIMAGE is enabled. `if_addgroup()` dereferences `curvnet`
unconditionally, so the first `ether_ifattach()` panicked with `FAR = 0x28`.

## A trap introduced while fixing the above

The transmit path in the bridge called `eth_type_trans()` purely to fill in
`skb->protocol`. That is harmless while the function is a stub. Once it is
implemented correctly it also pulls the Ethernet header, on transmit, where
the header must stay, and every outgoing frame goes out 14 bytes short. The
tell was `len=84` for a 98-byte ping and `len=28` for a 42-byte ARP.

`eth_type_trans()` is receive-side only. On transmit, read `h_proto` out of
the header without pulling it.
