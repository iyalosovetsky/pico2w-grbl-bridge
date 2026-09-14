# Vendored third-party code

- `dhcpserver.c` / `dhcpserver.h` — minimal DHCP server, from MicroPython (`lib/netutils`), MIT license (see `LICENSE.dhcpserver`). Vendored via the Raspberry Pi `pico-examples` `pico_w/wifi/access_point` example.
- `dnsserver.c` / `dnsserver.h` — minimal DNS server that answers every query with our own IP (used for the AP-mode captive config portal), Copyright Raspberry Pi (Trading) Ltd., BSD-3-Clause (see header comment in the file).

Both are used unmodified from `raspberrypi/pico-examples` (`pico_w/wifi/access_point/`), which is exactly what that example directory is meant for — a reusable AP-mode + DHCP/DNS building block.
