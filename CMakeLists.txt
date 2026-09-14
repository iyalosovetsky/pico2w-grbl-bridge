cmake_minimum_required(VERSION 3.13)

# Both RP2040 (Pico W) and RP2350 (Pico 2 W) are supported by the same source tree —
# override at configure time with -DPICO_BOARD=pico_w or -DPICO_BOARD=pico2_w.
set(PICO_BOARD pico2_w CACHE STRING "Board type (pico_w for RP2040, pico2_w for RP2350)")

include(pico_sdk_import.cmake)

project(pico2w_grbl_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# PIO-USB host wiring, overridable at configure time (see also tools/build.sh):
#   -DPIO_USB_HOST_DP_PIN=<gpio>   D+ pin; D- is always this pin + 1 (library default pinout)
#   -DPIO_USB_HOST_PIO_INDEX=<0|1|2>  which PIO block to claim (kept off whatever CYW43 uses)
set(PIO_USB_HOST_DP_PIN 0 CACHE STRING "GPIO pin for the PIO-USB host port's D+ (D- = this + 1)")
set(PIO_USB_HOST_PIO_INDEX 1 CACHE STRING "PIO block index (0/1/2) claimed by the PIO-USB host")

# Pico-PIO-USB (vendored as a git submodule): lets the grblHAL USB-host link run on its
# own GPIO pins via PIO instead of the chip's native USB controller, so the native port
# stays free for flashing and stdio-over-CDC debugging. Works on both RP2040 and RP2350.
add_subdirectory(third_party/Pico-PIO-USB)

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
    src/usb_descriptors.c
    src/shared_state.c
    src/usb_host_cdc.c
    src/grbl_link.c
    src/http_server.c
    src/api_handlers.c
    src/wifi_config.c
    third_party/dhcpserver.c
    third_party/dnsserver.c
    # TinyUSB's PIO-USB host/device class drivers — not yet wired up as a pico-sdk
    # CMake target in this SDK version, so pulled in directly (same approach the
    # upstream Pico-PIO-USB host_hid_to_device_cdc example uses).
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)
add_dependencies(bridge web_page_header)

target_include_directories(bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}          # tusb_config.h, lwipopts.h
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/third_party
    ${CMAKE_CURRENT_BINARY_DIR}        # generated/web_page.h
)

target_compile_definitions(bridge PRIVATE
    PIO_USB_USE_TINYUSB
    PIO_USB_HOST_DP_PIN=${PIO_USB_HOST_DP_PIN}
    PIO_USB_HOST_PIO_INDEX=${PIO_USB_HOST_PIO_INDEX}
)

# The native USB port is used as a plain CDC device (stdio) now that grblHAL is reached
# over the separate PIO-USB host port instead — see PINOUT.md.
pico_enable_stdio_usb(bridge 1)
pico_enable_stdio_uart(bridge 0)

target_link_libraries(bridge
    pico_stdlib
    pico_multicore
    pico_flash
    pico_unique_id
    hardware_flash
    hardware_watchdog
    hardware_clocks
    tinyusb_device
    tinyusb_host
    tinyusb_board
    pico_pio_usb
    pico_cyw43_arch_lwip_poll
)

pico_add_extra_outputs(bridge)
