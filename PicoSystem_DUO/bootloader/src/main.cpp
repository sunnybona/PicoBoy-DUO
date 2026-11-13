/*
 * main.cpp — PicoSystem DUO 开机选择 bootloader
 *
 * 链接在 0x10000000（含芯片 boot2），开机先跑这里。
 * 菜单两个选项：
 *   1. GAME BOY COLOR  -> 0x10040000 槽
 *   2. NINTENDO NES    -> 0x10080000 槽
 * 选定后读槽位向量表（槽+0x100），设 MSP/VTOR 直接跳转。
 * 子固件内 Y+B 看门狗重启，芯片 boot2 又会回到这里。
 */
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/gpio.h"

#include "board_config.h"
#include "display.h"
#include "input.h"

#define GBC_SLOT  0x10040000u
#define NES_SLOT  0x10080000u

#define C_BG      0x0000
#define C_TXT     0xFFFF
#define C_DIM     0x8410
#define C_BAR     0x1125   /* 深蓝标题栏 */
#define C_BOX     0x2104   /* 未选中项深灰底 */
#define C_SEL     0x07E0   /* 选中绿底 */
#define C_ONBAR   0x0000   /* 选中条上的文字 */

static void jump_to_slot(uint32_t slot)
{
    /* 子固件向量表在槽首 +0x100（前 256B 是 boot2 占位） */
    const uint32_t *vt = reinterpret_cast<const uint32_t *>(slot + 0x100);
    uint32_t initial_sp = vt[0];
    uint32_t reset_pc   = vt[1];

    __asm volatile("cpsid i" : : : "memory");
    __asm volatile("msr msp, %0" : : "r"(initial_sp) : "memory");
    *(volatile uint32_t *)0xE000ED08u = slot + 0x100;   /* SCB->VTOR */
    /* 子固件 crt0 不会重新开总中断（SDK 依赖复位默认 PRIMASK=0），
     * 必须在跳入前重新使能，否则子固件所有 ISR 永不触发。 */
    __asm volatile("cpsie i" : : : "memory");
    __asm volatile("bx %0" : : "r"(reset_pc) : "memory");
    __builtin_unreachable();
}

int main(void)
{
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(GB_SYS_CLOCK_KHZ, true);

    stdio_init_all();

    /* 背光：直接点亮（子固件里才做 PWM 调亮度） */
    gpio_init(GB_PIN_LCD_BL);
    gpio_set_dir(GB_PIN_LCD_BL, GPIO_OUT);
    gpio_put(GB_PIN_LCD_BL, 1);

    display_init();
    input_init();

    int sel = 0;
    const char *items[2] = {"GAME BOY", "NINTENDO"};
    const int box_y[2] = {92, 150};
    const int box_h = 44;

    /* ---- 静态部分：只画一次，之后不再整屏清黑，杜绝闪烁 ---- */
    display_fill(C_BG);
    display_rect(0, 0, 240, 36, C_BAR);
    display_text8(21, 10, "PICOBOY DUO", C_TXT, C_BAR, 2);
    display_text8(61, 48, "SELECT SYSTEM", C_DIM, C_BG, 1);

    /* 选项高亮条：只在选择变化时重绘 */
    auto draw_box = [&](int i)
    {
        bool on = (i == sel);
        display_rect(10, box_y[i], 220, box_h, on ? C_SEL : C_BOX);
        int tw = (int)strlen(items[i]) * 18;          /* scale 2 字宽 */
        display_text8((240 - tw) / 2, box_y[i] + 14, items[i],
                      on ? C_ONBAR : C_TXT, on ? C_SEL : C_BOX, 2);
    };
    draw_box(0);
    draw_box(1);

    display_text8(56, 220, "U/D MOVE  A OK", C_DIM, C_BG, 1);

    uint8_t prev = 0;
    while (true)
    {
        uint8_t cur = input_read();
        uint8_t pushed = (uint8_t)(cur & (uint8_t)~prev);
        prev = cur;

        if (pushed & 0xC0)                            /* UP 或 DOWN */
        {
            int old = sel;
            sel ^= 1;
            draw_box(old);
            draw_box(sel);
        }

        if (pushed & 0x01)                             /* A 选定跳转 */
        {
            jump_to_slot(sel == 0 ? GBC_SLOT : NES_SLOT);
        }

        sleep_ms(20);
    }
}
