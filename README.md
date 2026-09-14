# pico2w-grbl-bridge

> **`native-usb-host` branch.** Trying the board's built-in micro-USB port (via a USB-A
> OTG adapter) as the link to grblHAL, instead of `main`'s separate PIO-USB port on
> custom pins — first-pass hardware test, debug wiring to be finished later. Concretely,
> vs. `main`: the native USB port is host-only (no more stdio-over-CDC device role,
> `src/usb_descriptors.c` removed), debug `printf` output goes out **UART0** (GPIO0=TX,
> GPIO1=RX, 115200 8N1) instead, and `tools/build.sh` has no `--dp-pin`/`--pio` (nothing
> left to configure there). The rest of this README — REST API, WiFi setup, TBL/TBLABS/
> ST3215 parsing — is unchanged and still describes `main`'s PIO-USB wiring in the
> Wiring section below; ignore that section here and use a plain USB-A OTG adapter on
> the native port instead, same as any standard "Pico as USB host" setup.

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
motion planner, so they're not grbl axes and don't show up in `MPos:`/`WPos:` — see
`rotary_table.c` / `st3215.c`. The bridge just parses however many axes `MPos:`/`WPos:`
actually reports (whatever grbl axes exist on your build, typically the scanner's X/Y
carriage) and treats M101-M104 as plain G-code text like everything else sent through
`/api/gcode`.

