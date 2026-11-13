# HANDOFF — 换一台电脑继续这个工程

## 一句话

解压 → 设两个环境变量 → 一条 cmake 命令 → `build/rp2040_gbc.uf2` 就能烧。
**不需要任何 `-D` 参数**，ROM 路径、flash 容量、缩放倍率、SPI 时钟全部有默认值。

```powershell
$env:PICO_SDK_PATH="E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\pico-sdk"
$env:PATH="E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\cmake\bin;E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\ninja;E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\gcc-arm-none-eabi\bin;E:\Program Files\Raspberry Pi\Pico SDK v1.5.1\python;" + $env:PATH

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rp2040_gbc -j 1
```

（SDK 版本 1.5.1，工具链 GNU 10.3.1。SDK 装到别处只改上面第一行路径。）

## 默认值（都是 CMake 里的 CACHE 变量，可以 -D 覆盖）

| 变量 | 默认 | 说明 |
|---|---|---|
| `GB_DISPLAY_SCALE15` | `1` | 240×216 最近邻；`0` = 160×144 居中 |
| `GB_LCD_SPI_HZ` | `80000000` | PIO 位时钟，实测这块板 80MHz 稳，100MHz 乱 |
| `GB_FLASH_SIZE_BYTES` | `16777216` | 板载 16MB flash，**同时决定存档块位置** |
| `GB_AUDIO_SAMPLE_RATE` | `61035` | = PWM 载波 fclk/2^12，别乱改 |
| `GB_ROM_SLOTS` | `1;2;3;4;5;6` | 选择器里启用哪几个槽位 |
| `GB_ROM_1..6` | `rom/zelda_dx.gbc` … | 内置 6 个测试 ROM |

`rom/` 里 6 个文件都随包带上了（2.5MB），不依赖任何外部路径。

## 三个必须知道的坑

1. **`-j 1`**。宿主机内存小，`cmake --build` 默认并行会 `cc1plus.exe: out of memory`。
2. **改 `-D` 参数就删 build 目录重配**。PowerShell 里变量插值进 `-D` 会变成字面 `$`
   （`-DGB_DISPLAY_SCALE15=$($j.sc)` → Cache 里存成 `STRING=$`），于是 1x 和 1.5x 会编出
   完全相同的 elf。最稳的做法：改参数前 `Remove-Item build -Recurse -Force`。
3. **日志重定向要先建目录**。`cmake ... *> build\x.log` 在 `build\` 不存在时直接
   `DirectoryNotFoundException`。

## 16MB flash：两处都改过，别退回去

1. `memmap_16m.ld`（FLASH 区 `LENGTH = 16384k`）通过 `pico_set_linker_script()` 接上。
   用 SDK 自带的 `memmap_default.ld`（2048k）会 `section .rodata will not fit in region FLASH`
   —— 两个 1MB 的 ROM 一链就溢出。
2. `PICO_FLASH_SIZE_BYTES=16777216` 编译宏。存档块 = `容量 - 32KB`，本版实测地址
   `0x10FF8000`（从 `rp2040_gbc.dis` 里核对过）。宏不对，存档会写到不存在的地址上。

## 现状（哪些验过、哪些没验）

已验：交叉编译 0 warning（text 2647708 / bss 235164）；PC 无头测试 6 个 ROM 全部正常
（Zelda DX、Pocket Bomberman 真彩帧图见 `tools/*.png`）。

未验（**换机后从这里接着做**）：

- 实机 60fps：串口每秒一行 `fps=… rows=… push_us=… late=…us pad=… p1=…`。
  CGB 比 DMG 重，掉帧就先烧 `rp2040-gbc-1x-80m.uf2`（160×144）。
- 存档往返：Zelda 有 8KB 卡带 RAM，X+Y 按住 0.5s 存档 → A+B 退回选择器 → 再进，看能不能续上。
- 80MHz 以上（82~90MHz）没做二分，没有更高档固件。

## 包里有什么

```
CMakeLists.txt            全部默认值都在这里
pico_sdk_import.cmake
memmap_16m.ld             ← 16MB flash 链接脚本，别删
README.md                 架构说明 / 16MB 改法 / host 测试怎么跑
HANDOFF.md                本文件
src/                      main.cpp display.cpp/.h audio.cpp/.h audio_config.h
                          board_config.h input.cpp/.h save.cpp/.h font3x5.h
                          st7789_lcd.pio rom_table.h
lib/walnut/               walnut_cgb.h（279KB，上游原版）+ README + LICENSE(MIT)
lib/minigb_apu/           minigb_apu.c/.h（按 C 编译）
rom/                      6 个 ROM：zelda_dx.gbc / pocket_bomberman.gbc /
                          dd3_arcade.gb / double_dragon.gb / mario_yoshi.gb / spiderman2.gb
tools/                    make_roms_c.py（清单→C 数组）、host_test_gbc.c/.exe、
                          ppm2png.py、测试帧图
*.uf2                     成品固件（6rom 版 + 1x 回退版），编不起来也能直接烧
```

不含 `build*/`：CMake cache 存的是原机器的绝对路径，带过去必然 reconfigure 失败。
需要时在新机器上重新 configure 即可。

## host 测试怎么重跑

```powershell
gcc -O2 -std=c11 -I lib/walnut -o tools/host_test_gbc.exe tools/host_test_gbc.c
tools/host_test_gbc.exe rom/zelda_dx.gbc tools/zelda 900
python tools/ppm2png.py tools/zelda_idle.ppm
```

C 编译单元里必须先 `struct gb_s;` 前置声明再 include（头文件的函数原型写在结构体定义之前）。
