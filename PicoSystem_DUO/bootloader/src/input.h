#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* 按键位与 peanut_gb 的 JOYPAD_* 定义一致：
 * bit0 A, bit1 B, bit2 Select, bit3 Start,
 * bit4 Right, bit5 Left, bit6 Up, bit7 Down
 * 返回 1 = 按下。 */
uint8_t input_read(void);

void input_init(void);

#endif
