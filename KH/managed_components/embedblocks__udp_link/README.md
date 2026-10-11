# udp_link

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

A point-to-point UDP datagram transport for ESP-IDF. Open a link to one
peer, then send and receive payloads of up to 1460 bytes. Each datagram
carries a small header with a sequence number and a session ID, so the
receiver can see loss, late arrivals and sender restarts. udp_link reports
those facts; it never retries, reorders or buffers. Your code decides what
to do about them.

It is built for continuous streams where a late packet is worth less than
the next one: audio, sensor samples, telemetry, control loops. udp_link
creates no tasks and runs entirely in the tasks that call it.

---

## Features

* **One link, one peer** — each instance is one connected UDP socket.
  Datagrams from any other address or port never reach you.
* **Loss and restarts are visible** — every datagram carries a sequence
  number that advances only when the stack accepts a send, and a random
  session ID chosen at each open. `udp_link_seq_delta()` does the
  wrap-safe arithmetic.
* **Strict validation** — every received datagram is checked against its
  header, and anything wrong (malformed, wrong version, foreign stream,
  too big for your buffer) gets its own result code instead of reaching
  you as data.
* **Zero copy in udp_link** — the header travels separately, so the
  payload goes straight from your buffer to the stack and lands at
  `buf[0]` of your receive buffer, with whatever alignment you gave it.
* **Never blocks on send** — if the stack has no buffer or no route,
  `udp_link_send()` returns at once and you choose to drop, retry or slow
  down.
* **A timeout you can trust** — `udp_link_recv()` never returns
  `ESP_ERR_TIMEOUT` before `timeout_ms` has passed, whatever the tick rate.
* **Full duplex** — one task can block in `udp_link_recv()` while others
  call `udp_link_send()` on the same link.
* **No hidden execution** — no tasks, timers or callbacks; one heap
  allocation per link, made in `udp_link_open()`.

---

## Chip Support

udp_link uses only the lwIP socket API, so it runs on any target with a
network interface (Wi-Fi, Ethernet, or loopback for testing).

| Chip | Status |
|---|---|
| ESP32 | Tested |
| ESP32-S2 | Expected to work |
| ESP32-S3 | Expected to work |
| ESP32-C3 | Expected to work |
| ESP32-C6 | Expected to work |

Update this table as hardware runs are recorded: `examples/counter_stream`
in loopback mode is the acceptance test.

---

## Installation

```bash
idf.py add-dependency "embedblocks/udp_link^0.1.0"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  embedblocks/udp_link: "^0.1.0"
```

Then enable UDP checksum verification, which udp_link requires (the build
fails without it). In `sdkconfig.defaults`:

```
CONFIG_LWIP_CHECKSUM_CHECK_UDP=y
```

This is a stack-wide setting: every UDP socket on the node verifies
checksums.

---

## Usage

Both ends are configured as mirror images: each side's `peer_port` is the
other side's `local_port`, and both use the same `stream_id`.