The turntable and servo state itself is picked up from three extra status-report fields
this rig's grblHAL fork adds for exactly that reason — `TBL` (turntable angle, wrapped to
0-360°), `TBLABS` (turntable angle, unwrapped total rotation) and `ST3215` (tilt servo
angle), e.g. `<Idle|MPos:0.000,0.000,0.000|Bf:100,1023|FS:0,0|Ov:100,100,100|TBL:342.01|TBLABS:1062.01|ST3215:151.21>`.
Parsed in `grbl_link.c`'s `parse_status_report()` alongside `MPos`/`WPos`/`FS`, exposed as
`table`/`table_abs`/`servo` in `/api/status` (`null` if your build doesn't report them),
and shown on the web page next to the axis positions.

## Building

Requires `arm-none-eabi-gcc`, `cmake`, `python3`, `picotool` (only for `--flash`), and a
checked-out `pico-sdk` (2.1+, with RP2350/Pico 2 W board support) with submodules
`lib/tinyusb`, `lib/lwip`, `lib/cyw43-driver` initialized. Clone this repo with
`--recurse-submodules` (or `git submodule update --init`) to pull in the vendored
Pico-PIO-USB library.

```bash
git clone --recurse-submodules <this repo>
export PICO_SDK_PATH=/path/to/pico-sdk
tools/build.sh                               # pico2_w (RP2350), default PIO-USB pins
tools/build.sh --board pico_w                # a plain Pico W (RP2040) instead
tools/build.sh --dp-pin 16 --pio 0           # different PIO-USB pins (see Wiring)
tools/build.sh --flash                       # build, then flash over picotool
tools/build.sh --help                        # all options
```

`tools/build.sh` is a thin wrapper: `cmake -S . -B build-<board> -DPICO_SDK_PATH=... -DPICO_BOARD=... -DPIO_USB_HOST_DP_PIN=... -DPIO_USB_HOST_PIO_INDEX=... && cmake --build build-<board>`,
plus `picotool load -u -v -x build-<board>/bridge.uf2` for `--flash`. Same source tree,
same CMakeLists.txt for both chips — only `PICO_BOARD` changes between an RP2040 Pico W
and an RP2350 Pico 2 W. `--flash` needs the board already in BOOTSEL mode (hold BOOTSEL
while plugging in its *native* USB port) — this firmware doesn't expose picotool's USB
reset interface, so it can't reboot itself into BOOTSEL remotely. Without `--flash`, copy
the resulting `build-<board>/bridge.uf2` onto the `RPI-RP2` drive by hand instead. Either
way, that same native port stays usable afterwards for `printf` debugging (it enumerates
as a normal CDC serial device).

## Wiring

The grblHAL link does **not** use the board's native USB connector — it uses a second,
PIO-emulated USB port on separate GPIO pins, so the native port is free for flashing/debug.
Pins default to **GP0 = D+, GP1 = D-**, overridable without editing source via
`tools/build.sh --dp-pin <gpio> --pio <0|1|2>` (or `cmake -DPIO_USB_HOST_DP_PIN=... -DPIO_USB_HOST_PIO_INDEX=...`
directly — see `usb_host_cdc.c`). Build a USB-A host connector wired to:

| Signal | Pin |
|---|---|
| D+ | GP0 (`--dp-pin`) |
| D- | GP1 (always D+ pin + 1, fixed by the library's default pinout) |
| **VBUS (5V)** | **An external 5V source, or the board's own VSYS/5V pin — see below** |
| GND | GND |

> **D+/D- alone are not enough.** Without a real 5V wired to VBUS, the SKR Pico never
> sees a host connect and nothing will ever mount — no error, just permanent silence
> from `[usb]` on the debug console and `connected: false` on the web page. This is by
> far the most common reason grblHAL appears unreachable; check it before anything else.

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
grblHAL) via the PIO-USB host wiring above, and look for `[usb] CDC mounted`. If instead
you see `[usb] still no device on the PIO-USB host port` repeating every 10s (and never
even `[usb] device attached`, which fires for *any* USB device regardless of class,
before CDC-specific enumeration) — that's the VBUS wiring above, not a software bug.

Every boot prints a banner to that same console — what firmware is actually running is
often the first thing worth checking, especially after a few `--flash`es in a row:

```
========================================
 Scanner Rig Bridge
 Built: 2026-09-14 20:35:21 UTC (build #5, e6552c2-dirty)
========================================
...
[main] Ready — mode: STA  ip: 192.168.1.42  http://192.168.1.42/
```

`build #N` is the repo's commit count at build time (`git rev-list --count HEAD`), so it
climbs with every commit regardless of branch; `-dirty` means uncommitted changes were
present when you built (as they were above — this is expected while developing).

## WiFi setup

Two ways to give the bridge WiFi credentials, and they compose:

1. **At runtime, over the web UI** (`POST /api/wifi`, see below): on first boot, or
   whenever it has no working saved network, the bridge starts its own open access
   point, **`ScannerRig-xxxx`**, serving everything at `http://192.168.4.1/`. Configure
   your home network from there — the board saves the credentials to flash and reboots
   into station mode. If it can't reconnect (wrong password, AP out of range), it falls
   back to its own AP again after a 15s timeout, so it's never stranded.
2. **At build time** (`tools/build.sh --wifi-ssid <ssid> --wifi-password <password>`,
   or `-DWIFI_SSID=... -DWIFI_PASSWORD=...`): baked into the firmware as a fallback used
   only until something is saved to flash. Handy so a freshly-flashed board joins your
   network immediately instead of needing the AP-config dance every time during
   development. The first time it connects using this, it's saved to flash exactly like
   an `/api/wifi` update — **flash always wins from then on**, even across a later
   reflash with a build that has no baked-in default (or a different one).

`--wifi-password` on the command line lands in shell history; export `WIFI_SSID`/
`WIFI_PASSWORD` as environment variables instead if that matters to you — `build.sh`
picks them up automatically.

## REST API

All bodies are plain text (no JSON payloads to build by hand), responses are JSON.

| Method | Path | Body | Notes |
|---|---|---|---|
| GET | `/` | — | The web UI |
| GET | `/api/status` | — | `{connected, status, naxes, mpos[], has_wpos, wpos[], feed, speed, alarm, table, table_abs, servo, wifi_mode, ap_ssid, console[]}` |
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
