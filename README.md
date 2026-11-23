# Need for Speed: Hot Pursuit 2 for R36S & PortMaster Handhelds

An unofficial native port of the PlayStation 2 version of **Need for Speed: Hot Pursuit 2** (2002) to Linux handhelds (R36S, Anbernic RG35XX/RG34XX, Powkiddy, RK3326, Allwinner H700, and other PortMaster-compatible devices), with continued support for the New Nintendo 3DS.

It is the PS2 game itself, not an emulator or a remake: the code was reverse-engineered function-by-function from the retail PAL disc (`SLES-50731`) and reimplemented in C. It runs from your own disc's assets, which you install on the device. Every PS2 release is supported: USA SLUS-20362, Europe SLES-50731 (v1.00 or v2.00), Asia SLPM-65181 or Korea SLPM-67527 (see [docs/releases.md](docs/releases.md)). **No proprietary game data is distributed with this repository.**

---

## Features

- **Full PS2 Experience:** Hot Pursuit and World Racing modes, Quick Race, You're The Cop, Championship, Ultimate Racer, the career events, police chases with helicopter pursuit, replays, the complete EA TRAX jukebox, every licensed car and track, and the original PS2 cheat codes.
- **Native Single-Screen 640x480 Framing:** Replaces the dual-screen 3DS presentation with native PS2 single-screen rendering, complete with original PS2 camera field of view, HUD layouts, and dynamic minimap and tachometer anchoring across 4:3, 3:2, and 16:9 displays.
- **High-Performance GLES Renderer:** Citro3D/Citro2D graphics commands are translated to an optimized OpenGL ES 2.0 / 3.0 backend. Features an independent GL worker thread (`glthread`), persistent GPU vertex/index buffer caching (VBOs), opaque instancing, and persistent shader program caching (`nfshp2_programs.bin`).
- **On-Device Disc Installer:** Drop your PS2 `.iso` file directly into `ports/nfshp2/gamedata/` and launch the game. The port automatically unpacks `ZDIR.BIN` and `ZZDATA.BIN`, decompresses all 30 track archives, and packs audio banks on the device.
- **Handheld Quality-of-Life:**
  - **FPS & Performance Overlay:** Live frame rate, frame times, draw calls, and CPU/GPU metrics (`Select + L1`, or `L3` / `R3`).
  - **Stick / D-Pad Swap:** Instant toggle between Left Analog Stick and D-Pad steering (`Select + R1`) for stickless handhelds.
  - **Live Quality Presets:** Real-time cycling between graphics presets (`Select + R2`: Full, Fast, Fastest).
  - **Safe Exit:** Clean shutdown on `Select + Start` (held for 1 second).

---

## Supported Devices & Systems

- **Rockchip RK3326:** R36S, R35S, Powkiddy RGB20S, RG351P/M/V/MP running ArkOS, dArkOS, AmberELEC.
- **Allwinner H700:** Anbernic RG35XX (2024), RG35XX Plus, RG35XX H, RG34XX, RG40XX running Knulli or Batocera.
- **General Linux Handhelds:** Any 32-bit `armhf` Linux distribution equipped with SDL2 and OpenGL ES 2.0/3.0.
- **New Nintendo 3DS:** New 3DS, New 3DS XL, and New 2DS XL running Luma3DS custom firmware (supports Local Wireless multiplayer up to 8 consoles).

---

## Installation (PortMaster / Linux Handhelds)

### Method A: Automated On-Device Installation from ISO (Recommended)

1. Copy the PortMaster package (`NFSHP2.sh` and the `nfshp2/` directory) to your SD card's `ports/` directory (e.g. `roms/ports/` or `ports/`).
2. Copy your PS2 disc ISO image (USA `SLUS-20362`, Europe `SLES-50731` v1.00 or v2.00, Asia `SLPM-65181` or Korea `SLPM-67527`) to:
   ```
   ports/nfshp2/gamedata/your_disc.iso
   ```
   *(Ensure you have at least 3 GB of free space on your SD card beside the ISO).*
3. Launch **Need for Speed: Hot Pursuit 2** from your Ports menu.
4. The first launch automatically extracts the game assets, expands track files, and builds sound banks (progress is displayed on screen and logged to `ports/nfshp2/patchlog.txt`). The process takes about 8 to 10 minutes on an RK3326.
5. Once the game boots to the language select screen, the installation is complete. You can safely delete the `.iso` file from `ports/nfshp2/gamedata/`.

### Method B: Manual Extraction on PC (Faster / No Python on Device)

If your device firmware does not have `python3`, or you prefer faster extraction:

