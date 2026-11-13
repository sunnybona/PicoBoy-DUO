# PicoBoy DUO

A dual-emulator handheld firmware for the **Raspberry Pi RP2040** and a 240×240
ST7789 LCD. One device boots into a selector and runs either a **NES** or a
**Game Boy / Game Boy Color** game, with the two emulators kept fully isolated
as separate firmware images in different flash slots.

## How it works

Instead of merging both emulators into a single binary (which shares RAM and is
prone to cross-contamination), each emulator is built as a complete, independent
firmware and placed in its own flash slot. A small **bootloader** is the normal
power-on entry point; choosing a system reboots the chip and jumps to that slot.
Pressing **Y + B** inside any game soft-reboots back to the selector.

### Flash layout (16 MB)

| Address       | Contents                          |
|---------------|-----------------------------------|
| `0x10000000`  | Bootloader (selector)             |
| `0x10040000`  | Game Boy / GBC firmware slot      |
| `0x10080000`  | NES firmware slot                 |
| `0x100D0000`  | Shared settings sector (4 KB)     |
| `0x10120000`  | ROM area (one tar, mixed `.nes`/`.gb`/`.gbc`) |

Each slot keeps the standard 256-byte boot2 placeholder at its start; the vector
table lives at slot + `0x100`. The bootloader loads the initial SP and reset
vector from the slot, sets MSP/VTOR, enables interrupts, and branches to it.

## Repository layout

```
PicoSystem_DUO/
  bootloader/            selector firmware (PICOBOY DUO)
  gbc/                   Game Boy / GBC firmware (Walnut core + minigb_apu)
  merge_uf2.py           combines the three slot UF2 images into one
PicoSystem_InfoNes/      NES firmware (PicoSystem framework + InfoNES)
rom_uploader.py          host tool: packs ROMs and flashes firmware by address
```

## Hardware

- RP2040, overclocked to 250 MHz
- ST7789 240×240 SPI LCD, no TE (tearing-effect) pin; frame pacing is fixed
- Buttons are active-high with external pull-downs:
  A=GP18, B=GP19, X=GP17, Y=GP16,
  Up=GP23, Down=GP20, Left=GP22, Right=GP21
- Backlight PWM on GP12; audio PWM on GP11

## Building

Requires the Raspberry Pi **Pico SDK 1.5.x** with the bundled ARM GCC, CMake and
Ninja. Each firmware is an out-of-source build:

```sh
# point each project at the SDK, then configure and build
cmake -B <build> -S PicoSystem_DUO/bootloader
cmake --build <build>
```

The NES and GBC builds use a custom link script (`memmap_slot.ld`) so they are
linked at their slot addresses. After building all three, merge them:

```sh
python PicoSystem_DUO/merge_uf2.py PicoSystem_DUO.uf2 \
    bootloader/build/duo_bootloader.uf2 \
    gbc/build/rp2040_gbc.uf2 \
    PicoSystem_InfoNes/build_slot/PicoSystem_InfoNes.uf2
```

Flashing multi-slot images over drag-and-drop can leave slot sectors unerased;
prefer `picotool load -v`, or flash each `.bin` at its slot address.

## Controls

- **Y + B** — return to the bootloader selector
- **Y** in a menu — open settings (brightness / invert / volume)
- Settings persist across reboots in the shared flash sector

## Credits and license

Built on the open-source projects
[PicoSystem_InfoNes](https://github.com/fhoedemakers/PicoSystem_InfoNes),
[InfoNES](https://github.com/jay-kumogata/InfoNES),
[Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) and the minigb_apu library.
See each subproject for its license. Game ROMs are not included.
