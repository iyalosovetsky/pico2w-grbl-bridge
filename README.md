# pico2w-grbl-bridge

WiFi/REST bridge for the [scanner turntable rig](https://github.com/iyalosovetsky/rotary-pico)
running a [custom grblHAL](https://github.com/iyalosovetsky/RP2040_pico2w) on a BTT SKR Pico.

The SKR Pico has no WiFi and its grblHAL console is only reachable over USB CDC. This
firmware runs on a separate **Raspberry Pi Pico 2 W**, which:

1. Acts as a **USB host** and plugs into the SKR Pico's USB port, talking to grblHAL's
   console as a CDC-ACM client (send G-code lines, get `ok`/`error`/status back).
2. Serves a **web page** and a small **REST API** over WiFi to send G-code and see the
   machine's current state (status, MPos/WPos, feed/speed, a live console log).

## Architecture

- **Core 1** owns the USB-host link exclusively (`src/usb_host_cdc.c`, `src/grbl_link.c`):
  pumps TinyUSB's host stack, sends queued G-code lines one at a time with the normal
  `ok`/`error:` handshake, sends real-time bytes (`?`/`!`/`~`/soft-reset) out of band, and
  polls `?` every 250 ms to keep the machine state fresh.
- **Core 0** owns WiFi and the HTTP server (`src/wifi_config.c`, `src/http_server.c`,
  `src/api_handlers.c`): a minimal non-blocking HTTP/1.0 server on lwIP's raw TCP API,
  no RTOS, no sockets layer.
- The two cores only talk through `src/shared_state.c` (mutex-protected machine state,
  console log ring buffer, outbound G-code queue, real-time request flags).

Axis count is **not** hardcoded to X/Y/Z/A: in this custom grblHAL build the turntable
(`M102`-`M104`) and the ST3215 tilt servo (`M101`) are driven independently of grbl's
motion planner and never appear in the `<...>` status report — see `rotary_table.c` /
`st3215.c`. The bridge just parses however many axes `MPos:`/`WPos:` actually reports
(whatever grbl axes exist on your build, typically the scanner's X/Y carriage) and treats
M101-M104 as plain G-code text like everything else sent through `/api/gcode`.

## Building

Requires `arm-none-eabi-gcc`, `cmake`, and a checked-out `pico-sdk` (2.1+, RP2350/Pico 2 W
support) with submodules `lib/tinyusb`, `lib/lwip`, `lib/cyw43-driver` initialized.

```bash
mkdir build && cd build
cmake -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=pico2_w ..
make -j$(nproc)
```

Produces `build/bridge.uf2`. Flash it by holding BOOTSEL while plugging the Pico 2 W into
your PC (a *second* USB connection, just for flashing — see wiring below), then copy
`bridge.uf2` onto the `RPI-RP2` drive that appears.

## Wiring

The Pico 2 W's own USB connector is used in **host** role to talk to the SKR Pico, so it
needs a USB-A **host/OTG adapter** on that port (same approach already proven working on
this bench for USB-host HID in `pico_read_usb_keyboard`). Two things to get right:

1. **VBUS power.** A host port normally *supplies* 5V to the device; the Pico's own USB
   connector doesn't do that by default (VBUS is wired as an input). If the SKR Pico
   doesn't enumerate, check that your OTG adapter/cable injects 5V onto VBUS (a powered
   USB hub between the two boards is the simplest fix if a bare OTG adapter doesn't work).
   The SKR Pico is normally powered independently from its 12/24V input, so this is only
   about USB enumeration, not board power.
2. **Debug console.** With the native USB port in host role, `printf` output comes out
   UART0 instead (GPIO0 = TX, GPIO1 = RX, 115200 8N1) — wire a USB-serial adapter there
   to watch boot/connection logs, or check `[link]`/`[wifi]`/`[usb]` lines from a UART
   terminal during bring-up.

Bring-up order to sanity-check the hardware before trusting the web UI: flash, open a
UART terminal, plug in the SKR Pico (running grblHAL) via the OTG adapter, and look for
`[usb] CDC mounted` — if that never appears, it's the VBUS issue above, not a software bug.

## WiFi setup

On first boot (no saved credentials) the bridge starts its own open access point,
**`ScannerRig-xxxx`**, serving everything at `http://192.168.4.1/`. Configure your home
network via `POST /api/wifi` (see below) — the board saves the credentials to flash and
reboots into station mode. If it can't reconnect (wrong password, AP out of range), it
automatically falls back to its own AP again after a 15s timeout, so it's never stranded.

## REST API

All bodies are plain text (no JSON payloads to build by hand), responses are JSON.

| Method | Path | Body | Notes |
|---|---|---|---|
| GET | `/` | — | The web UI |
| GET | `/api/status` | — | `{connected, status, naxes, mpos[], has_wpos, wpos[], feed, speed, alarm, wifi_mode, ap_ssid, console[]}` |
| POST | `/api/gcode` | one command per line | Queued and sent to grblHAL one line at a time; `{total, queued, rejected}` |
| POST | `/api/hold` | — | Real-time feed hold (`!`) |
| POST | `/api/resume` | — | Real-time cycle resume (`~`) |
| POST | `/api/reset` | — | Real-time soft reset (`Ctrl-X`) |
| GET | `/api/wifi` | — | `{mode, ap_ssid}` |
| POST | `/api/wifi` | `ssid\npassword` | Saves credentials, reboots into STA mode |

Example:

```bash
curl -d $'G91\nG1 X5 F300' http://<bridge-ip>/api/gcode
curl http://<bridge-ip>/api/status
```

## Status

Builds and links cleanly (`arm-none-eabi-gcc` 14.2, pico-sdk 2.3.0). Not yet flashed to
real hardware — the USB-host VBUS question above is the main open risk to verify first.