1. Copy `ZDIR.BIN` and `ZZDATA.BIN` from your disc's `NFSHP2/` folder into `PS2 Files/Full Game/`. The extraction keeps `PAL.TAG` where the disc has it, and `make_card.py` writes `RELEASE.TXT` with the release's name.
2. From the repository root on your computer (requires Python 3.8+):
   ```bash
   python tools/zdir_extract.py --retail --decompress
   python tools/audio_pack.py extracted-retail/SOUND extracted-retail/SOUND
   python tools/make_card.py card
   ```
3. Copy the resulting `card/NFSHP2` folder to your handheld SD card at:
   ```
   ports/nfshp2/gamedata/NFSHP2
   ```

---

## Controls

### In a Race

| Handheld Button | Racer Action | You're The Cop Action |
| --- | --- | --- |
| Left Analog Stick / D-Pad Left & Right | Steer | Steer |
| B (Bottom button) | Accelerate | Accelerate |
| Y (Left button) | Brake & Reverse | Brake & Reverse |
| A (Right button) | Horn | Change Target |
| X (Top button) | Change Camera | Change Camera |
| L1 | Look Back | Call Roadblock |
| R1 | Handbrake | Call Helicopter |
| L2 | Shift Down (Manual) / Zone Freeze (Auto) | Nitro |
| R2 | Shift Up (Manual) / Zone Preview (Auto) | Shift Up |
| Right Analog Stick Up / Down | Accelerate / Brake | Accelerate / Brake |
| D-Pad Up | Change HUD Mode | Change HUD Mode |
| Start | Pause | Pause |

### In-Game Shortcuts

| Combination | Action |
| --- | --- |
| **Select + Start** *(hold 1 sec)* | Clean exit to frontend / PortMaster |
| **Select + L1** *(or L3 / R3)* | Toggle FPS & Performance Monitor Overlay |
| **Select + R1** | Toggle D-Pad and Left Analog Stick Swap |
| **Select + R2** | Cycle Graphics Preset (0 = Full, 1 = Fast, 2 = Fastest) |

### In the Menus

- **D-Pad / Left Stick:** Navigate options
- **B (Bottom):** Select / Confirm
- **Y (Left):** Back / Cancel (or **A / Right** depending on device mapping)
- **X (Top):** View Help / Controller diagram

---

## Settings (`nfshp2.cfg`)

You can customize the port by creating or editing `ports/nfshp2/nfshp2.cfg` with one `KEY=value` per line:

| Option | Values | Default | Description |
| --- | --- | --- | --- |
| `NFSHP2_SHOW_FPS` | `0` or `1` | `0` | Enable FPS & hardware performance overlay at launch |
| `NFSHP2_QUALITY` | `0`, `1`, `2` | `0` | Graphics preset (`0` = Full, `1` = Fast, `2` = Fastest with 75% render scale) |
| `NFSHP2_ASPECT` | `0` or `1` | `1` | `1` stretches view to screen edges; `0` forces pillarboxed 4:3 PS2 ratio |
| `NFSHP2_DPAD_STICK` | `0` or `1` | `0` | Launch with D-Pad and Left Analog Stick steering swapped |
| `NFSHP2_GLYPHS` | `ps2`, `xbox`, `nintendo` | `nintendo` | Button prompt glyph style on screen |
| `NFSHP2_REGION` | `NTSC` | unset | Turns the language choice off: English, with no language screen at start and no Languages item in Options. Every release has the choice by default |
| `NFSHP2_RENDER_SCALE` | `0.25` to `1.0` | `1.0` | Render target scale factor (e.g. `0.75` for 75% internal resolution) |

---

## Cheat Codes

Enter these 8-button sequences on the Main Menu to unlock bonus cars, tracks, and picture modes:

### Picture Modes & Bonus Events

| Unlock | Key Sequence (Nintendo Handheld Layout) |
| --- | --- |
| Motion Blur Mode | L2, L1, R1, Y, R1, R1, R1, L1 |
| Black & White Mode | R1, L1, D-Pad Left, B, D-Pad Left, D-Pad Left, D-Pad Left, L1 |
| Heavy Fog Mode | B, Y, D-Pad Right, R2, D-Pad Right, D-Pad Right, D-Pad Right, Y |
| R/C Car Mode | D-Pad Right, B, L1, R1, L1, L1, L1, B |
| Video Mode | L1, R1, D-Pad Right, Y, D-Pad Right, D-Pad Right, D-Pad Right, R1 |
| Chrome Cars | L2, R2, D-Pad Left, Y, D-Pad Left, D-Pad Left, D-Pad Left, R2 |
| Championship Bonus Event | L1, Y, R2, R2, R2, R1, L1, Y |
| Ultimate Racer Bonus Event | D-Pad Right, Y, D-Pad Left, D-Pad Left, D-Pad Left, R2, D-Pad Right, Y |

