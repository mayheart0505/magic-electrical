idf_component_register(
    SRCS              "src/udp_link.c" "src/udp_link_pure.c"
    INCLUDE_DIRS      "include"
    PRIV_INCLUDE_DIRS "private" "src"
    REQUIRES          esp_netif
    PRIV_REQUIRES     lwip esp_hw_support heap log
)

# Wait primitive (UDP_LINK_SPEC.md §9.4). Default: select().
# For the wait-primitive comparison (TC-MEM-04):  idf.py -DUDP_LINK_WAIT_SO_RCVTIMEO=1 build
if(UDP_LINK_WAIT_SO_RCVTIMEO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UDP_LINK_WAIT_SO_RCVTIMEO=1)
endif()
