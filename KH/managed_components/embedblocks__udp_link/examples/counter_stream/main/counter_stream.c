/*
 * udp_link counter stream: bring-up and test app (design file 07, with review fixes).
 *
 * Each endpoint runs one TX task sending a counter and one RX task checking what
 * arrives, on the same udp_link instance, so both directions and full duplex are
 * exercised together.
 *
 *   Loopback (default): two instances on one board via 127.0.0.1. No Wi-Fi.
 *   Two boards:         one instance per board over Wi-Fi (menuconfig).
 *
 * Changes from 07:
 *   - counters are _Atomic (plain shared counters are a data race in C);
 *   - ESP_ERR_NO_MEM from udp_link_recv is a local condition, counted separately,
 *     not as a rejected datagram;
 *   - the loopback report checks each direction with three ordered reads
 *     (tx, rx, tx) against the exact bound rx <= tx_after + 1; the old
 *     "equal or one below" comparison of two whole-endpoint copies can fail on
 *     a correct build;
 *   - the reporter paces with xTaskDelayUntil and prints a PASS/FAIL verdict.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "udp_link.h"

#if !CONFIG_COUNTER_LOOPBACK
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#endif

static const char *TAG = "udp_link_counter";

#define PAYLOAD_LEN  CONFIG_COUNTER_PAYLOAD_LEN     /* 4 .. UDP_LINK_MAX_PAYLOAD_LIMIT */
#define PERIOD_MS    CONFIG_COUNTER_PERIOD_MS
#define RX_TIMEOUT   1000                           /* ms */
#define REPORT_MS    1000
#define STREAM_ID    0x42
#define PORT_A       5000
#define PORT_B       5001
#define TASK_STACK   (4096 + PAYLOAD_LEN)           /* starting point; measure */

_Static_assert(PAYLOAD_LEN >= 4 && PAYLOAD_LEN <= UDP_LINK_MAX_PAYLOAD_LIMIT, "PAYLOAD_LEN range");

typedef struct {
    const char    *name;
    udp_link_handle_t  udp_link;
    /* written by the TX task */
    _Atomic uint32_t tx_ok, tx_dropped, tx_errors;
    /* written by the RX task */
    _Atomic uint32_t rx_ok, rx_gap, rx_late, rx_bad, rx_rejected, rx_restarts, rx_nomem;
} endpoint_t;

static void inc(_Atomic uint32_t *c)              { atomic_fetch_add(c, 1); }
static uint32_t get(const _Atomic uint32_t *c)    { return atomic_load((_Atomic uint32_t *)c); }

/* ---- payload pattern ---- */

static void fill(uint8_t *b, uint32_t n)
{
    b[0] = n >> 24; b[1] = n >> 16; b[2] = n >> 8; b[3] = (uint8_t)n;
    for (size_t i = 4; i < PAYLOAD_LEN; i++) {
        b[i] = (uint8_t)(n + i);
    }
}

static bool check(const uint8_t *b, size_t len, uint32_t *n)
{
    if (len != PAYLOAD_LEN) return false;
    uint32_t v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                 ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
    for (size_t i = 4; i < PAYLOAD_LEN; i++) {
        if (b[i] != (uint8_t)(v + i)) return false;
    }
    *n = v;
    return true;
}

/* ---- TX task ---- */

static void tx_task(void *arg)
{
    endpoint_t *ep = arg;
    uint8_t buf[PAYLOAD_LEN];
    uint32_t counter = 0;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        fill(buf, counter);
        esp_err_t err = udp_link_send(ep->udp_link, buf, sizeof buf);
        if (err == ESP_OK) {
            counter++;                              /* mirrors udp_link's sequence */
            inc(&ep->tx_ok);                        /* after the send: see report() */
        } else if (err == ESP_ERR_NO_MEM || err == ESP_ERR_UDP_LINK_NO_ROUTE) {
            inc(&ep->tx_dropped);                   /* resend the same counter next period */
        } else {
            inc(&ep->tx_errors);
            ESP_LOGE(TAG, "%s udp_link_send: %s", ep->name, udp_link_err_to_name(err));
        }
        xTaskDelayUntil(&last, pdMS_TO_TICKS(PERIOD_MS));
    }
}

