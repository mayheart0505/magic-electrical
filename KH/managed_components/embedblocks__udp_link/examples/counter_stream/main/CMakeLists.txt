idf_component_register(
    SRCS     "counter_stream.c"
    REQUIRES udp_link esp_netif esp_event esp_wifi nvs_flash protocol_examples_common
)
