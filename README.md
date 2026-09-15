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
  (`?`/`!`/`~`/soft-reset) out of band, and polls `?` every 500 ms to keep the machine
  state fresh (`STATUS_POLL_INTERVAL_MS` — this rig doesn't need finer-grained polling
  than that).
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

The active work coordinate system (`G54`..`G59.3`) is picked up the same way, from an
optional `|WCS:G54|` status field — but unlike `TBL`/`ST3215`, grblHAL only includes it
right after it *changes* (a `G54`-`G59` select, `G10 L2`/`L20`, ...), not on every report,
same as `WCO` itself. `grbl_link.c` caches the last one seen (same pattern as its `WCO`
cache) and also seeds it once per USB mount with a one-shot `$G` parser-state query
(parsing `[GC:G0 G54 G17 ...]` for the `G5x` token), so the web page's "WCS:" readout in
the status card shows something correct even before any coordinate-system command has
actually been sent. Exposed as `wcs` in `/api/status` (`null` until known).

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
 Scanner Rig Bridge v0.1
 Built: 2026-09-15 09:12:03 UTC (build #12, e4bb8bc)
========================================
...
[main] Ready — mode: STA  ip: 10.80.39.126  http://10.80.39.126/
```

The version comes from the `VERSION` file at the repo root (bump it and commit to cut a
new release). `build #N` is the repo's commit count at build time (`git rev-list --count
HEAD`), so it climbs with every commit regardless of version; `-dirty` means uncommitted
changes were present when you built.

### Onboard LED

Without a console attached, the onboard LED (`src/led.c`) is the only feedback there is:

| Pattern | Meaning |
|---|---|
| Fast blink (~150ms) | Booting, no WiFi IP yet |
| Three short flashes, once | Just got an IP (STA connected, or its own AP came up) |
| Off, brief pulse every 10th line | Normal operation — pulses once per 10 grblHAL status reports received (a "still talking to grblHAL" heartbeat; `LED_HEARTBEAT_EVERY_N_LINES` in `shared_state.h` — halved alongside the poll interval so the real-world blink rate is unchanged) |

### Recovering from a stalled USB link

A flaky cable can wedge core1's USB/TinyUSB state (seen in practice) without crashing
outright — `usb_host_cdc_task()` keeps returning, but grblHAL is never heard from again.
`main.c` arms the hardware watchdog (`hardware_watchdog`, ~8s) once WiFi is up, and core1
bumps a plain counter every pass of its own loop regardless of whether grblHAL responds
(`shared_state_core1_tick()`); core0 only calls `watchdog_update()` while that counter is
still advancing. If it stalls for 3s, core0 stops feeding the watchdog on purpose and the
chip resets itself a few seconds later — the same clean re-init as a power cycle, since
that's the most reliable way to recover a wedged USB peripheral. Also covers a core0-only
hang for free: if core0 itself wedges, `watchdog_update()` simply stops being called.
`watchdog_enable_caused_reboot()` logs whether the last boot was one of these recoveries.

The web page has its own independent staleness check for the same failure, since a stall
this brief might not even reach the watchdog's timeout, or the HTTP server itself could
be the thing that's stuck: if the board's own `last_status_ms` hasn't advanced in 3s, the
page shows "з'єднання втрачене" and blanks the status/position fields instead of a
frozen "Idle" that looks fine but isn't (`STALE_MS` in `web/index.html`).

A separate, real bug hit the board itself: `refreshStatus()`'s `setInterval` had no
re-entrancy guard, so a poll that took longer than 500ms (more likely with the "full"
console view's constantly-changing, larger content) let the next tick fire an overlapping
`fetch()` on top of it — and sending a command made this far more likely, since it fired
an *extra* immediate `refreshStatus()` right next to whatever the timer was about to do
anyway. Enough overlapping connections against a server with only a small TCP backlog
(`tcp_listen_with_backlog`, bumped 4 → 8 as extra headroom) could wedge it. Fixed with a
`statusFetchInFlight` flag that skips a tick outright if the previous one hasn't finished.

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

## Jog dials

The web page's jog card lets you nudge all four moving parts with the mouse wheel
instead of typing G-code — hover a widget and scroll (a hover tooltip on the card
itself repeats this). The card is laid out like the rig itself: a wide **Y** bar across
the top, a tall **Z** bar down the right edge, and underneath the Y bar the tilt servo's
circle tucked into the top-right corner with the turntable's ellipse spreading out
beneath/behind it to fill the rest of the space:

![The web page: state card, G-code sender, jog dials (Y bar on top, Z bar on the right,
tilt circle tucked into the top-right corner, turntable ellipse filling the rest), and
console](docs/jog-dials.png)

| Widget | Shape | Sends | Range |
|---|---|---|---|
| Стіл (turntable) | Elongated ellipse, needle | `M104 Q<delta>` (relative — grblHAL accumulates it) | none (continuous rotation) |
| Tilt (servo) | Partial arc, needle | `M101 Q<absolute>` (current + delta, clamped) | 130°-235° ($451/$452) |
| Y | Wide bar (top) | `$J=G91 Y<delta> F300` (jog) | — |
| Z | Tall bar (right edge) | `$J=G91 Z<delta> F300` (jog) | — |
| ← / → (in the Y bar) | Small round buttons, left/right edge of the Y bar | `$J=G91 Y50 F3000` / `$J=G91 Y-50 F3000` — a fixed 50mm jog, one click | — |
| 0 (in the Y bar) | Small accent-colored button, bottom-right corner of the Y bar | `G54` then `G10 L20 P1 Y0` — zero Y in G54 | — |
| ↑ / ↓ (in the Z bar) | Small round buttons, top/bottom edge of the Z bar | `$J=G91 Z20 F600` / `$J=G91 Z-20 F600` — a fixed 20mm jog, one click | — |
| 0 (in the Z bar) | Small accent-colored button, bottom-right corner of the Z bar | `G54` then `G10 L20 P1 Z0` — zero Z in G54 | — |

F3000 on the ←/→ buttons isn't arbitrary — it matches this rig's Y-axis `$111` (max
rate) setting, raised from its default 1000 to 3000 directly on the grblHAL controller
(along with `$121`, acceleration, 10 -> 30) after a first attempt at just raising the
bridge-side F alone (600 -> 1800) turned out to change almost nothing: `$J` silently
clamps to whatever `$111` allows, and 1000 was the real ceiling the whole time. Those
two settings live in grblHAL's own EEPROM on the SKR Pico, not anywhere in this repo —
re-tune with `$111=<rate>`/`$121=<accel>` (sent as plain G-code through the bridge) if
the rig's real-world limits change, and keep the buttons' F here in sync.

The turntable is drawn as a flat ellipse (`rx` >> `ry`, `polarToEllipseXY()`) rather than
a plain circle, since it's a table lying flat and this gives it an isometric look instead
of a face-on one. It's absolutely positioned to share the same box as the tilt circle
(`.dial-main` in `web/index.html`) instead of stacking below it in a flex row — an oval
doesn't fill the rectangular corner it sits in, so it spreads underneath/behind that
corner and uses nearly the whole card instead of just the leftover strip beneath the
circle.

### Continuous rotation

Below the turntable ellipse are three more controls, for spinning it indefinitely
instead of nudging it by a fixed amount — separate from the M104 wheel-jog above:

| Button | Position | Sends |
|---|---|---|
| ↺ | bottom-left | `M102 P1` — start spinning CCW; click again to stop |
| ↻ | bottom-right (of the ellipse) | `M102 P0` — start spinning CW; click again to stop |
| ■ | bottom-right corner (of the whole widget) | `M103` — stop unconditionally, regardless of tracked state |

Clicking one direction while the other is already spinning just switches direction
(grblHAL's `M102` takes over the motor immediately, no need to stop first). The active
direction button is highlighted (`.rotate-btn.active` in `web/index.html`); that
highlight — and the underlying `tableRotation` JS state it reflects — is client-side
only (the status report has no "is spinning continuously" field to poll), so it's also
cleared whenever the wheel-jog or a soft reset is used, since both actually stop the
motor on the real hardware regardless of what the buttons last did.

All four debounce wheel input (~220ms after you stop scrolling) into a single command
per gesture rather than one per wheel tick — `attachWheelCommand()` in `web/index.html`.
For the turntable and servo, scrolling away from you (up) decreases the angle; toward
you (down) increases it. **Y and Z are inverted relative to that** (their
`attachWheelCommand()` calls pass a negative sensitivity) — toward you (down) moves
negative, away from you (up) moves positive.

The two circular dials share one screen-angle convention (`polarToXY()`): 0° points
right (3 o'clock / positive X-axis, not 12 o'clock), and increasing angle sweeps
**counter-clockwise** — standard math convention, not clock convention. The servo's arc
maps its hardware range onto clock-face positions per this rig's actual travel (130° =
scanner horizontal = "9:30", 235° = scanner down = "5:00"), swept through the bottom of
the dial rather than the top.

## REST API

All bodies are plain text (no JSON payloads to build by hand), responses are JSON.

| Method | Path | Body | Notes |
|---|---|---|---|
| GET | `/` | — | The web UI |
| GET | `/api/status` | — | `{connected, status, naxes, mpos[], has_wpos, wpos[], feed, speed, alarm, table, table_abs, servo, wcs, wifi_mode, ap_ssid, last_status_ms, console[], console_filtered[], console_filtered_seq[]}` |
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

`console` is a full, unfiltered transcript (every line sent and received, including raw
`<...>` status reports every 500ms). `console_filtered` is the same transcript minus the
status-report firehose: sent commands, everything else grblHAL says (`ok`/`error:`/
`ALARM:`/`[MSG:...]`/`$$` dumps/...), and — on an actual state transition (e.g. `Idle`
becoming `Run`) or right after a user command even when the status word didn't change —
the raw status report itself, verbatim, not paraphrased, so a manually sent `?` still
shows its real `MPos`/`TBL`/`ST3215`/... rather than just the state word. See
`parse_status_report()`/`force_next_status_log` in `grbl_link.c`. The web page's console
panel shows one at a time with a toggle (`☰`/`…`) and can be hidden entirely; both ship
in every `/api/status` response so switching is instant, no extra request.

`console_filtered` sends the newest 30 lines each poll (`STATUS_FILTERED_LINES` in
`api_handlers.c`) — comfortably inside the ring buffer's own 32-line capacity
(`CONSOLE_LOG_LINES` in `shared_state.h`, unchanged) and the 7168-byte response buffer
(`RESP_BUF_SIZE` in `http_server.c`, bumped from 6144 to fit it), so this cost no extra
static RAM on the board itself. Each line also carries a monotonically increasing
sequence number in the parallel `console_filtered_seq` array
(`shared_state_filtered_push()`), and the web page accumulates every new-sequence line
into its own up-to-40-line scrollback (`FILTERED_SCROLLBACK_MAX`/`mergeFilteredLines()`
in `web/index.html`) instead of replacing the displayed list wholesale every poll — the
30-line-per-poll window just makes it far less likely for a burst of activity to
produce more new lines than a single 500ms poll can see (previously 12, an easy number
to exceed). If it ever does, the extras are skipped; if the sequence number goes
backwards — the board rebooted and restarted counting from 1 — the page detects that and
starts its scrollback over rather than getting stuck treating every future line as
"already seen".

`alarm` holds the text of the last `error:` or `ALARM:` line and is what the web page's
red banner shows — it's cleared by the *next user command* (any `/api/gcode`, `/api/hold`,
`/api/resume`, or `/api/reset` call), not automatically when grblHAL's status moves on, so
a notice doesn't disappear before you've actually seen it.

## Status

Working end-to-end on real hardware (Pico W + SKR Pico running grblHAL): USB host link,
WiFi, REST API, and the web UI all confirmed as of 2026-09-15. Builds cleanly for both
`pico2_w` (RP2350) and `pico_w` (RP2040) targets.
