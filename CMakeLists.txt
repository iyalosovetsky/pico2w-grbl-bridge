cmake_minimum_required(VERSION 3.13)

# Both RP2040 (Pico W) and RP2350 (Pico 2 W) are supported by the same source tree —
# override at configure time with -DPICO_BOARD=pico_w or -DPICO_BOARD=pico2_w.
set(PICO_BOARD pico2_w CACHE STRING "Board type (pico_w for RP2040, pico2_w for RP2350)")

include(pico_sdk_import.cmake)

project(pico2w_grbl_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# native-usb-host branch: grblHAL is reached over the board's own native USB controller
# in host role (via a USB-A OTG adapter on its built-in micro-USB port), instead of the
# separate PIO-USB host port on custom pins used on main. Debug output goes out UART0
# (GPIO0=TX/GPIO1=RX, 115200 8N1) since the native port is occupied by the host role —
# see pico_enable_stdio_uart below.

# Optional WiFi STA credentials baked into the firmware as a fallback, used only until
# something is saved to flash (via /api/wifi, or a prior build with these set) — see
# tools/build.sh --wifi-ssid/--wifi-password. Left blank, the board just starts in its
# own AP mode until configured over the web UI.
set(WIFI_SSID "" CACHE STRING "Default WiFi SSID baked into the firmware (flash-saved credentials always take priority)")
set(WIFI_PASSWORD "" CACHE STRING "Default WiFi password baked into the firmware (see WIFI_SSID)")

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

# Build metadata (timestamp, git-derived build number/hash/dirty flag) for the startup
# banner — regenerated on every build (not just every cmake configure), via a target
# with no real OUTPUT so it's always considered out of date.
set(BUILD_INFO_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h)
add_custom_target(build_info_header ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_build_info.py ${BUILD_INFO_HEADER}
    BYPRODUCTS ${BUILD_INFO_HEADER}
    COMMENT "Generating build_info.h"
)

add_executable(bridge
    src/main.c
    src/led.c
    src/shared_state.c
    src/usb_host_cdc.c
    src/grbl_link.c
    src/http_server.c
    src/api_handlers.c
    src/wifi_config.c
    third_party/dhcpserver.c
    third_party/dnsserver.c
)
add_dependencies(bridge web_page_header build_info_header)

target_include_directories(bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}          # tusb_config.h, lwipopts.h
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/third_party
    ${CMAKE_CURRENT_BINARY_DIR}        # generated/web_page.h
)

target_compile_definitions(bridge PRIVATE
    WIFI_SSID_DEFAULT=\"${WIFI_SSID}\"
    WIFI_PASSWORD_DEFAULT=\"${WIFI_PASSWORD}\"
)

# The native USB port is used in host role (talking to grblHAL) — free it from stdio and
# use UART0 for printf debugging instead.
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
