/*
 * main.cpp — Walnut-CGB 前端（已验证的 rp2040-gbc 驱动架构，DUO 槽位版）
 *
 * 本固件链接在 flash 0x10040000 槽位，由 bootloader 跳入。
 *   - ROM 从 flash 0x10110000 读取：单个 ROM 或未压缩 tar（XIP 直读）
 *   - 美化 RomSelector：亮度 / 反色 / 音量，设置持久化到 0x100D0000
 *   - 无 TE：固定帧节拍（GB 59.7275Hz）
 *   - 游戏内 Y+B 看门狗重启，回 bootloader
 */
#include <stdio.h>
#include <string.h>
#include <string>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/flash.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"

#include "board_config.h"
#include "display.h"
#include "audio.h"
#include "input.h"
#include "rom_tar.h"

/* ------------------------------------------------------------------ */
/* Walnut-CGB 编译期开关（include 之前定义）                            */
/* ------------------------------------------------------------------ */
#define ENABLE_SOUND        1
#define ENABLE_LCD          1
#define WALNUT_FULL_GBC_SUPPORT 1
#define WALNUT_GB_12_COLOUR 1
#define WALNUT_GB_32BIT_DMA 1
#define WALNUT_GB_32BIT_ALIGNED 1
#define WALNUT_GB_HIGH_LCD_ACCURACY 1

uint8_t audio_read(uint16_t addr)             { return audio_apu_read(addr); }
void    audio_write(uint16_t addr, uint8_t v) { audio_apu_write(addr, v); }

#include "walnut_cgb.h"

#define IO_IDX_JOYP  0x00
#define IO_IDX_IF    0x0F
#define IO_IDX_IE    0xFF

#define GB_FRAME_NS  16742706ull

/* ------------------------------------------------------------------ */
/* 常量与全局                                                          */
/* ------------------------------------------------------------------ */
static constexpr uintptr_t ROM_FILE_ADDR = 0x10120000;

/* 设置持久化：0x100D0000，一个 4KB 扇区 */
#define SETTINGS_FLASH_OFFSET (0x100D0000 - 0x10000000)
#define SETTINGS_BLOCK_SIZE  4096
#define SET_MARK_VOLUME  5      /* "VOL" 标记位置 */
#define SET_POS_VOLUME   9
#define SET_POS_BACKLIGHT 10
#define SET_POS_INVERT   11

static int g_volume = 10;
static int g_backlight = 80;
static bool g_invert = false;

static struct gb_s s_gb;
static uint8_t s_cart_ram[32768];

static const uint8_t *g_rom_data;
static size_t g_rom_size;

/* ------------------------------------------------------------------ */
/* GB/GBC ROM 识别（头部校验和或 Nintendo logo，任一成立即可）          */
/* ------------------------------------------------------------------ */
static const uint8_t kLogoSig[] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D};

static bool check_gb_logo(const uint8_t *d)
{
    for (size_t i = 0; i < sizeof(kLogoSig); i++)
        if (d[0x104 + i] != kLogoSig[i])
            return false;
    return true;
}

static bool check_gb_header_checksum(const uint8_t *d)
{
    int chk = 0;
    for (int i = 0x134; i <= 0x14C; i++)
        chk = chk - d[i] - 1;
    return (chk & 0xFF) == d[0x14D];
}

static bool check_gb_magic(const uint8_t *d)
{
    return check_gb_header_checksum(d) || check_gb_logo(d);
}

/* ------------------------------------------------------------------ */
/* 背光：GP12 PWM，线性占空 0..100%                                     */
/* ------------------------------------------------------------------ */
static void apply_backlight(int pct)
{
    uint16_t level = (uint16_t)(pct * 255 / 100);
    pwm_set_gpio_level(GB_PIN_LCD_BL, level);
}

static void backlight_init()
{
    gpio_set_function(GB_PIN_LCD_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(GB_PIN_LCD_BL);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, 255);
    pwm_init(slice, &c, true);
    apply_backlight(g_backlight);
}

