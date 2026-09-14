cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pico2w_grbl_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Embed web/index.html into a C header at build time.
set(WEB_PAGE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/web_page.h)
add_custom_command(
    OUTPUT ${WEB_PAGE_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_web_header.py
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${WEB_PAGE_HEADER} index_html
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_web_header.py
    COMMENT "Embedding web/index.html"
)
add_custom_target(web_page_header DEPENDS ${WEB_PAGE_HEADER})

add_executable(bridge
    src/main.c
    src/shared_state.c
    src/usb_host_cdc.c
    src/grbl_link.c
    src/http_server.c
    src/api_handlers.c
    src/wifi_config.c
    third_party/dhcpserver.c
    third_party/dnsserver.c
)
add_dependencies(bridge web_page_header)

target_include_directories(bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}          # tusb_config.h, lwipopts.h
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/third_party
    ${CMAKE_CURRENT_BINARY_DIR}        # generated/web_page.h
)

# The native USB port is used in host role (talking to grblHAL) — free it from stdio and
# use UART0 (GPIO0/1 by default) for printf debugging instead. See PINOUT.md.
pico_enable_stdio_usb(bridge 0)
pico_enable_stdio_uart(bridge 1)

target_link_libraries(bridge
    pico_stdlib
    pico_multicore
    pico_flash
    pico_unique_id
    hardware_flash
    hardware_watchdog
    tinyusb_host
    tinyusb_board
    pico_cyw43_arch_lwip_poll
)

pico_add_extra_outputs(bridge)
