# claude-desktop-buddy

[![CI](https://github.com/ttpears/claude-desktop-buddy/actions/workflows/ci.yml/badge.svg)](https://github.com/ttpears/claude-desktop-buddy/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ttpears/claude-desktop-buddy?sort=semver)](https://github.com/ttpears/claude-desktop-buddy/releases/latest)
[![License: MIT](https://img.shields.io/github/license/ttpears/claude-desktop-buddy)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-M5StickC%20Plus%20%C2%B7%20M5StickS3-blue)](platformio.ini)
[![Bridge](https://img.shields.io/badge/bridge-ttpears%2Fbuddy--bridge-orange)](https://github.com/ttpears/buddy-bridge)

> **This is the [ttpears](https://github.com/ttpears) fork** of
> [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy).
> It carries extra battery-life and UI work on top of upstream and ships
> **flashable firmware images** in [Releases](https://github.com/ttpears/claude-desktop-buddy/releases).
> It's the stick-side half of [**ttpears/buddy-bridge**](https://github.com/ttpears/buddy-bridge),
> which drives this firmware from Claude **Code** sessions across many machines
> (the desktop-app path below still works unchanged). Unofficial; not affiliated
> with or endorsed by Anthropic.

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet on ESP32 that lives off permission
approvals and interaction with Claude. It sleeps when nothing's happening,
wakes when sessions start, gets visibly impatient when an approval prompt is
waiting, and lets you approve or deny right from the device.

<p align="center">
  <img src="docs/device.jpg" alt="M5StickC Plus running the buddy firmware" width="500">
</p>

## Hardware

The firmware runs on **two boards from one codebase**, built on the
[M5Unified](https://github.com/m5stack/M5Unified) library:

- **M5StickC Plus** — ESP32 + AXP192 PMIC. The original target. Its AXP192
  coulomb counter powers the battery page's hardware-integrated average-draw
  and runtime estimate.
- **M5StickS3** — ESP32-S3 + M5PM1 PMIC, ES8311 speaker, native USB-C. It has
  no RTC (the clock runs in software, synced from the bridge), no coulomb
  counter (battery page shows a voltage-based estimate only), and no user LED
  (the attention blink is a no-op).

Each board is its own PlatformIO environment — `m5stickc-plus` and
`m5sticks3`. The hardware that differs between them is isolated in
`src/board.{h,cpp}` behind the `BOARD_STICKC_PLUS` / `BOARD_STICKS3` build
flags, so porting to another ESP32/ESP32-S3 board mostly means adding a
backend there.

## Flashing

Pick the PlatformIO environment for your board: **`m5stickc-plus`** or
**`m5sticks3`**. (With two environments you must pass `-e` — a bare
`pio run -t upload` would try to build/flash both.)

**From source:** install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then build + upload your board's env:

```bash
pio run -e m5stickc-plus -t upload   # M5StickC Plus
pio run -e m5sticks3     -t upload   # M5StickS3
```

The M5StickS3 connects over **native USB-C** — it enumerates as a USB
serial/JTAG port, no external USB-serial adapter needed. If GIF pets don't
appear, upload the filesystem image once too:

```bash
pio run -e <env> -t uploadfs
```

Starting from a previously-flashed device, wipe it first:

```bash
pio run -e <env> -t erase && pio run -e <env> -t upload
```

Once running, you can also wipe everything from the device itself: **hold A
→ settings → reset → factory reset → tap twice**.

**From a release (no toolchain):** prebuilt images currently target the
**M5StickC Plus** (ESP32). Download the latest
[release](https://github.com/ttpears/claude-desktop-buddy/releases/latest) and
flash with [esptool](https://github.com/espressif/esptool) (verify against
`SHA256SUMS` first). Which image to use depends on whether the device is fresh
or already paired:

```bash
# FRESH / blank device — merged image at 0x0 (bootloader + partitions + app)
esptool.py --chip esp32 write_flash 0x0 claude-desktop-buddy-<version>-merged.bin

# UPDATING an already-paired device — app image only, at 0x10000
esptool.py --chip esp32 write_flash 0x10000 claude-desktop-buddy-<version>-app.bin
```

> ⚠️ **The merged `0x0` image wipes NVS — your BLE bond and saved settings.**
> `nvs` lives at `0x9000`, inside the range the merged image overwrites, so
> flashing it forces a **re-pair** (and resets brightness/screen-off). To update
> a stick you've already paired, flash the **`-app.bin` at `0x10000`** (or use
> `pio run -e <env> -t upload`) — it touches only the app partition and leaves
> the bond intact. If you do flash the merged image, also **remove the stale
> bond on the host** (e.g. Windows → Bluetooth → *Remove device*) before
> re-pairing, or the old keys will make the link drop ~1s after every connect.

> **M5StickS3:** prebuilt release images aren't published yet — build from
> source above. (When they are, they'll be `--chip esp32s3` at the same
> `0x0` / `0x10000` offsets.)

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the stick:

- Make sure it's awake (any button press)
- Check the stick's settings menu → bluetooth is on

## Controls

|                         | Normal               | Pet         | Info        | Approval    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A** (front)           | next screen          | next screen | next screen | **approve** |
| **B** (right)           | scroll transcript    | next page   | next page   | **deny**    |
| **Hold A**              | menu                 | menu        | menu        | menu        |
| **Power** (left, short) | toggle screen off    |             |             |             |
| **Power** (left, ~6s)   | hard power off       |             |             |             |
| **Shake**               | dizzy                |             |             | —           |
| **Face-down**           | nap (energy refills) |             |             |             |

The screen auto-powers-off after 30s of no interaction (kept on while an
approval prompt is up). Any button press wakes it.

## ASCII pets

Eighteen pets, each with seven animations (sleep, idle, busy, attention,
celebrate, dizzy, heart). Menu → "next pet" cycles them with a counter.
Choice persists to NVS.

## GIF pets

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the stick switches to GIF mode live. **Settings
→ delete char** reverts to ASCII mode.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide; height up to ~140px stays on a 135×240 portrait screen.
Crop tight to the character — transparent margins waste screen and shrink
the sprite. `tools/prep_character.py` handles the resize: feed it source
GIFs at any sizes and it produces a 96px-wide set where the character is the
same scale in every state.

The whole folder must fit the device's filesystem — ~1.8MB on the M5StickC
Plus (4MB flash), with more headroom on the M5StickS3 (8MB flash).
`gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.

See `characters/bufo/` for a working example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo` stages it into `data/` and runs
`pio run -t uploadfs` directly over USB.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks** (StickC Plus) |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Project layout

```
src/
  main.cpp       — loop, state machine, UI screens
  buddy.cpp      — ASCII species dispatch + render helpers
  buddies/       — one file per species, seven anim functions each
  ble_bridge.cpp — Nordic UART service, line-buffered TX/RX
  character.cpp  — GIF decode + render
  data.h         — wire protocol, JSON parse
  xfer.h         — folder push receiver
  stats.h        — NVS-backed stats, settings, owner, species choice
characters/      — example GIF character packs
tools/           — generators and converters
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.

## Credits

- **[@ToxicOrca](https://github.com/ToxicOrca)** — the **battery-life work** in
  this fork: adaptive frame rate, IMU/display throttling, BLE stop-advertising on
  connect, and the longer keepalive window. (Also the Android bridge app and
  bridge-side battery/token work over in
  [buddy-bridge](https://github.com/ttpears/buddy-bridge).)
- Forked from [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy);
  original firmware and BLE protocol by Anthropic.