/* ------------------------------------------------------------------ */
/* 设置持久化（256 字节页缓冲，避免大静态数组顶过 RAM overlay 基址）     */
/* ------------------------------------------------------------------ */
static void restore_settings()
{
    const uint8_t *s = reinterpret_cast<const uint8_t *>(0x100D0000);
    if (s[SET_MARK_VOLUME] == 'V' && s[SET_MARK_VOLUME + 1] == 'O' &&
        s[SET_MARK_VOLUME + 2] == 'L')
    {
        g_volume = s[SET_POS_VOLUME];
        g_backlight = s[SET_POS_BACKLIGHT];
        g_invert = s[SET_POS_INVERT] != 0;
    }
}

static void persist_settings()
{
    static uint8_t page[FLASH_PAGE_SIZE];

    audio_set_mute(1);
    multicore_reset_core1();
    uint32_t saved = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, SETTINGS_BLOCK_SIZE);

    for (uint32_t off = 0; off < SETTINGS_BLOCK_SIZE; off += FLASH_PAGE_SIZE)
    {
        memset(page, 0xFF, sizeof(page));
        if (off == 0)
        {
            page[SET_MARK_VOLUME] = 'V';
            page[SET_MARK_VOLUME + 1] = 'O';
            page[SET_MARK_VOLUME + 2] = 'L';
            page[SET_POS_VOLUME] = (uint8_t)g_volume;
            page[SET_POS_BACKLIGHT] = (uint8_t)g_backlight;
            page[SET_POS_INVERT] = g_invert ? 1 : 0;
        }
        flash_range_program(SETTINGS_FLASH_OFFSET + off, page, FLASH_PAGE_SIZE);
    }

    restore_interrupts(saved);
    display_start_pusher();
    audio_set_mute(0);
}

/* ------------------------------------------------------------------ */
/* 卡带回调                                                            */
/* ------------------------------------------------------------------ */
static uint8_t rom_read_cb(struct gb_s *, const uint_fast32_t addr)
{
    return (addr < g_rom_size) ? g_rom_data[addr] : 0xFF;
}

static uint16_t rom_read16_cb(struct gb_s *, const uint_fast32_t addr)
{
    uint8_t b0 = (addr < g_rom_size) ? g_rom_data[addr] : 0xFF;
    uint8_t b1 = (addr + 1 < g_rom_size) ? g_rom_data[addr + 1] : 0xFF;
    return (uint16_t)(b0 | ((uint16_t)b1 << 8));
}

static uint32_t rom_read32_cb(struct gb_s *, const uint_fast32_t addr)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        uint8_t b = (addr + (uint_fast32_t)i < g_rom_size)
                        ? g_rom_data[addr + i] : 0xFF;
        v |= ((uint32_t)b) << (8 * i);
    }
    return v;
}

static uint8_t ram_read_cb(struct gb_s *, const uint_fast32_t addr)
{
    return (addr < sizeof(s_cart_ram)) ? s_cart_ram[addr] : 0xFF;
}

static void ram_write_cb(struct gb_s *, const uint_fast32_t addr, uint8_t val)
{
    if (addr < sizeof(s_cart_ram))
        s_cart_ram[addr] = val;
}

static void error_cb(struct gb_s *, const enum gb_error_e err, const uint16_t addr)
{
    printf("[gb] fatal error %d at 0x%04X\n", (int)err, addr);
    audio_set_mute(1);
    display_message("GB ERROR", "CHECK ROM", "AND RESET");
    while (true) tight_loop_contents();
}

/* ------------------------------------------------------------------ */
/* DMG 调色板：柔化绿色四档（BG/OBJ0/OBJ1 同色），亮度均匀、对比清晰      */
/* 对应 #C8D69B #86A05E #4A6B45 #1F3826                                */
/* ------------------------------------------------------------------ */
static const uint16_t s_dmg_pal12[12] = {
    /* OBJ0 */ 0xCEB3, 0x850B, 0x4B48, 0x19C4,
    /* OBJ1 */ 0xCEB3, 0x850B, 0x4B48, 0x19C4,
    /* BG   */ 0xCEB3, 0x850B, 0x4B48, 0x19C4,
};

