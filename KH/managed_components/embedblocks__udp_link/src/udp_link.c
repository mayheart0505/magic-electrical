/*
 * udp_link: UDP transport mechanism between two peers. Implements UDP_LINK_SPEC.md.
 *
 * Section references (§n) are to UDP_LINK_SPEC.md.
 */
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include <sys/select.h>
#else  /* host builds of the test harness: POSIX sockets */
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "udp_link.h"
#include "udp_link_priv.h"

#ifndef UDP_LINK_WAIT_SO_RCVTIMEO
#define UDP_LINK_WAIT_SO_RCVTIMEO 0             /* 0: select() wait (default); 1: SO_RCVTIMEO */
#endif

#ifdef ESP_PLATFORM
#include "udp_link_build_checks.h"
#endif

static const char *TAG = "udp_link";

#define WAIT_FOREVER_TICKS  UINT32_MAX

/* ---- instance (§5) ---- */

struct udp_link_instance {
    udp_link_config_t     cfg;
    int               sock;
    StaticSemaphore_t tx_mutex_storage;
    SemaphoreHandle_t tx_mutex;
    uint32_t          tx_seq;               /* guarded by tx_mutex */
    esp_err_t         tx_prev_result;       /* guarded by tx_mutex */
    uint32_t          session;
    atomic_bool       rx_busy;
#if UDP_LINK_WAIT_SO_RCVTIMEO
    bool              rcvtimeo_valid;       /* RX path only: single-receiver rule */
    uint32_t          rcvtimeo_ticks;
#endif
};

/* ---- udp_link_open (§6) ---- */

