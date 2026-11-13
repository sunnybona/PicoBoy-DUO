# rp2040-gbc — Walnut-CGB（GB/GBC）模拟器

和 `rp2040-gb`（peanut_gb 版）并列的独立工程：**架构模板整套照搬**，只把核心换成
`lib/walnut/walnut_cgb.h`（Walnut-CGB，Peanut-GB 的性能重写版），并加上 **CGB 真彩**。

| | rp2040-gb | rp2040-gbc（本工程） |
|---|---|---|
| 核心 | `peanut_gb.h` | `walnut_cgb.h`（279KB 单头文件，master 原版未改） |
| 机器 | DMG | DMG + **CGB**（`WALNUT_FULL_GBC_SUPPORT 1`） |
| 颜色 | 固定 4 级绿 | CGB：调色板索引 → `cgb.fixPalette[64]`（RGB565）<br>DMG：12 色（色阶 + 图层 OBJ0/OBJ1/BG） |
| 跑帧 | `gb_run_frame` | `gb_run_frame_dualfetch`（16/32 位取指链） |
| ROM 取指 | 8 位回调 | 8 / 16 / 32 位三档回调（CGB 的 HDMA 与 dual-fetch 要用后两档） |

其余全部复用：PIO 软件 SPI + DMA + 双核（core0 跑模拟器/按键/音频，core1 只推屏）、
半幅乒乓、80MHz 位时钟、1.5 倍 240×216（上下各 12px 黑边）、PWM DAC 音频、
8 键输入、flash 存档、开机 ROM 选择器（A+B 按住 0.5s 退回、X+Y 按住 0.5s 存档）。

## 现成固件

**主固件：`rp2040-gbc-6roms-16m-15x-80m.uf2`（16MB flash 版，6 个 ROM 全在里面）**

| 槽位 | 游戏 | 机器 | 大小 |
|---|---|---|---|
| 1 | ZELDA DX | CGB 真彩 | 1024K |
| 2 | POCKET BOMBER | CGB 真彩 | 1024K |
| 3 | DD3 ARCADE | DMG 12 色 | 128K |
| 4 | DOUBLE DRAGON | DMG 12 色 | 128K |
| 5 | MARIO YOSHI | DMG 12 色 | 64K |
| 6 | SPIDERMAN 2 | DMG 12 色 | 128K |

text 2647708（2.6MB / 16MB），bss 235164，uf2 5.3MB。
回退版 `rp2040-gbc-6roms-16m-1x-80m.uf2`：同样 6 个 ROM，但 160×144 居中（CGB 掉帧时用）。

分部小固件（2MB flash 的普通 Pico 也能烧）：

| 文件 | ROM | text / bss |
|---|---|---|
| `rp2040-gbc-15x-80m.uf2` | 4 个 DMG | 550484 / 235164 |
| `rp2040-gbc-1x-80m.uf2` | 4 个 DMG，160×144 | 550380 / 177320 |
| `rp2040-gbc-zelda-15x-80m.uf2` | ZELDA DX + DD3 + MARIO | 1336892 / 235164 |
| `rp2040-gbc-bomberman-15x-80m.uf2` | POCKET BOMBERMAN + DD3 + MARIO | 1336892 / 235164 |

### 16MB flash 是怎么开起来的（两块要一起改）

这块板（PicoSystem 同款）是 16MB flash，SDK 默认当 2MB，会卡在两处：

1. **链接脚本**：`pico-sdk/src/rp2_common/pico_standard_link/memmap_default.ld` 里
   `FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 2048k` 写死。工程里放了一份
   `memmap_16m.ld`（改成 `16384k`），用 `pico_set_linker_script()` 接上。
   不改这行，两个 1MB 的 ROM 一链就 `section .rodata will not fit in region FLASH`。
2. **容量宏**：`-DGB_FLASH_SIZE_BYTES=16777216` → 编译期 `PICO_FLASH_SIZE_BYTES`。
   存档块地址就是 `PICO_FLASH_SIZE_BYTES - 32KB`，所以这个必须和真容量一致，
   否则存档会写到超过 2MB 的不存在地址上。本版实测存档块 = `0x10FF8000`（16MB 末尾）。


RAM 只有 264KB：1.5 倍版两块乒乓缓冲就占 103680B，加上 `struct gb_s`（wram 32K + vram 16K）
和 32KB 卡带 RAM，bss 到 235KB，双核各 8KB 栈后只剩 ~13KB。**再加内存就得砍缩放或砍卡带 RAM。**

## 构建

```powershell
$env:PICO_SDK_PATH="E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\pico-sdk"
$env:PATH="E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\cmake\bin;E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\ninja;E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\gcc-arm-none-eabi\bin;E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\python;"+$env:PATH

# 1.5 倍
cmake -S . -B build_gbc80 -G Ninja -DCMAKE_BUILD_TYPE=Release -DGB_LCD_SPI_HZ=80000000 -DGB_DISPLAY_SCALE15=1
cmake --build build_gbc80 --target rp2040_gbc -j 1     # 必须 -j 1，宿主机内存不够并行

# 1x
cmake -S . -B build_gbc1x -G Ninja -DCMAKE_BUILD_TYPE=Release -DGB_LCD_SPI_HZ=80000000 -DGB_DISPLAY_SCALE15=0
cmake --build build_gbc1x --target rp2040_gbc -j 1
```

