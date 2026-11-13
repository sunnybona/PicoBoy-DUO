#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* 初始化 PWM DAC + 音频 DMA（环形缓冲自动续流）。 */
void audio_init(void);

/* miniGB APU 转接：peanut_gb 的 audio_read/audio_write 直接调这两个。 */
uint8_t audio_apu_read(uint16_t addr);
void    audio_apu_write(uint16_t addr, uint8_t val);

/* 生成一帧（AUDIO_SAMPLES_TOTAL 个 int16）并推入环形缓冲。每帧调一次。 */
void audio_apu_frame(void);

/* 把 miniGB APU 每帧生成的 AUDIO_SAMPLES_TOTAL 个 int16（双声道交错）
 * 混成单声道后推进环形缓冲。words 传 AUDIO_SAMPLES_TOTAL。 */
void audio_push_frame(const int16_t *stereo, size_t words);

/* 静音（flash 擦写等会阻塞几十毫秒的场合用）。 */
void audio_set_mute(int mute);

/* 音量档 0..10。 */
void audio_set_volume(int v);
int  audio_get_volume(void);

/* 诊断：环形缓冲当前积压的样本数（正=数据过剩，负=不足）。 */
int32_t audio_fill(void);

#endif
