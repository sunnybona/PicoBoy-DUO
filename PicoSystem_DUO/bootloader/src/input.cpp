/*
 * input.c — 8 个按键，当前硬件高电平有效（外部下拉，按下接 VCC）。
 * 引脚见 board_config.h；没接的键填 -1 即可。
 */
#include "input.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

typedef struct {
    int pin;
    uint8_t bit;
} btn_t;

static const btn_t s_btns[] = {
    { GB_PIN_BTN_A,     0x01 },
    { GB_PIN_BTN_B,     0x02 },
    { GB_PIN_BTN_Y,     0x04 },   /* Y -> GB SELECT */
    { GB_PIN_BTN_X,     0x08 },   /* X -> GB START  */
    { GB_PIN_BTN_RIGHT, 0x10 },
    { GB_PIN_BTN_LEFT,  0x20 },
    { GB_PIN_BTN_UP,    0x40 },
    { GB_PIN_BTN_DOWN,  0x80 },
};

#define BTN_COUNT (sizeof(s_btns) / sizeof(s_btns[0]))

void input_init(void)
{
    for(size_t i = 0; i < BTN_COUNT; i++)
    {
        int p = s_btns[i].pin;
        if(p < 0) continue;
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_pull_down(p);
    }
}

uint8_t input_read(void)
{
    uint8_t out = 0;
    for(size_t i = 0; i < BTN_COUNT; i++)
    {
        int p = s_btns[i].pin;
        if(p < 0) continue;
        if(gpio_get(p))           /* 高电平有效：外部下拉，按下接 VCC */
            out |= s_btns[i].bit;
    }
    return out;
}