/* ------------------------------------------------------------------ */
/* 扫描线回调                                                          */
/* ------------------------------------------------------------------ */
static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels,
                             const uint_fast8_t line)
{
    static uint16_t row[160];

    if (gb->cgb.cgbMode)
    {
        for (int i = 0; i < 160; i++)
            row[i] = gb->cgb.fixPalette[pixels[i] & 0x3F];
    }
    else
    {
        for (int i = 0; i < 160; i++)
        {
            uint8_t px = pixels[i];
            uint8_t layer = (px >> 4) & 3;
            /* 与 proven rp2040-gbc 一致：shade 0=最亮、3=最暗，直接索引（表内亮→暗） */
            uint8_t shade = px & 3;
            if (layer == 2)      row[i] = s_dmg_pal12[8 + shade];
            else if (layer == 1) row[i] = s_dmg_pal12[4 + shade];
            else                 row[i] = s_dmg_pal12[shade];
        }
    }

    if (g_invert)
        for (int i = 0; i < 160; i++)
            row[i] ^= 0xFFFF;

    display_push_rgb_line(row, (uint8_t)line);
}

/* ------------------------------------------------------------------ */
/* 每帧按键合并                                                        */
/* ------------------------------------------------------------------ */
static void update_pad(void)
{
    uint8_t pad = input_read();
    s_gb.direct.joypad = (uint8_t)~pad;

    uint8_t sel = s_gb.hram_io[IO_IDX_JOYP] & 0x30;
    uint8_t lo = 0x0F;
    if (!(sel & 0x10)) lo &= (uint8_t)~(pad >> 4);
    if (!(sel & 0x20)) lo &= (uint8_t)~(pad & 0x0F);
    s_gb.hram_io[IO_IDX_JOYP] = (uint8_t)(0xC0 | sel | lo);

    if (pad && (s_gb.hram_io[IO_IDX_IE] & 0x10))
        s_gb.hram_io[IO_IDX_IF] |= 0x10;
}

/* ------------------------------------------------------------------ */
/* 设置菜单：Y 进入，U/D 选择，L/R 调节，B 返回                         */
/* ------------------------------------------------------------------ */
#define C_BG      0x0000
#define C_TXT     0xFFFF
#define C_DIM     0x8410
#define C_BAR     0x1125   /* 深蓝标题栏 */
#define C_BOX     0x2104   /* 未选中项深灰底 */
#define C_SEL     0x07E0   /* 选中绿底 */
#define C_ONBAR   0x0000   /* 选中条上的文字 */

static void settings_menu()
{
    int sel = 0;
    const int row_y[3] = {62, 112, 162};
    const int row_h = 40;

    /* ---- 静态骨架只画一次 ---- */
    display_wait_idle();
    display_fill(C_BG);
    display_rect(0, 0, 240, 30, C_BAR);
    display_text8(48, 8, "SETTINGS", C_TXT, C_BAR, 2);

    auto settings_line = [](int i)
    {
        char line[26];
        if (i == 0)      snprintf(line, sizeof(line), "BRIGHTNESS %d%%", g_backlight);
        else if (i == 1) snprintf(line, sizeof(line), "INVERT %s", g_invert ? "ON" : "OFF");
        else             snprintf(line, sizeof(line), "VOLUME %d", g_volume);
        return std::string(line);
    };

    auto draw_row = [&](int i)
    {
        bool on = (i == sel);
        display_rect(8, row_y[i], 224, row_h, on ? C_SEL : C_BOX);
        std::string line = settings_line(i);
        int tw = (int)line.size() * 9;               /* scale 1 字宽 */
        display_text8((240 - tw) / 2, row_y[i] + 16, line.c_str(),
                      on ? C_ONBAR : C_TXT, on ? C_SEL : C_BOX, 1);
    };
    draw_row(0);
    draw_row(1);
    draw_row(2);

    display_text8(92, 220, "B BACK", C_DIM, C_BG, 1);

    uint8_t prev = 0;
    while (true)
    {
        uint8_t cur = input_read();
        uint8_t pushed = (uint8_t)(cur & (uint8_t)~prev);
        prev = cur;

        if (pushed & 0x40)                             /* UP */
        {
            int old = sel;
            sel = (sel + 2) % 3;
            if (old != sel) { draw_row(old); draw_row(sel); }
        }
        if (pushed & 0x80)                             /* DOWN */
        {
            int old = sel;
            sel = (sel + 1) % 3;
            if (old != sel) { draw_row(old); draw_row(sel); }
        }

        bool changed = false;
        if (pushed & 0x20)                             /* LEFT */
        {
            if (sel == 0) g_backlight = g_backlight >= 10 ? g_backlight - 10 : 0;
            if (sel == 2) g_volume = g_volume > 0 ? g_volume - 1 : 0;
            if (sel == 1) g_invert = false;
            changed = true;
        }
        if (pushed & 0x10)                             /* RIGHT */
        {
            if (sel == 0) g_backlight = g_backlight <= 90 ? g_backlight + 10 : 100;
            if (sel == 2) g_volume = g_volume < 10 ? g_volume + 1 : 10;
            if (sel == 1) g_invert = true;
            changed = true;
        }
        if (changed)
        {
            apply_backlight(g_backlight);
            audio_set_volume(g_volume);
            draw_row(sel);
        }

        if (pushed & 0x02)                             /* B */
        {
            persist_settings();
            return;
        }
        sleep_ms(20);
    }
}

