/*
 * udp_link private header: pure functions (UDP_LINK_SPEC.md §13).
 * No socket, no RTOS, no state. Included by udp_link.c and by the host tests.
 * Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "udp_link.h"

#define UDP_LINK_PROTO_VERSION  1

/* Facts about one receive, derived from recvmsg's return value (§9.2). */
typedef struct {
    size_t received;        /* datagram bytes received, counted up to 12 + cap */
    bool   exceeded;        /* datagram was longer than 12 + cap               */
} udp_link_rx_facts_t;

typedef enum {
    UDP_LINK_OPEN_SOCKET,
    UDP_LINK_OPEN_BIND,
    UDP_LINK_OPEN_CONNECT,
} udp_link_open_step_t;

/* Wire format (§3) */
void udp_link_hdr_encode(uint8_t out[UDP_LINK_HEADER_SIZE], uint8_t stream_id,
                     uint16_t payload_len, uint32_t session, uint32_t seq);

/* Receive facts and validation (§4, §9.2) */
udp_link_rx_facts_t udp_link_rx_facts(size_t ret, size_t cap);
esp_err_t udp_link_validate(const uint8_t hdr[UDP_LINK_HEADER_SIZE], udp_link_rx_facts_t facts,
                        size_t cap, uint8_t stream_id, uint16_t max_payload,
                        udp_link_rx_info_t *info);

/* Argument checks (§6, §8, §9.1) */
bool      udp_link_peer_ip_ok(uint32_t addr_network_order);
esp_err_t udp_link_check_open_args(const udp_link_config_t *cfg, bool out_ok);
esp_err_t udp_link_check_send_args(bool handle_ok, const void *payload, size_t len,
                               uint16_t max_payload);
esp_err_t udp_link_check_recv_args(bool handle_ok, const void *buf, size_t cap,
                               uint32_t timeout_ms, const udp_link_rx_info_t *info);

/* Receive timing (§9.3) */
uint32_t udp_link_timeout_ticks(uint32_t timeout_ms, uint32_t tick_hz);
uint32_t udp_link_wait_k(uint32_t remaining_ticks);

/* Errno mapping (§6, §8) */
esp_err_t udp_link_map_open_errno(udp_link_open_step_t step, int err);
esp_err_t udp_link_map_send_errno(int err);