### Cars

| Unlock | Key Sequence |
| --- | --- |
| McLaren F1 | B, L1, B, L1, R1, D-Pad Left, R1, D-Pad Left |
| McLaren F1 LM | Y, L1, Y, L1, B, D-Pad Right, B, D-Pad Right |
| Mercedes CLK-GTR | R2, R1, R2, R1, D-Pad Left, B, D-Pad Left, B |
| Chevrolet Corvette Z06 | D-Pad Left, R2, D-Pad Left, R2, L1, R1, L1, R1 |
| Ferrari F50 | L1, B, L1, B, D-Pad Right, L2, D-Pad Right, L2 |
| Lamborghini Diablo 6.0 VT | D-Pad Right, R2, D-Pad Right, R2, R1, L1, R1, L1 |
| Lamborghini Murcielago | R1, L2, R1, L2, L1, D-Pad Right, L1, D-Pad Right |
| Porsche Carrera GT | D-Pad Left, D-Pad Right, D-Pad Left, D-Pad Right, R1, R2, R1, R2 |
| Aston Martin V12 Vanquish | R2, D-Pad Right, R2, D-Pad Right, B, D-Pad Left, B, D-Pad Left |
| BMW Z8 | Y, D-Pad Right, Y, D-Pad Right, R2, B, R2, B |
| Ferrari 360 Spider | R2, Y, R2, Y, B, L2, B, L2 |
| Ferrari 550 Barchetta | L1, Y, L1, Y, D-Pad Right, R1, D-Pad Right, R1 |
| HSV Coupe GTS | L1, L2, L1, L2, R1, B, R1, B |
| Ford TS50 | D-Pad Right, D-Pad Left, D-Pad Right, D-Pad Left, R2, Y, R2, Y |
| Lotus Elise | B, R2, B, R2, D-Pad Left, Y, D-Pad Left, Y |
| Mercedes CL55 AMG | L1, D-Pad Right, L1, D-Pad Right, L2, Y, L2, Y |

---

## Building from Source

### R36S & Linux Handhelds (`armhf` / `aarch64`)

The build is automated inside Docker:

```bash
# 32-bit ARM build (recommended for R36S / PortMaster)
sh r36s/build.sh

# 64-bit ARM build
ARCH=aarch64 sh r36s/build.sh

# Package into PortMaster zip and folder layout
sh r36s/package.sh
```

The resulting package is output to `r36s/build/portmaster/nfshp2.zip`.

### New Nintendo 3DS

Requires devkitPro with `DEVKITARM`:

```powershell
& .\tools\build_cia.ps1
```

Generates `3DS Files/NFSHP2-3DS.3dsx` and `NFSHP2-3DS.cia`.

---

## Project Structure

- `r36s/` -- R36S / Linux handheld compatibility layer (`r36s/compat`), GL worker thread, PortMaster launcher (`NFSHP2.sh`), ISO installer, and Dockerfiles.
- `3DS Files/` -- Source code of the reverse-engineered game engine in C.
- `tools/` -- Extraction and conversion tools (`zdir_extract.py`, `audio_pack.py`, `make_card.py`).
- `docs/` -- Architecture documentation, R36S optimization backlog, and reverse-engineered file format specs.
- `HANDOFF.md` -- Detailed session-by-session engineering changelog.

---

## Credits & Acknowledgements

- **Electronic Arts & EA Black Box:** Original creators of Need for Speed: Hot Pursuit 2.
- **Octoblimp & Contributors:** Complete PS2 game decompilation and New Nintendo 3DS port.
- **Manuel Peña Llull ([@manuelpllull](https://github.com/manuelpllull)):** R36S / Linux handheld port, GLES rendering layer, GL threading, single-screen framing, PortMaster integration, and engine audio fixes.
- **AI Pairing:** Developed in collaboration with Claude Opus 5.5 and Antigravity (Google DeepMind).

---

## License

This project is licensed under the **GNU General Public License v3.0** (`LICENSE`).  
*Need for Speed* and *Need for Speed: Hot Pursuit 2* are registered trademarks of Electronic Arts Inc. This fan-made open-source port is not affiliated with, authorized, or endorsed by Electronic Arts. No copyrighted assets or proprietary binaries are included.