/* ------------------------------------------------------------------ */
/* ROM 选择器                                                          */
/* ------------------------------------------------------------------ */
struct rom_info_t
{
    const uint8_t *data;
    size_t size;
    char name[26];
};

static size_t rom_size_from_header(const uint8_t *d)
{
    uint8_t code = d[0x148];
    if (code <= 8) return 32768u << code;
    return 0;
}

static void select_rom(bool is_tar, int entry_count)
{
    static rom_info_t roms[64];
    int count = 0;
    const char *base = reinterpret_cast<const char *>(ROM_FILE_ADDR);

    if (is_tar)
    {
        for (int i = 0; i < entry_count && count < 64; i++)
        {
            TAREntry e = extractTAREntryatindex(i, (const void *)base,
                                               check_gb_magic);
            if (!e.data) continue;
            roms[count].data = e.data;
            roms[count].size = e.size;
            size_t n = e.filename.size();
            if (n > 25) n = 25;
            memcpy(roms[count].name, e.filename.data(), n);
            roms[count].name[n] = 0;
            count++;
        }
    }
    else
    {
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(ROM_FILE_ADDR);
        roms[0].data = raw;
        roms[0].size = rom_size_from_header(raw);
        if (!roms[0].size) roms[0].size = 1024 * 1024;
        strcpy(roms[0].name, "SINGLE ROM");
        count = 1;
    }

    if (count == 0)
    {
        display_message("NO GB ROM", "FLASH A .GB", "OR .GBC");
        while (true) sleep_ms(200);
    }

    int sel = 0;
    int top = 0;
    const int VISIBLE = 7;
    const int row_h = 26;

    /* ---- 静态骨架只画一次，之后仅重绘列表行，杜绝闪烁 ---- */
    display_wait_idle();
    display_fill(C_BG);
    display_rect(0, 0, 240, 30, C_BAR);
    display_text8(48, 11, "GAME BOY LIBRARY", C_TXT, C_BAR, 1);
    display_text8(39, 224, "U/D MOVE A OK Y SET", C_DIM, C_BG, 1);

    auto draw_list_row = [&](int slot)
    {
        int i = top + slot;
        bool on = (i == sel);
        int y = 38 + slot * row_h;
        display_rect(8, y, 224, 24, on ? C_SEL : C_BOX);
        char shown[26];
        strncpy(shown, roms[i].name, 24);
        shown[24] = 0;
        display_text8(14, y + 8, shown,
                      on ? C_ONBAR : C_TXT, on ? C_SEL : C_BOX, 1);
    };

    auto redraw_window = [&]()
    {
        int n = count - top;
        if (n > VISIBLE) n = VISIBLE;
        for (int s = 0; s < n; s++)
            draw_list_row(s);
    };
    redraw_window();

    uint8_t prev = 0;
    while (true)
    {
        uint8_t cur = input_read();
        uint8_t pushed = (uint8_t)(cur & (uint8_t)~prev);
        prev = cur;

        bool moved = false;
        if (pushed & 0x80)                                 /* DOWN */
        {
            sel = (sel + 1) % count;
            moved = true;
        }
        if (pushed & 0x40)                                 /* UP */
        {
            sel = (sel + count - 1) % count;
            moved = true;
        }

        if (moved)
        {
            int new_top = top;
            if (sel < top) new_top = sel;
            else if (sel >= top + VISIBLE) new_top = sel - VISIBLE + 1;
            if (new_top != top)
            {
                top = new_top;
                redraw_window();
            }
            else
            {
                /* 窗口不动：重画当前窗口即可（无整屏清黑，不会闪） */
                redraw_window();
            }
        }

        if (pushed & 0x04)                                 /* Y */
        {
            settings_menu();
            /* 从设置返回后骨架可能被覆盖，整体重画一次 */
            display_fill(C_BG);
            display_rect(0, 0, 240, 30, C_BAR);
            display_text8(48, 11, "GAME BOY LIBRARY", C_TXT, C_BAR, 1);
            display_text8(39, 224, "U/D MOVE A OK Y SET", C_DIM, C_BG, 1);
            redraw_window();
        }

        if (pushed & 0x01)                                  /* A */
        {
            g_rom_data = roms[sel].data;
            g_rom_size = roms[sel].size;
            return;
        }
        sleep_ms(20);
    }
}