改 `-D` 参数时**删掉 build 目录重配**——PowerShell 里变量插值进 `-D` 会变成字面 `$`，
缓存成 `STRING=$`，两次改参数会编出完全一样的 elf。

## 加一个 GBC 游戏

`rom/` 里现在有 4 个 DMG 测试 ROM + 2 个 CGB 游戏（`zelda_dx.gbc`、`pocket_bomberman.gbc`，
都是 1MB / MBC5 / 64 bank）。换成自己的 `.gbc`：

```powershell
cmake -S . -B build_z80 -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DGB_LCD_SPI_HZ=80000000 -DGB_DISPLAY_SCALE15=1 -DGB_ROM_SLOTS="1;2;3" `
  -DGB_ROM_1="C:/Users/bobo/Desktop/xxx.gbc" -DGB_ROM_1_LABEL="XXX" `
  -DGB_ROM_2="$PWD/rom/dd3_arcade.gb" -DGB_ROM_2_LABEL="DD3 ARCADE" `
  -DGB_ROM_3="$PWD/rom/mario_yoshi.gb" -DGB_ROM_3_LABEL="MARIO YOSHI"
```

ROM 直接内嵌进 flash（2MB 上限，text 已占 550KB），槽位由 `GB_ROM_SLOTS` 决定，
清单文件 `gb_rom_manifest.txt` → `tools/make_roms_c.py` → `rom_data.c`。
换电脑只要把工程目录带走，ROM 路径全是 `${CMAKE_CURRENT_LIST_DIR}/rom`。

## 代码要点

- `src/main.cpp`：Walnut 接线全在这里
  - `audio_read`/`audio_write` 必须在 `#include "walnut_cgb.h"` **之前**定义（头文件内部直接调用它们）
  - 编译期开关：`ENABLE_SOUND`、`WALNUT_FULL_GBC_SUPPORT` + `WALNUT_GB_12_COLOUR`（必须同开）、
    `WALNUT_GB_32BIT_DMA` / `WALNUT_GB_32BIT_ALIGNED`（32 位取指回调逐字节拼，不依赖非对齐访问）
  - `lcd_draw_line_cb`：CGB 走 `gb->cgb.fixPalette[px & 0x3F]`，DMG 走 `色阶 + (px>>4)&3` 图层查 12 色表，
    凑成 160 个 RGB565 后交给 `display_push_rgb_line()`
  - `update_pad()`：和 rp2040-gb 定稿的那套**一字不差**——写 `direct.joypad = ~pad`、
    按 `hram_io[JOYP] & 0x30` 重算两半区、按住时 `if(pad && (IE & 0x10)) IF |= 0x10`
  - 帧节奏 `GB_FRAME_NS = 16742706`（CGB 双速模式只改 CPU 时钟，不改帧率）
- `src/display.cpp` 新增两个函数（其余与 rp2040-gb 相同）
  - `display_push_rgb_line(const uint16_t *rgb, uint8_t line)` — 颜色已算好的行
  - `display_set_dmg_palette(const uint16_t pal4[4])` — 换 DMG 4 级色表
- `lib/walnut/walnut_cgb.h` 上游原版，未改；README/LICENSE 一并保留（MIT）

## 已验证 / 待验证

已验证（本机）：

- 交叉编译 **0 warning**，各版 uf2 都出得来，`arm-none-eabi-size` 两项都装得进 264KB / 2MB
- **host 无头测试**（`tools/host_test_gbc.c` + `tools/host_test_gbc.exe`）：
  4 个 DMG ROM 各 160 帧（第 60 帧按一次 START）——`gb_init` 三档回调、`gb_run_frame_dualfetch`、
  DMG 12 色渲染、`update_pad` 全部正常；帧图 `tools/*_idle.png` / `*_end.png`
- **CGB 真彩已跑通**：`zelda_dx.gbc` 900 帧、`pocket_bomberman.gbc` 600 帧，
  两个都被识别成 `mode=CGB mbc=5 banks_mask=63`，帧图是满色——
  `tools/zelda_dx_end.png`（红/蓝 Zelda DX 标题）、`tools/pocket_bomberman_end.png`（彩色 logo + Normal Game 菜单）。
  CGB 分支就是 `gb->cgb.fixPalette[px & 0x3F]`，颜色不再来自 4 级色表。

待实机验证（现在做不到的事，先写清楚）：

1. CGB 模拟比 DMG 重，1.5 倍版能不能稳 60fps 未知；串口每秒一行
   `fps=xx rows=144 push_us=xx late=xxus pad=xx p1=xx`，`fps` 掉就先换 `1x` 固件。
2. 80MHz 以上（82~90MHz）没做过实机二分，沿用 80MHz。
3. CGB 存档（Zelda 有 8KB 卡带 RAM）走的是和 DMG 同一套 flash 尾部块，未实机验证过读写。

## host 测试怎么重跑

```powershell
gcc -O2 -std=c11 -I lib/walnut -o tools/host_test_gbc.exe tools/host_test_gbc.c
tools/host_test_gbc.exe rom/dd3_arcade.gb tools/dd3_arcade
python tools/ppm2png.py tools/dd3_arcade_idle.ppm
```

（C 编译器里 `struct gb_s` 必须先前置声明——头文件的函数原型写在结构体定义之前，
C 会把每个原型里的 `struct gb_s` 当成新类型；C++ 没这个问题，所以 `main.cpp` 不用管。）
