# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).


## [0.1.0] - 2026-10-05

### Added
- Initial release of `udp_link`: a point-to-point UDP datagram transport for
  ESP-IDF v6.0.x, built on the lwIP socket API. No internal task, no
  callbacks, no buffering.
- Handle-based API: `udp_link_open()`, `udp_link_close()`,
  `udp_link_send()`, `udp_link_recv()`, plus `udp_link_seq_delta()` and
  `udp_link_err_to_name()`. Any number of instances, one per local port.
- Wire format version 1: a 12-byte big-endian header (version, stream ID,
  payload length, session, sequence) in front of every payload of 1 to 1460
  bytes. One payload is always exactly one datagram.
- Sequence numbers that advance only when the stack accepts a send, and a
  random session per open, so a receiver can see loss, late datagrams and
  sender restarts.
- Six-rule validation of every received datagram, with distinct results for
  malformed, wrong-version, wrong-stream, truncated and oversized datagrams.
- Zero-copy I/O: the header travels in its own gather/scatter element, so
  the payload is sent from and received into the caller's buffer, starting
  at `buf[0]`.
- Oversized-datagram detection with a one-byte sentinel, independent of
  `MSG_TRUNC` and of how the socket layer counts a truncated receive.
- `udp_link_send()` never waits for buffer space (`ESP_ERR_NO_MEM` instead)
  and reports a missing route or link as `ESP_ERR_UDP_LINK_NO_ROUTE`.
- `udp_link_recv()` timeout built on an absolute tick deadline: it never
  returns `ESP_ERR_TIMEOUT` before `timeout_ms` has elapsed.
- Full duplex: one task may send while another receives on the same
  instance. Concurrent senders are serialized so wire order equals sequence
  order.
- Build-time checks for the lwIP options the guarantees depend on
  (UDP checksums, single-pbuf TX, full-duplex netconn) and for the
  supported ESP-IDF range.
- `UDP_LINK_SPEC.md`: the complete implementation specification.
- `examples/counter_stream`: bring-up and test app, in loopback on one board
  or between two boards over Wi-Fi, with a PASS/FAIL report every second.
- `host_test/`: host tests of the validator, helpers and argument checks
  (with ASan/UBSan and a 2-million-case differential test), and of
  `udp_link.c` itself over Linux loopback sockets in both receive-wait
  variants at three tick rates.