/* ---- RX task ---- */

static void rx_task(void *arg)
{
    endpoint_t *ep = arg;
    uint8_t buf[PAYLOAD_LEN] __attribute__((aligned(4)));
    udp_link_rx_info_t info;
    bool     have_session = false;
    uint32_t session = 0, expected = 0;

    for (;;) {
        esp_err_t err = udp_link_recv(ep->udp_link, buf, sizeof buf, RX_TIMEOUT, &info);
        if (err == ESP_ERR_TIMEOUT) continue;
        if (err == ESP_ERR_NO_MEM) {                /* local: nothing consumed, call again */
            inc(&ep->rx_nomem);
            continue;
        }
        if (err != ESP_OK) {
            inc(&ep->rx_rejected);
            ESP_LOGE(TAG, "%s udp_link_recv: %s", ep->name, udp_link_err_to_name(err));
            continue;
        }

        /* Minimal receiver for a test: every session change is a restart. A
         * production pipe uses the full or reduced receiver policy instead. */
        if (!have_session || info.session != session) {
            if (have_session) inc(&ep->rx_restarts);
            have_session = true;
            session  = info.session;
            expected = info.seq;
        }

        int32_t d = udp_link_seq_delta(expected, info.seq);
        if (d < 0) { inc(&ep->rx_late); continue; }
        atomic_fetch_add(&ep->rx_gap, (uint32_t)d);
        expected = info.seq + 1;

        uint32_t n;
        if (check(buf, info.payload_len, &n) && n == info.seq) {
            inc(&ep->rx_ok);
        } else {
            inc(&ep->rx_bad);
        }
    }
}

/* ---- reporting ---- */

static endpoint_t ep_a = { .name = "A" };
static endpoint_t ep_b = { .name = "B" };

static void log_endpoint(const endpoint_t *ep)
{
    ESP_LOGI(TAG, "%s tx ok=%" PRIu32 " drop=%" PRIu32 " err=%" PRIu32
                  " | rx ok=%" PRIu32 " gap=%" PRIu32 " late=%" PRIu32 " bad=%" PRIu32
                  " rej=%" PRIu32 " restart=%" PRIu32 " nomem=%" PRIu32,
             ep->name, get(&ep->tx_ok), get(&ep->tx_dropped), get(&ep->tx_errors),
             get(&ep->rx_ok), get(&ep->rx_gap), get(&ep->rx_late), get(&ep->rx_bad),
             get(&ep->rx_rejected), get(&ep->rx_restarts), get(&ep->rx_nomem));
}

/* Counters that must stay 0 on every endpoint, in every mode. */
static bool defects_zero(const endpoint_t *ep)
{
    return get(&ep->tx_errors) == 0 && get(&ep->rx_bad) == 0 && get(&ep->rx_rejected) == 0;
}

#if CONFIG_COUNTER_LOOPBACK

/*
 * One direction, snd -> rcv. The sender counts a datagram only after udp_link_send
 * returns, and the receiver (higher priority) may count it first, so at any
 * instant rcv.rx_ok <= snd.tx_ok + 1. Reading tx after rx makes that bound hold
 * for the values read. The lower side (datagrams queued, not yet received) has
 * no guaranteed bound, so it is reported as "inflight", not checked; real loss
 * shows up as rx gap instead.
 */
static bool check_direction(const endpoint_t *snd, const endpoint_t *rcv, int *inflight)
{
    const uint32_t tx_before = get(&snd->tx_ok);
    const uint32_t rx        = get(&rcv->rx_ok);
    const uint32_t tx_after  = get(&snd->tx_ok);
    *inflight = (int)(int32_t)(tx_before - rx);
    return rx <= tx_after + 1;
}

