# udp_link counter stream

A bring-up and test app for the udp_link component. Each endpoint sends an incrementing counter to the other and checks everything it receives. It needs no peripheral, so it is the first thing to run on a new board, a new ESP-IDF release or a new configuration.

Each endpoint runs one TX task and one RX task on the **same** udp_link instance, so both directions run at once. That also exercises udp_link's full-duplex guarantee.

## Modes

| Mode | Boards | Network | Use it for |
| --- | --- | --- | --- |
| **Loopback** (default) | 1 | two instances talk via `127.0.0.1` | first bring-up; a strict PASS/FAIL check of udp_link |
| **Two boards** | 2 | Wi-Fi station on the same access point | real link behavior: loss, reboots, disconnects |

## Requirements

- ESP-IDF **6.0.x**. udp_link's build checks and its manifest reject other releases.
- The udp_link component folder must be named `udp_link`, because ESP-IDF names a component after its folder. The example finds it two levels up (`../..`, set in this folder's `CMakeLists.txt`).
- Two-board mode only: two boards and a 2.4 GHz access point. Both board addresses must stay fixed for the whole run, through static IPs or DHCP reservations.

`sdkconfig.defaults` already sets what udp_link needs:

```
CONFIG_LWIP_CHECKSUM_CHECK_UDP=y   # required; the build fails without it
CONFIG_LWIP_NETIF_LOOPBACK=y       # loopback mode
CONFIG_VFS_SUPPORT_SELECT=y        # udp_link's default receive wait
CONFIG_FREERTOS_HZ=1000
```

If you built this project before adding those defaults, run `idf.py fullclean` so they take effect.

## Run: loopback (one board)

```sh
cd examples/counter_stream
idf.py set-target esp32s3          # your chip
idf.py build flash monitor
```

Every second the app prints both endpoints' counters and a verdict:

```
I (5012) udp_link_counter: A tx ok=500 drop=0 err=0 | rx ok=500 gap=0 late=0 bad=0 rej=0 restart=0 nomem=0
I (5012) udp_link_counter: B tx ok=500 drop=0 err=0 | rx ok=499 gap=0 late=0 bad=0 rej=0 restart=0 nomem=0
I (5012) udp_link_counter: report 5: PASS (inflight A->B 1, B->A 0; failed reports so far 0)
```

**Pass criterion:** every report says `PASS`, and `failed reports so far` stays at 0. Leave it running for several minutes, since a soak catches what a short run misses.

## Run: two boards

1. On each board, run `idf.py menuconfig` and set:
   - **udp_link counter stream**: untick *Loopback mode*, then set *Peer board IPv4 address* to the **other** board's address. Leave the port the same on both boards.
   - **Example Connection Configuration**: your Wi-Fi SSID and password.
2. Build and flash each board with its own configuration:

   ```sh
   idf.py -B build_a build flash monitor -p PORT_OF_BOARD_A     # peer IP = board B
   idf.py -B build_b build flash monitor -p PORT_OF_BOARD_B     # peer IP = board A
   ```

   Using separate build folders (`-B`) keeps the two configurations apart. You can also reconfigure and rebuild between flashes.

Each board prints its own counters. At startup it also prints the Wi-Fi power-save mode actually in effect: the app asks for `none`, but the driver may refuse, for example when Bluetooth is enabled too. In two-board mode there is no automatic PASS/FAIL, because one board cannot see the other's counters. Read the values against the table below.

## Reading the counters

| Counter | Meaning | Loopback | Two boards |
| --- | --- | --- | --- |
| `tx ok` | sends the stack accepted | +100 per second at the default 10 ms period | same |
| `tx drop` | `udp_link_send` returned `NO_MEM` or `NO_ROUTE`; the same counter is resent next period | 0 | nonzero while the link is down or overloaded |
| `tx err` | any other send result | **0** | **0**, including across Wi-Fi disconnects |
| `rx ok` | datagrams received with an intact payload whose counter equals `seq` | at most the peer's `tx ok` + 1 | slightly below the peer's `tx ok` |
| `rx gap` | sequence numbers skipped (lost, or still to arrive late) | 0 | small; grows with radio loss or a starved RX task |
| `rx late` | datagrams older than expected | 0 | rare |
| `rx bad` | payload pattern broken, or counter ≠ `seq` | **0** | **0** |
| `rx rej` | `udp_link_recv` rejected the datagram (malformed, wrong version or stream, truncated, other socket error) | **0** | **0** |
| `rx restart` | the sender's session changed | 0 | +1 each time the peer reboots |
| `rx nomem` | the receive wait ran out of memory; nothing was lost, and the task called again | 0 | 0 unless the heap is exhausted |

The counters in **bold** must be 0 in every mode. A nonzero value is a defect, in udp_link or in the configuration. In particular, a `tx err` that appears only while Wi-Fi drops or reconnects means the port reported an errno that udp_link's error mapping doesn't cover.

### What the loopback verdict checks

A report is `FAIL` if any of these happen:

- a bold counter is nonzero;
- `drop`, `gap`, `late` or `restart` is nonzero, since loopback has no radio and should lose nothing;
- in either direction, the receiver's `rx ok` exceeds the sender's `tx ok` + 1;
- an endpoint made more or fewer send attempts than one per period, with a tolerance of ±1 per report.

The receiver can briefly be one ahead because the sender counts a datagram only *after* `udp_link_send` returns. The higher-priority RX task can receive and count it first. The reporter reads each direction in the order sender, receiver, sender, so the `+1` bound is exact.

`inflight` is how many datagrams were sent but not yet counted at the moment of the report. It is shown for information only. It is normally 0 to 2; a value that keeps growing means the RX side is falling behind.

## Quick checks in two-board mode

- **Reboot one board.** The other board's `restart` goes up by 1, and its `gap` does not jump.
- **Switch the access point off and on.** Each sender's `drop` rises while the link is down and stops rising once it is back. `tx err` stays 0, and both directions carry data again **without** reopening anything.
- **Move a board far from the access point.** `gap` rises, while `bad` and `rej` stay 0.

## Stress

In menuconfig, set *Send period* to 1 ms (one tick at 1000 Hz) and *Payload length* towards 1460. Then:

- `tx drop` shows where the stack starts refusing datagrams;
- `rx gap` shows where the socket's receive mailbox overflows (`CONFIG_LWIP_UDP_RECVMBOX_SIZE`).

In loopback, nonzero values here mean the verdict will report `FAIL`. That is expected under stress, so read the counters rather than the verdict.

Each task keeps a payload-sized buffer on its stack, and the stack size is set to 4096 + payload length. After raising the payload length, check the tasks' high-water marks with `uxTaskGetStackHighWaterMark()`.

## Comparing udp_link's two receive waits

udp_link waits for data with `select()` by default. To build the `SO_RCVTIMEO` variant instead:

```sh
idf.py -DUDP_LINK_WAIT_SO_RCVTIMEO=1 fullclean build flash monitor
```

Run both builds for the same duration and compare their counters, CPU load and free heap. Only one internal function in `udp_link.c` differs between the two builds.

## Troubleshooting

| Symptom | Cause |
| --- | --- |
| `#error "udp_link: verified on ESP-IDF 6.0.x only"` | Wrong ESP-IDF release. |
| `#error "udp_link: requires UDP checksums..."` | `CONFIG_LWIP_CHECKSUM_CHECK_UDP` is off. Run `idf.py fullclean`, or enable it in menuconfig (Component config → LWIP → Checksums). |
| `Failed to resolve component 'udp_link'` | The component folder is not named `udp_link`, or it is not at `../..` relative to this folder. |
| `udp_link_open` fails at startup (`ESP_ERROR_CHECK` abort) | `ESP_ERR_INVALID_STATE`: the port is already in use. `ESP_ERR_NO_MEM`: out of sockets (`CONFIG_LWIP_MAX_SOCKETS`) or heap. |
| Two boards: `tx ok` rises but the peer's `rx ok` stays 0 | Peer IP or port mismatch, a changed DHCP address, AP client isolation, or a `max_payload` too large for the path. None of these produce an error on either side. |
| Two boards: bursty `gap` | Wi-Fi power save is active; check the mode logged at startup. |

## Files

| File | What |
| --- | --- |
| `main/counter_stream.c` | the app: TX and RX tasks, payload pattern, reporter |
| `main/Kconfig.projbuild` | menuconfig options (mode, peer, port, payload, period) |
| `sdkconfig.defaults` | settings udp_link and loopback mode need |
| `CMakeLists.txt` | finds udp_link at `../..` and Wi-Fi bring-up in `protocol_examples_common` |
