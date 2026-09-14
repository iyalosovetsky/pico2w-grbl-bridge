# pico2w-grbl-bridge

WiFi/REST bridge for the [scanner turntable rig](https://github.com/iyalosovetsky/rotary-pico)
running a [custom grblHAL](https://github.com/iyalosovetsky/RP2040_pico2w) on a BTT SKR Pico.

The SKR Pico has no WiFi and its grblHAL console is only reachable over USB CDC. This
firmware runs on a separate **Raspberry Pi Pico W or Pico 2 W** (RP2040 or RP2350 — same
source tree, see Building), which:

1. Acts as a **USB host** on its own PIO-driven USB port (separate GPIO pins, *not* the
   board's native USB connector) and plugs into the SKR Pico's USB port, talking to
   grblHAL's console as a CDC-ACM client (send G-code lines, get `ok`/`error`/status back).
2. Serves a **web page** and a small **REST API** over WiFi to send G-code and see the
   machine's current state (status, MPos/WPos, feed/speed, a live console log).

The board's *native* USB port is deliberately left alone: it's still what you use to
flash the board and to watch `printf` debug output over a normal serial terminal — the
grblHAL link runs entirely over a second, PIO-emulated USB port instead
([Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB), vendored as a submodule).

## Architecture

- **Core 1** owns the USB-host link exclusively (`src/usb_host_cdc.c`, `src/grbl_link.c`):
  pumps TinyUSB's host stack over Pico-PIO-USB, sends queued G-code lines one at a time
  with the normal `ok`/`error:` handshake, sends real-time bytes (`?`/`!`/`~`/soft-reset)
  out of band, and polls `?` every 250 ms to keep the machine state fresh.
- **Core 0** owns WiFi, the HTTP server, and the native USB device stack (stdio-over-CDC
  debug console): `src/wifi_config.c`, `src/http_server.c`, `src/api_handlers.c` — a
  minimal non-blocking HTTP/1.0 server on lwIP's raw TCP API, no RTOS, no sockets layer.
- The two cores only talk through `src/shared_state.c` (mutex-protected machine state,
  console log ring buffer, outbound G-code queue, real-time request flags).
- TinyUSB runs in both roles at once — device (native controller, rhport 0, stdio) and
  host (PIO-USB, rhport 1, grblHAL) — the same pattern as Pico-PIO-USB's own
  `host_hid_to_device_cdc` example. The PIO-USB host is explicitly pinned to PIO1
  (`usb_host_cdc.c`) so it can never collide with the PIO block the CYW43 WiFi driver
  claims dynamically for its SPI-over-PIO link to the wireless chip.

Axis count is **not** hardcoded to X/Y/Z/A: in this custom grblHAL build the turntable
(`M102`-`M104`) and the ST3215 tilt servo (`M101`) are driven independently of grbl's
motion planner and never appear in the `<...>` status report — see `rotary_table.c` /
`st3215.c`. The bridge just parses however many axes `MPos:`/`WPos:` actually reports
(whatever grbl axes exist on your build, typically the scanner's X/Y carriage) and treats
M101-M104 as plain G-code text like everything else sent through `/api/gcode`.

## Building

Requires `arm-none-eabi-gcc`, `cmake`, `python3`, and a checked-out `pico-sdk` (2.1+, with
RP2350/Pico 2 W board support) with submodules `lib/tinyusb`, `lib/lwip`,
`lib/cyw43-driver` initialized. Clone this repo with `--recurse-submodules` (or
`git submodule update --init`) to pull in the vendored Pico-PIO-USB library.

```bash
git clone --recurse-submodules <this repo>
mkdir build && cd build
cmake -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=pico2_w ..   # or -DPICO_BOARD=pico_w for a plain Pico W (RP2040)
make -j$(nproc)
```

Same source tree, same CMakeLists.txt — `PICO_BOARD` is the only thing that changes
between an RP2040 Pico W and an RP2350 Pico 2 W. Produces `build/bridge.uf2`. Flash it by
holding BOOTSEL while plugging the board's *native* USB port into your PC, then copy
`bridge.uf2` onto the `RPI-RP2` drive that appears — the same port stays usable for that
and for `printf` debugging afterwards (it enumerates as a normal CDC serial device).

## Wiring

The grblHAL link does **not** use the board's native USB connector — it uses a second,
PIO-emulated USB port on separate GPIO pins (`PIO_USB_HOST_DP_PIN` in `usb_host_cdc.c`,
default **GP0 = D+, GP1 = D-**), so the native port is free for flashing/debug. Build a
USB-A host connector wired to:

| Signal | Pin |
|---|---|
| D+ | GP0 (configurable) |
| D- | GP1 (D+ pin + 1, fixed by the library's default pinout) |
| VBUS (5V) | An external 5V source, or the board's own VSYS/5V pin |
| GND | GND |

22 Ω series resistors on D+/D- are recommended (standard USB signal integrity practice,
per Pico-PIO-USB's own docs). Unlike a native-port OTG adapter, **you provide VBUS
yourself here** — wire real 5V to it, since the SKR Pico's USB side needs to see 5V to
enumerate at all (it's independently powered from its 12/24V input otherwise, this is
only about USB enumeration). This whole port is a fresh breakout — the prior VBUS-via-
native-port headache from an earlier version of this design doesn't apply anymore, since
D+/D-/5V/GND are now just GPIO/power pins you wire explicitly rather than fighting the
native connector's fixed, input-only VBUS pin.

Bring-up order to sanity-check the hardware before trusting the web UI: flash, open a
serial terminal on the board's native USB CDC port, plug in the SKR Pico (running
grblHAL) via the PIO-USB host wiring above, and look for `[usb] CDC mounted` — if that
never appears, recheck the VBUS wiring above before suspecting a software bug.

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

Builds and links cleanly for both `pico2_w` (RP2350) and `pico_w` (RP2040) targets
(`arm-none-eabi-gcc` 14.2, pico-sdk 2.3.0). Not yet flashed to real hardware — the VBUS
wiring above is the main open item to verify first.