static void report_loopback(void)
{
    static uint32_t prev_attempts[2];
    static unsigned reports, failures;
    const endpoint_t *eps[2] = { &ep_a, &ep_b };
    bool pass = true;
    int inflight_ab, inflight_ba;

    pass &= check_direction(&ep_a, &ep_b, &inflight_ab);
    pass &= check_direction(&ep_b, &ep_a, &inflight_ba);

    for (int i = 0; i < 2; i++) {
        const endpoint_t *ep = eps[i];
        pass &= defects_zero(ep);
        pass &= get(&ep->tx_dropped) == 0 && get(&ep->rx_gap) == 0 &&
                get(&ep->rx_late) == 0 && get(&ep->rx_restarts) == 0;
        /* one send attempt per period, give or take one per report */
        const uint32_t attempts = get(&ep->tx_ok) + get(&ep->tx_dropped) + get(&ep->tx_errors);
        const int32_t  delta    = (int32_t)(attempts - prev_attempts[i]);
        if (reports > 0 && (delta < REPORT_MS / PERIOD_MS - 1 || delta > REPORT_MS / PERIOD_MS + 1)) {
            pass = false;
        }
        prev_attempts[i] = attempts;
    }

    log_endpoint(&ep_a);
    log_endpoint(&ep_b);
    reports++;
    if (!pass) failures++;
    ESP_LOGI(TAG, "report %u: %s (inflight A->B %d, B->A %d; failed reports so far %u)",
             reports, pass ? "PASS" : "FAIL", inflight_ab, inflight_ba, failures);
}

#endif

/* ---- startup ---- */

static void open_endpoint(endpoint_t *ep, uint16_t local_port,
                          esp_ip4_addr_t peer_ip, uint16_t peer_port)
{
    const udp_link_config_t cfg = {
        .local_port  = local_port,
        .peer_ip     = peer_ip,
        .peer_port   = peer_port,
        .stream_id   = STREAM_ID,
        .max_payload = PAYLOAD_LEN,
    };
    ESP_ERROR_CHECK(udp_link_open(&cfg, &ep->udp_link));
}

static void start_endpoint(endpoint_t *ep)
{
    BaseType_t rc = xTaskCreate(tx_task, "tx", TASK_STACK, ep, 5, NULL);
    configASSERT(rc == pdPASS);
    rc = xTaskCreate(rx_task, "rx", TASK_STACK, ep, 6, NULL);
    configASSERT(rc == pdPASS);
    (void)rc;
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());               /* udp_link needs a running TCP/IP stack */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_COUNTER_LOOPBACK
    /* No RF running: session values may be pseudo-random. Harmless here, since
     * each instance opens once per boot. */
    const esp_ip4_addr_t lo = { .addr = ESP_IP4TOADDR(127, 0, 0, 1) };
    open_endpoint(&ep_a, PORT_A, lo, PORT_B);        /* both sockets exist ...       */
    open_endpoint(&ep_b, PORT_B, lo, PORT_A);
    start_endpoint(&ep_a);                           /* ... before either task sends */
    start_endpoint(&ep_b);
#else
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(example_connect());              /* Wi-Fi from protocol_examples_common */

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {                             /* e.g. refused under BT coexistence */
        ESP_LOGW(TAG, "power save not disabled: %s", esp_err_to_name(err));
    }
    wifi_ps_type_t ps;
    ESP_ERROR_CHECK(esp_wifi_get_ps(&ps));
    ESP_LOGI(TAG, "Wi-Fi power save in effect: %s",
             ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min modem" : "max modem");

    esp_ip4_addr_t peer;
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_COUNTER_PEER_IP, &peer));
    open_endpoint(&ep_a, CONFIG_COUNTER_PORT, peer, CONFIG_COUNTER_PORT);
    start_endpoint(&ep_a);
#endif

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(REPORT_MS));
#if CONFIG_COUNTER_LOOPBACK
        report_loopback();
#else
        log_endpoint(&ep_a);
        ESP_LOGI(TAG, "defect counters (tx err, rx bad, rx rej): %s",
                 defects_zero(&ep_a) ? "all 0" : "NONZERO");
#endif
    }
}
