/*
 * udp_link pure functions (UDP_LINK_SPEC.md §13). No socket, no RTOS, no state.
 */
#include <errno.h>
#include <string.h>
#include "udp_link_priv.h"

/* ---- big-endian helpers (§3, COMM-WIRE-01) ---- */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void udp_link_hdr_encode(uint8_t out[UDP_LINK_HEADER_SIZE], uint8_t stream_id,
                     uint16_t payload_len, uint32_t session, uint32_t seq)
{
    out[0] = UDP_LINK_PROTO_VERSION;
    out[1] = stream_id;
    wr16(&out[2], payload_len);
    wr32(&out[4], session);
    wr32(&out[8], seq);
}

/* ---- receive facts (§9.2, COMM-IDF-03) ---- */

udp_link_rx_facts_t udp_link_rx_facts(size_t ret, size_t cap)
{
    const size_t space = UDP_LINK_HEADER_SIZE + cap;
    udp_link_rx_facts_t f = {
        .exceeded = ret > space,
        .received = ret > space ? space : ret,
    };
    return f;
}

/* ---- validation (§4, COMM-VAL-01, COMM-VAL-02) ---- */

esp_err_t udp_link_validate(const uint8_t hdr[UDP_LINK_HEADER_SIZE], udp_link_rx_facts_t f,
                        size_t cap, uint8_t stream_id, uint16_t max_payload,
                        udp_link_rx_info_t *info)
{
    /* Rule 1: the whole header was received. Nothing below reads hdr before this. */
    if (f.received < UDP_LINK_HEADER_SIZE) {
        return ESP_ERR_UDP_LINK_MALFORMED;
    }
    /* Rule 2 */
    if (hdr[0] != UDP_LINK_PROTO_VERSION) {
        return ESP_ERR_UDP_LINK_VERSION;
    }
    /* Rule 3 */
    if (hdr[1] != stream_id) {
        return ESP_ERR_UDP_LINK_STREAM;
    }
    /* Rule 4 */
    const uint16_t plen = rd16(&hdr[2]);
    if (plen < 1 || plen > UDP_LINK_MAX_PAYLOAD_LIMIT) {
        return ESP_ERR_UDP_LINK_MALFORMED;
    }

    const udp_link_rx_info_t fields = {
        .payload_len = plen,
        .session     = rd32(&hdr[4]),
        .seq         = rd32(&hdr[8]),
    };

    /* Rule 5 */
    if (plen > max_payload) {
        *info = fields;
        return ESP_ERR_UDP_LINK_SIZE_MISMATCH;
    }

    /* Rule 6 */
    if (f.exceeded) {
        if (plen > cap) {
            *info = fields;                 /* buffer too small */
            return ESP_ERR_UDP_LINK_TRUNCATED;
        }
        return ESP_ERR_UDP_LINK_MALFORMED;      /* longer than declared */
    }
    if (f.received - UDP_LINK_HEADER_SIZE != plen) {
        return ESP_ERR_UDP_LINK_MALFORMED;      /* length disagrees with header */
    }
    *info = fields;
    return ESP_OK;
}

/* ---- argument checks (§6, §8, §9.1) ---- */

bool udp_link_peer_ip_ok(uint32_t addr_network_order)
{
    uint8_t octet[4];
    memcpy(octet, &addr_network_order, sizeof octet);   /* octet[0] is the first octet */
    if (octet[0] == 0) {
        return false;                       /* 0.0.0.0/8 */
    }
    if (octet[0] >= 224) {
        return false;                       /* 224.0.0.0/4 multicast, 240.0.0.0/4 reserved */
    }
    return true;                            /* includes 127.0.0.0/8 */
}

esp_err_t udp_link_check_open_args(const udp_link_config_t *cfg, bool out_ok)
{
    if (cfg == NULL || !out_ok) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->local_port == 0 || cfg->peer_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->max_payload < 1 || cfg->max_payload > UDP_LINK_MAX_PAYLOAD_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!udp_link_peer_ip_ok(cfg->peer_ip.addr)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t udp_link_check_send_args(bool handle_ok, const void *payload, size_t len,
                               uint16_t max_payload)
{
    if (!handle_ok || payload == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > max_payload) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t udp_link_check_recv_args(bool handle_ok, const void *buf, size_t cap,
                               uint32_t timeout_ms, const udp_link_rx_info_t *info)
{
    if (!handle_ok || buf == NULL || info == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms != UDP_LINK_WAIT_FOREVER && timeout_ms > UDP_LINK_MAX_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ---- receive timing (§9.3) ---- */

uint32_t udp_link_timeout_ticks(uint32_t timeout_ms, uint32_t tick_hz)
{
    return (uint32_t)(((uint64_t)timeout_ms * tick_hz + 999u) / 1000u);
}

uint32_t udp_link_wait_k(uint32_t remaining_ticks)
{
    return remaining_ticks > 2 ? remaining_ticks - 1 : 1;     /* max(R - 1, 1) */
}

/* ---- errno mapping (§6, §8) ---- */

esp_err_t udp_link_map_open_errno(udp_link_open_step_t step, int err)
{
    if (step == UDP_LINK_OPEN_SOCKET && (err == ENFILE || err == ENOBUFS || err == ENOMEM)) {
        return ESP_ERR_NO_MEM;
    }
    if (step == UDP_LINK_OPEN_BIND && err == EADDRINUSE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_FAIL;
}

esp_err_t udp_link_map_send_errno(int err)
{
    switch (err) {
    case ENOMEM:
    case ENOBUFS:
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return ESP_ERR_NO_MEM;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EIO:
        return ESP_ERR_UDP_LINK_NO_ROUTE;
    default:
        return ESP_FAIL;
    }
}

/* ---- helper (§11, COMM-API-06) ---- */

const char *udp_link_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_UDP_LINK_NO_ROUTE:      return "ESP_ERR_UDP_LINK_NO_ROUTE";
    case ESP_ERR_UDP_LINK_TRUNCATED:     return "ESP_ERR_UDP_LINK_TRUNCATED";
    case ESP_ERR_UDP_LINK_MALFORMED:     return "ESP_ERR_UDP_LINK_MALFORMED";
    case ESP_ERR_UDP_LINK_VERSION:       return "ESP_ERR_UDP_LINK_VERSION";
    case ESP_ERR_UDP_LINK_STREAM:        return "ESP_ERR_UDP_LINK_STREAM";
    case ESP_ERR_UDP_LINK_SIZE_MISMATCH: return "ESP_ERR_UDP_LINK_SIZE_MISMATCH";
    default:                         return esp_err_to_name(err);
    }
}