```c
#include "udp_link.h"

static udp_link_handle_t s_link;

static void rx_task(void *arg)
{
    uint8_t buf[256] __attribute__((aligned(4)));
    udp_link_rx_info_t info;
    uint32_t expected = 0;

    for (;;) {
        esp_err_t err = udp_link_recv(s_link, buf, sizeof buf, 100, &info);
        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NO_MEM) {
            continue;                           // nothing arrived / try again
        }
        if (err != ESP_OK) {
            ESP_LOGW("app", "rejected: %s", udp_link_err_to_name(err));
            continue;                           // bad datagram, already discarded
        }
        int32_t d = udp_link_seq_delta(expected, info.seq);
        if (d > 0) {
            ESP_LOGW("app", "%ld datagrams missing", (long)d);
        }
        expected = info.seq + 1;
        // info.payload_len bytes are at buf[0]
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    // ... bring up Wi-Fi ...

    const udp_link_config_t cfg = {
        .local_port  = 5000,
        .peer_ip     = { .addr = ESP_IP4TOADDR(192, 168, 1, 20) },
        .peer_port   = 5000,                    // the peer's local_port
        .stream_id   = 0x42,                    // same on both ends
        .max_payload = 256,                     // must fit the path MTU
    };
    ESP_ERROR_CHECK(udp_link_open(&cfg, &s_link));
    xTaskCreate(rx_task, "rx", 4096, NULL, 6, NULL);

    uint8_t block[256];
    for (;;) {
        // ... fill block ...
        esp_err_t err = udp_link_send(s_link, block, sizeof block);
        if (err == ESP_ERR_NO_MEM || err == ESP_ERR_UDP_LINK_NO_ROUTE) {
            // local and temporary: drop, retry or slow down
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

The link can be opened before Wi-Fi connects. It starts carrying data
once the network is up, and keeps working across disconnects without
being reopened.

### Send results

| Result | Meaning | What to do |
|---|---|---|
| `ESP_OK` | the local stack accepted the datagram (not: delivered) | — |
| `ESP_ERR_NO_MEM` | no buffer in the stack right now | drop, retry or slow down |
| `ESP_ERR_UDP_LINK_NO_ROUTE` | no route or no Wi-Fi link to the peer | same; it clears when the link returns |
| `ESP_ERR_INVALID_SIZE` | `len > max_payload` | fix the caller |
| `ESP_ERR_INVALID_ARG` | null handle or payload, or `len == 0` | fix the caller |
| `ESP_FAIL` | any other socket error | report; unexpected |

Your payload buffer is yours again as soon as `udp_link_send()` returns,
whatever the result.

### Receive results

| Result | Meaning | `info` |
|---|---|---|
| `ESP_OK` | `info.payload_len` bytes at `buf[0]` | valid |
| `ESP_ERR_TIMEOUT` | nothing arrived within `timeout_ms` | — |
| `ESP_ERR_NO_MEM` | the wait could not get memory; nothing was lost | — |
| `ESP_ERR_UDP_LINK_TRUNCATED` | datagram larger than `cap`; first `cap` bytes kept | valid |
| `ESP_ERR_UDP_LINK_SIZE_MISMATCH` | peer sends more than this link's `max_payload` | valid |
| `ESP_ERR_UDP_LINK_MALFORMED` | datagram disagrees with its header | — |
| `ESP_ERR_UDP_LINK_VERSION` | unsupported protocol version | — |
| `ESP_ERR_UDP_LINK_STREAM` | `stream_id` does not match | — |
| `ESP_ERR_INVALID_STATE` | another task is already in `udp_link_recv()` on this link | — |
| `ESP_ERR_INVALID_ARG` | bad argument, or `timeout_ms` above 24 h | — |
| `ESP_FAIL` | any other socket error | — |

Each call consumes at most one datagram. A rejected datagram is gone;
count it and call again. `ESP_ERR_NO_MEM` is a local condition, not a bad
datagram.

### Rules

- **One receiving task per link.** A second concurrent `udp_link_recv()`
  is rejected with `ESP_ERR_INVALID_STATE`.
- **Any number of sending tasks.** They are serialized; their datagrams
  share one sequence.
- **Use a finite receive timeout if the link is ever closed.**
  `UDP_LINK_WAIT_FOREVER` can only be ended by a datagram from the peer.
- **Close only after every task using the link has stopped calling it.**
  With assertions enabled, `udp_link_close()` asserts if a call is still
  in progress.
- **Keep both addresses fixed** (static IP or DHCP reservation). No NAT
  between the peers.
- **Pick `max_payload` for the path.** 1460 assumes a 1500-byte MTU; a
  too-large datagram is lost silently, with no error on either side.

### Interpreting `seq` and `session`

udp_link reports them as received and keeps no receive state. For a
simple stream:

- `udp_link_seq_delta(expected, info.seq)` > 0: that many datagrams are
  missing (lost, or still to arrive late);
- < 0: a late or duplicate datagram;
- `info.session` changed: the sender rebooted or reopened its link, and
  its sequence restarted at 0.

`UDP_LINK_SPEC.md` describes a fuller receiver policy that also handles
delayed datagrams from a previous session.

---

## Wire Format

Every datagram is a 12-byte header followed by the payload, all fields
big-endian:

| Offset | Field | Type |
|---|---|---|
| 0 | version (`1`) | u8 |
| 1 | stream_id | u8 |
| 2 | payload_len (1–1460) | u16 |
| 4 | session | u32 |
| 8 | seq | u32 |
| 12 | payload | bytes |

A receiver on a PC only needs to strip or parse these 12 bytes; it does not
need udp_link.

---

## Examples

| Example | What it does | Hardware |
|---|---|---|
| `examples/counter_stream` | Each end sends a counter and checks what arrives; prints PASS/FAIL every second in loopback mode | One board (loopback) or two boards over Wi-Fi |

---

## Testing

| Folder | What it is | How to run |
|---|---|---|
| `examples/counter_stream` | On-target acceptance test in loopback mode, no peripherals | `idf.py set-target esp32c3 build flash monitor` |
| `host_test/` | Host tests of the validator and helpers (ASan/UBSan, 2-million-case differential test), and of `udp_link.c` over Linux loopback sockets at 1000, 300 and 100 Hz | `make` (Linux or WSL) |

---

## Notes

**Why the receive timeout is never early.** `udp_link_recv()` converts
`timeout_ms` to an absolute deadline on the FreeRTOS tick count, with one
extra tick for the partly elapsed current tick. Only the deadline check
returns `ESP_ERR_TIMEOUT`; the wait underneath is just a wait. At a 100 Hz
tick a 1 ms timeout can therefore last up to a few tick periods. Use a
higher `CONFIG_FREERTOS_HZ` if you poll with short timeouts.

**Oversized datagrams.** The receive offers one byte beyond your buffer.
Only a datagram longer than the buffer reaches it, which tells udp_link the
datagram was truncated whatever the socket layer reports.

**Receive wait.** The wait uses `select()` by default. Building with
`idf.py -DUDP_LINK_WAIT_SO_RCVTIMEO=1` switches to `SO_RCVTIMEO`; only one
internal function changes. Which one becomes the only implementation will
be decided by measurements on hardware.

**Large data.** A payload must fit in one datagram. To send something
larger, such as a camera frame, split it into chunks with your own chunk
header and reassemble on the receiver.

---

## Known Limitations

- IPv4 unicast only; no broadcast, multicast or IPv6.
- No retransmission, acknowledgement, reordering or de-duplication.
- No fragmentation: a payload is at most 1460 bytes, less on paths with a
  smaller MTU.
- No authentication or encryption. `seq` and `session` are not security
  features; a pipe that needs them adds its own in the payload.
- One receiving task per link; no wait across several links.
- No local way to wake a blocked `udp_link_recv()`; stop it with a finite
  timeout.

---

## Requirements

- ESP-IDF v6.0.x. The build fails on other releases until a release has
  been verified.
- `CONFIG_LWIP_CHECKSUM_CHECK_UDP=y`.
- lwIP port options `LWIP_NETIF_TX_SINGLE_PBUF`, `LWIP_NETCONN_FULLDUPLEX`
  and `LWIP_NETCONN_SEM_PER_THREAD`, and `CONFIG_VFS_SUPPORT_SELECT` for
  the default wait (ESP-IDF's defaults). The build checks all of them.
- `esp_netif_init()` called before `udp_link_open()`.

---

## License

MIT License — see LICENSE file.