esp_err_t udp_link_open(const udp_link_config_t *cfg, udp_link_handle_t *out)
{
    if (out != NULL) {
        *out = NULL;
    }
    esp_err_t err = udp_link_check_open_args(cfg, out != NULL);
    if (err != ESP_OK) {
        return err;
    }

    struct udp_link_instance *h = heap_caps_calloc(1, sizeof *h,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->cfg            = *cfg;
    h->sock           = -1;
    h->tx_mutex       = xSemaphoreCreateMutexStatic(&h->tx_mutex_storage);
    h->tx_seq         = 0;
    h->tx_prev_result = ESP_OK;
    atomic_init(&h->rx_busy, false);

    h->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h->sock < 0) {
        err = udp_link_map_open_errno(UDP_LINK_OPEN_SOCKET, errno);
        goto fail;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family      = AF_INET;
    local.sin_port        = htons(cfg->local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(h->sock, (struct sockaddr *)&local, sizeof local) < 0) {
        err = udp_link_map_open_errno(UDP_LINK_OPEN_BIND, errno);
        goto fail;
    }

    struct sockaddr_in peer;
    memset(&peer, 0, sizeof peer);
    peer.sin_family      = AF_INET;
    peer.sin_port        = htons(cfg->peer_port);
    peer.sin_addr.s_addr = cfg->peer_ip.addr;           /* already network order */
    if (connect(h->sock, (struct sockaddr *)&peer, sizeof peer) < 0) {
        err = udp_link_map_open_errno(UDP_LINK_OPEN_CONNECT, errno);
        goto fail;
    }

    h->session = esp_random();
    *out = h;
    return ESP_OK;

fail:
    if (h->sock >= 0) {
        close(h->sock);
    }
    vSemaphoreDelete(h->tx_mutex);
    heap_caps_free(h);
    return err;
}

/* ---- udp_link_close (§7) ---- */

esp_err_t udp_link_close(udp_link_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#ifndef NDEBUG
    /* Misuse diagnostic (COMM-LIFE-03). Never leaves the mutex held. */
    const bool rx_idle = !atomic_load(&h->rx_busy);
    const bool tx_idle = xSemaphoreTake(h->tx_mutex, 0) == pdTRUE;
    if (tx_idle) {
        xSemaphoreGive(h->tx_mutex);
    }
    assert(rx_idle && "udp_link_close while udp_link_recv is in progress");
    assert(tx_idle && "udp_link_close while udp_link_send is in progress");
#endif

    (void)close(h->sock);                   /* result not reported (COMM-API-04) */
    vSemaphoreDelete(h->tx_mutex);
    heap_caps_free(h);
    return ESP_OK;
}

/* ---- udp_link_send (§8) ---- */

esp_err_t udp_link_send(udp_link_handle_t h, const void *payload, size_t len)
{
    esp_err_t err = udp_link_check_send_args(h != NULL, payload, len,
                                         h != NULL ? h->cfg.max_payload : 0);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t hdr[UDP_LINK_HEADER_SIZE];
    struct iovec iov[2] = {
        { .iov_base = hdr,             .iov_len = sizeof hdr },
        { .iov_base = (void *)payload, .iov_len = len        },
    };
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov    = iov;
    msg.msg_iovlen = 2;

    int  saved_errno = 0;
    bool log_now;

    xSemaphoreTake(h->tx_mutex, portMAX_DELAY);
    {
        udp_link_hdr_encode(hdr, h->cfg.stream_id, (uint16_t)len, h->session, h->tx_seq);
        if (sendmsg(h->sock, &msg, MSG_DONTWAIT) >= 0) {
            err = ESP_OK;
            h->tx_seq++;                            /* only on acceptance (COMM-TX-04) */
        } else {
            saved_errno = errno;
            err = udp_link_map_send_errno(saved_errno);
        }
        log_now = (err == ESP_ERR_UDP_LINK_NO_ROUTE && h->tx_prev_result != ESP_ERR_UDP_LINK_NO_ROUTE);
        h->tx_prev_result = err;
    }
    xSemaphoreGive(h->tx_mutex);

    if (log_now) {
        ESP_LOGW(TAG, "no route to peer (errno %d)", saved_errno);
    }
    return err;
}

/* ---- udp_link_recv (§9) ---- */

typedef enum {
    IO_GOT,             /* a datagram was received; *got holds recvmsg's return value */
    IO_EMPTY,           /* nothing to receive (EAGAIN / EWOULDBLOCK)                  */
    IO_WAIT_NOMEM,      /* the wait failed for lack of memory; nothing consumed       */
    IO_ERROR,           /* any other failure                                          */
} io_t;

static io_t io_from_recvmsg(ssize_t n, size_t *got)
{
    if (n >= 0) {
        *got = (size_t)n;
        return IO_GOT;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return IO_EMPTY;
    }
    return IO_ERROR;
}

/* One receive attempt that never waits. */
static io_t attempt(struct udp_link_instance *h, struct msghdr *m, size_t *got)
{
    m->msg_flags = 0;
    return io_from_recvmsg(recvmsg(h->sock, m, MSG_DONTWAIT), got);
}

static void ticks_to_timeval(uint32_t ticks, struct timeval *tv)
{
    const uint32_t ms = ticks * (uint32_t)portTICK_PERIOD_MS;
    tv->tv_sec  = (time_t)(ms / 1000u);
    tv->tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
}

/*
 * The wait primitive (§9.4). Waits at most `ticks` (or without limit), then
 * makes one receive attempt. This is the only function that differs between
 * the two variants.
 */
#if !UDP_LINK_WAIT_SO_RCVTIMEO

static io_t wait_then_attempt(struct udp_link_instance *h, struct msghdr *m,
                              uint32_t ticks, size_t *got)
{
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(h->sock, &rd);

    struct timeval tv, *tvp = NULL;
    if (ticks != WAIT_FOREVER_TICKS) {
        ticks_to_timeval(ticks, &tv);
        tvp = &tv;
    }
    if (select(h->sock + 1, &rd, NULL, NULL, tvp) < 0) {
        /* No recvmsg has run since the last EAGAIN: nothing was consumed. */
        return errno == ENOMEM ? IO_WAIT_NOMEM : IO_ERROR;
    }
    /* Readable or timed out: only the next attempt and the deadline decide. */
    return attempt(h, m, got);
}

#else  /* UDP_LINK_WAIT_SO_RCVTIMEO */

static io_t wait_then_attempt(struct udp_link_instance *h, struct msghdr *m,
                              uint32_t ticks, size_t *got)
{
    if (!h->rcvtimeo_valid || h->rcvtimeo_ticks != ticks) {
        struct timeval tv = { 0, 0 };                   /* zero: no limit */
        if (ticks != WAIT_FOREVER_TICKS) {
            ticks_to_timeval(ticks, &tv);
        }
        if (setsockopt(h->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) {
            h->rcvtimeo_valid = false;
            return IO_ERROR;
        }
        h->rcvtimeo_valid = true;
        h->rcvtimeo_ticks = ticks;
    }
    m->msg_flags = 0;
    return io_from_recvmsg(recvmsg(h->sock, m, 0), got);
}

#endif

static esp_err_t recv_loop(struct udp_link_instance *h, void *buf, size_t cap,
                           uint32_t timeout_ms, udp_link_rx_info_t *info)
{
    const bool     finite   = timeout_ms != UDP_LINK_WAIT_FOREVER;
    const uint32_t c0       = (uint32_t)xTaskGetTickCount();
    const uint32_t deadline = finite
        ? c0 + udp_link_timeout_ticks(timeout_ms, configTICK_RATE_HZ) + 1u   /* +1: never early */
        : 0;

    uint8_t hdr[UDP_LINK_HEADER_SIZE];
    uint8_t sentinel;
    struct iovec iov[3] = {
        { .iov_base = hdr,       .iov_len = sizeof hdr },
        { .iov_base = buf,       .iov_len = cap        },
        { .iov_base = &sentinel, .iov_len = 1          },
    };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov    = iov;
    m.msg_iovlen = 3;

    size_t got = 0;
    io_t io = attempt(h, &m, &got);                     /* attempt before any wait */

    for (;;) {
        switch (io) {
        case IO_GOT:
            return udp_link_validate(hdr, udp_link_rx_facts(got, cap), cap,
                                 h->cfg.stream_id, h->cfg.max_payload, info);
        case IO_WAIT_NOMEM:
            return ESP_ERR_NO_MEM;
        case IO_ERROR:
            return ESP_FAIL;
        case IO_EMPTY:
            break;
        }

        if (timeout_ms == 0) {
            return ESP_ERR_TIMEOUT;
        }

        uint32_t k = WAIT_FOREVER_TICKS;
        if (finite) {
            const uint32_t now = (uint32_t)xTaskGetTickCount();
            if ((int32_t)(now - deadline) >= 0) {
                return ESP_ERR_TIMEOUT;                 /* the only way to a timeout */
            }
            k = udp_link_wait_k(deadline - now);            /* >= 1 */
        }
        io = wait_then_attempt(h, &m, k, &got);
    }
}

esp_err_t udp_link_recv(udp_link_handle_t h, void *buf, size_t cap,
                    uint32_t timeout_ms, udp_link_rx_info_t *info)
{
    esp_err_t err = udp_link_check_recv_args(h != NULL, buf, cap, timeout_ms, info);
    if (err != ESP_OK) {
        return err;
    }
    if (atomic_exchange(&h->rx_busy, true)) {
        return ESP_ERR_INVALID_STATE;                   /* not ours to clear */
    }
    err = recv_loop(h, buf, cap, timeout_ms, info);
    atomic_store(&h->rx_busy, false);
    return err;
}
