#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include "board_config.h"
/* minigb_apu 是按 C 编译的（lib/minigb_apu/minigb_apu.c），所以这里要 C 链接；
 * AUDIO_SAMPLES / AUDIO_SAMPLES_TOTAL 也靠它保证全工程一致。 */
extern "C" {
#include "minigb_apu.h"
} /* extern "C" */

/*
 * 采样率 = PWM 载波频率。
 * 250MHz / (2^12) = 61035.156Hz —— 取整 61035，误差 2.5ppm，听不出来。
 * 这条链路本质是"一位 PWM 当 12bit DAC，每个 PWM 周期更新一次电压"，
 * 所以采样率必须等于载波频率，缺一不可。
 *
 * 换主频时必须同步改这里（CMake 里也传同一个值）：
 *   载波 = GB_SYS_CLOCK_KHZ*1000 / 2^GB_AUDIO_PWM_BITS
 */
#ifndef AUDIO_SAMPLE_RATE
#if GB_SYS_CLOCK_KHZ == 250000 && GB_AUDIO_PWM_BITS == 12
#define AUDIO_SAMPLE_RATE 61035
#elif GB_SYS_CLOCK_KHZ == 200000 && GB_AUDIO_PWM_BITS == 12
#define AUDIO_SAMPLE_RATE 48828
#elif GB_SYS_CLOCK_KHZ == 133000 && GB_AUDIO_PWM_BITS == 12
#define AUDIO_SAMPLE_RATE 32470
#else
#error "请在 audio_config.h 里为你的主频/位宽补一个 AUDIO_SAMPLE_RATE"
#endif
#endif

#define AUDIO_PWM_WRAP   ((1u << GB_AUDIO_PWM_BITS) - 1u)
#define AUDIO_PWM_MID    (AUDIO_PWM_WRAP / 2u)   /* 中点 = 静音 */

/* AUDIO_SAMPLES_TOTAL 是 (unsigned)(...) 形式的浮点表达式，不能当数组长度用，
 * 这里给一个整型上界（每帧约 rate/59.7 个样本）。 */
#define AUDIO_PCM_WORDS  ((((AUDIO_SAMPLE_RATE) / 59) + 8) * 2)

#endif
