# pico2w-grbl-bridge

WiFi/REST bridge for the [scanner turntable rig](https://github.com/iyalosovetsky/rotary-pico)
running a [custom grblHAL](https://github.com/iyalosovetsky/RP2040_pico2w) on a BTT SKR Pico.

The SKR Pico has no WiFi and its grblHAL console is only reachable over USB CDC. This
firmware runs on a separate **Raspberry Pi Pico W or Pico 2 W** (RP2040 or RP2350 — same
source tree, see Building), which:

1. Acts as a **USB host**, via a plain USB-A OTG adapter on its own built-in micro-USB
   port, and plugs into the SKR Pico's USB port, talking to grblHAL's console as a
   CDC-ACM client (send G-code lines, get `ok`/`error`/status back).
2. Serves a **web page** and a small **REST API** over WiFi to send G-code and see the
   machine's current state (status, MPos/WPos, feed/speed, a live console log).

Since the board's native USB port is occupied by the host role, `printf` debug output
goes out **UART0** instead (GPIO0=TX, GPIO1=RX, 115200 8N1) — see Wiring.

Confirmed working end-to-end on real hardware (SKR Pico + Pico W) as of 2026-09-15.

## Architecture

- **Core 1** owns the USB-host link exclusively (`src/usb_host_cdc.c`, `src/grbl_link.c`):
  pumps TinyUSB's host stack on the native USB controller, sends queued G-code lines one
  at a time with the normal `ok`/`error:` handshake, sends real-time bytes
  (`?`/`!`/`~`/soft-reset) out of band, and polls `?` every 250 ms to keep the machine
  state fresh.
- **Core 0** owns WiFi and the HTTP server: `src/wifi_config.c`, `src/http_server.c`,
  `src/api_handlers.c` — a minimal non-blocking HTTP/1.0 server on lwIP's raw TCP API, no
  RTOS, no sockets layer.
- The two cores only talk through `src/shared_state.c` (mutex-protected machine state,
  console log ring buffer, outbound G-code queue, real-time request flags).

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
`lib/tinyusb`, `lib/lwip`, `lib/cyw43-driver` initialized.

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
tools/build.sh                               # pico2_w (RP2350)
tools/build.sh --board pico_w                # a plain Pico W (RP2040) instead
tools/build.sh --flash                       # build, then flash over picotool
tools/build.sh --help                        # all options
```

`tools/build.sh` is a thin wrapper: `cmake -S . -B build-<board> -DPICO_SDK_PATH=... -DPICO_BOARD=... && cmake --build build-<board>`,
plus `picotool load -u -v -x build-<board>/bridge.uf2` for `--flash`. Same source tree,
same CMakeLists.txt for both chips — only `PICO_BOARD` changes between an RP2040 Pico W
and an RP2350 Pico 2 W. `--flash` needs the board already in BOOTSEL mode (hold BOOTSEL
while plugging in its native USB port) — this firmware doesn't expose picotool's USB
reset interface, so it can't reboot itself into BOOTSEL remotely. Without `--flash`, copy
the resulting `build-<board>/bridge.uf2` onto the `RPI-RP2` drive by hand instead.

Once flashed, that same native port switches to host role (see Wiring) — plug it into a
PC again (BOOTSEL) any time you need to reflash.

## Wiring

**grblHAL link (USB host):** a plain, passive USB-A OTG adapter on the board's own
built-in micro-USB port, cabled to the SKR Pico's USB port — no extra wiring, same
approach already proven on this bench for USB-host HID in `pico_read_usb_keyboard`. If
your adapter/cable doesn't pass real 5V through to VBUS and the SKR Pico doesn't
enumerate, try one that does (a short USB-A extension or hub works) — but in testing here
a bare passive OTG adapter was enough: grblHAL's own USB stack forces its VBUS-detect on
in firmware regardless of the physical pin (`dcd_rp2040.c`), and the native RP2040/RP2350
host controller's device-attach detection is pure D+/D- line-state sensing, not gated on
VBUS either. So it isn't a hard requirement here the way it can be on some other
host/device combinations — if you do hit "nothing ever attaches," it's still the first
thing to try, just not a given.

**Debug console:** since the native USB port is busy with the host role, `printf` goes
out **UART0** instead — GPIO0 = TX, GPIO1 = RX, 115200 8N1. Wire a USB-serial adapter
there to watch boot/connection logs.

Bring-up order to sanity-check the hardware before trusting the web UI: flash, open a
UART terminal, plug in the SKR Pico (running grblHAL) via the OTG adapter, and look for
`[usb] CDC mounted` — if that never appears, `[usb] still no device on the USB host
port` will keep repeating every 10s as a live reminder that nothing has attached yet
(check the OTG cable/adapter itself before suspecting a software bug).

Every boot prints a banner to that same console — what firmware is actually running is
often the first thing worth checking, especially after a few `--flash`es in a row:

```
========================================
 Scanner Rig Bridge
 Built: 2026-09-15 09:12:03 UTC (build #12, e4bb8bc)
========================================
...
[main] Ready — mode: STA  ip: 10.80.39.126  http://10.80.39.126/
```

`build #N` is the repo's commit count at build time (`git rev-list --count HEAD`), so it
climbs with every commit; `-dirty` means uncommitted changes were present when you built.

### Onboard LED

Without a console attached, the onboard LED (`src/led.c`) is the only feedback there is:

| Pattern | Meaning |
|---|---|
| Fast blink (~150ms) | Booting, no WiFi IP yet |
| Three short flashes, once | Just got an IP (STA connected, or its own AP came up) |
| Off, brief pulse every 20th line | Normal operation — pulses once per 20 grblHAL status reports received (a "still talking to grblHAL" heartbeat; `LED_HEARTBEAT_EVERY_N_LINES` in `shared_state.h`) |

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

Working end-to-end on real hardware (Pico W + SKR Pico running grblHAL): USB host link,
WiFi, REST API, and the web UI all confirmed as of 2026-09-15. Builds cleanly for both
`pico2_w` (RP2350) and `pico_w` (RP2040) targets.
