/*
 * Build-time checks (UDP_LINK_SPEC.md §12, COMM-IDF-12). ESP-IDF builds only.
 */
#pragma once

#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "lwip/opt.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
#error "udp_link: verified on ESP-IDF 6.0.x only; this ESP-IDF release is unverified"
#endif
#if !LWIP_NETIF_TX_SINGLE_PBUF
#error "udp_link: TX payload lifetime requires LWIP_NETIF_TX_SINGLE_PBUF"
#endif
#if !LWIP_NETCONN_FULLDUPLEX || !LWIP_NETCONN_SEM_PER_THREAD
#error "udp_link: full duplex requires LWIP_NETCONN_FULLDUPLEX and LWIP_NETCONN_SEM_PER_THREAD"
#endif
#if !UDP_LINK_WAIT_SO_RCVTIMEO && !defined(CONFIG_VFS_SUPPORT_SELECT)
#error "udp_link: the select() wait requires CONFIG_VFS_SUPPORT_SELECT"
#endif
#if !CHECKSUM_GEN_UDP || !CHECKSUM_CHECK_UDP
#error "udp_link: requires UDP checksums; set CONFIG_LWIP_CHECKSUM_CHECK_UDP=y in menuconfig"
#endif
