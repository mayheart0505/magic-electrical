cmake_minimum_required(VERSION 3.16)

# udp_link itself, plus Wi-Fi bring-up for two-board mode.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(udp_link_counter_stream)