/* ------------------------------------------------------------------ */
int main(void)
{
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(GB_SYS_CLOCK_KHZ, true);

    stdio_init_all();

    restore_settings();

    display_init();
    backlight_init();
    display_splash("WALNUT CGB");
    sleep_ms(800);
    input_init();
    audio_init();
    audio_set_volume(g_volume);

    display_start_pusher();

    while (true)
    {
        const char *base = reinterpret_cast<const char *>(ROM_FILE_ADDR);
        bool is_tar = strncmp("ustar", base + 257, 5) == 0;
        int entry_count = 0;
        if (is_tar)
            entry_count = GetValidTAREntries((const void *)base,
                                             check_gb_magic);
        else if (!check_gb_magic(reinterpret_cast<const uint8_t *>(base)))
        {
            display_wait_idle();
            display_message("NO GB ROM", "FLASH A .GB", "OR .GBC");
            while (true) sleep_ms(200);
        }

        audio_set_mute(1);
        select_rom(is_tar, entry_count);
        audio_set_mute(0);

        memset(&s_gb, 0, sizeof(s_gb));
        memset(s_cart_ram, 0, sizeof(s_cart_ram));
        enum gb_init_error_e ie = gb_init(&s_gb, rom_read_cb, rom_read16_cb,
                                          rom_read32_cb, ram_read_cb,
                                          ram_write_cb, error_cb, NULL);
        if (ie != GB_INIT_NO_ERROR)
        {
            printf("gb_init error %d\n", (int)ie);
            display_message("BAD ROM", "INIT FAIL", "SEE SERIAL");
            sleep_ms(1500);
            continue;
        }

        gb_init_lcd(&s_gb, lcd_draw_line_cb);
        s_gb.direct.joypad = 0;
        s_gb.direct.frame_skip = false;
        s_gb.direct.interlace = false;

        display_wait_idle();
        display_fill(C_BG);

        uint64_t next_frame_ns = time_us_64() * 1000ull;
        while (true)
        {
            uint64_t target_us = next_frame_ns / 1000ull;
            while (time_us_64() < target_us)
                tight_loop_contents();

            uint64_t now_ns = time_us_64() * 1000ull;
            if (now_ns > next_frame_ns + 2 * GB_FRAME_NS)
                next_frame_ns = now_ns;

            update_pad();
            display_begin_gb_frame();
            gb_run_frame_dualfetch(&s_gb);
            display_end_gb_frame();

            audio_apu_frame();

            /* Y+B：看门狗重启回 bootloader */
            uint8_t pad = input_read();
            if ((pad & 0x06) == 0x06)
            {
                audio_set_mute(1);
                watchdog_reboot(0, 0, 0);
            }

            next_frame_ns += GB_FRAME_NS;
        }
    }
}
