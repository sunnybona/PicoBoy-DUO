/*
 * audio.c — 单 GPIO PWM 当 12bit DAC（PicoSystem 一类的做法）
 *
 * 原理：PWM 载波频率 = 采样率。每个 PWM 周期更新一次占空比，
 * 经过喇叭前级的 RC 低通就还原成模拟波形。所以：
 *     wrap = 4095, clkdiv = 1, fclk = 250MHz  -> 61035.16Hz 采样率
 * DMA 由 PWM 的 DREQ 驱动，每产生一个 PWM wrap 就自动从环形缓冲取下一个样本，
 * CPU 完全不参与音频输出，只要每帧（16.74ms）往里写 1021 个样本即可。
 *
 * 因为 61.035kHz 采样率与 APU 的 1021x59.7275=60.98kHz 有 0.09% 的偏差，
 * 这里做了"橡胶带"式微校正：缓冲偏少就重复样本、偏多就丢样本，每帧最多 ±8 个。
 */
#include "audio.h"
#include "audio_config.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"

#define AUDIO_PIN   GB_PIN_AUDIO
#define RING_MASK   (GB_AUDIO_RING - 1u)
#define RING_HALF   (GB_AUDIO_RING / 2)

static uint16_t s_ring[GB_AUDIO_RING];
static uint32_t s_wp;                    /* 已写入样本总数（单调递增） */
static volatile uint32_t s_blocks;       /* DMA 已完成的环形块数 */
static uint8_t  s_dma_ch;
static uint     s_slice;
static int      s_muted;
static int      s_volume = 10;   /* 0..10 音量档 */

static struct minigb_apu_ctx s_apu;

uint8_t audio_apu_read(uint16_t addr)
{
    return minigb_apu_audio_read(&s_apu, addr);
}

void audio_apu_write(uint16_t addr, uint8_t val)
{
    minigb_apu_audio_write(&s_apu, addr, val);
}

static inline int32_t audio_consumed(void)
{
    /* trans_count 递减到 0 表示本块播完 */
    uint32_t remaining = dma_channel_hw_addr(s_dma_ch)->transfer_count;
    uint32_t in_block = GB_AUDIO_RING - remaining;
    return (int32_t)(s_blocks * GB_AUDIO_RING + in_block);
}

int32_t audio_fill(void)
{
    return (int32_t)s_wp - audio_consumed();
}

static void audio_dma_isr(void)
{
    uint32_t mask = 1u << s_dma_ch;
    if(dma_hw->ints0 & mask)
    {
        dma_hw->ints0 = mask;            /* 清标志 */
        s_blocks++;
        /* 重新从环形缓冲头部开始，形成无限循环 */
        dma_channel_transfer_from_buffer_now(s_dma_ch, s_ring, GB_AUDIO_RING);
    }
}

void audio_init(void)
{
    minigb_apu_audio_init(&s_apu);
    for(uint32_t i = 0; i < GB_AUDIO_RING; i++)
        s_ring[i] = AUDIO_PWM_MID;

    s_slice = pwm_gpio_to_slice_num(AUDIO_PIN);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, AUDIO_PWM_WRAP);

    /* 分频 = fclk / ((wrap+1) * 采样率)，绝大多数情况下正好是 1.0，
     * 但浮点算出来会略小于 1 而触发 SDK 的参数断言，所以整数化处理。 */
    float div = (float)clock_get_hz(clk_sys) /
                ((float)(AUDIO_PWM_WRAP + 1u) * (float)AUDIO_SAMPLE_RATE);
    int di = (int)div;
    int df = (int)((div - (float)di) * 16.0f + 0.5f);
    if(df >= 16) { df = 0; di += 1; }
    if(di < 1)   { di = 1; df = 0; }
    pwm_config_set_clkdiv_int_frac(&cfg, (uint8_t)di, (uint8_t)df);

    pwm_init(s_slice, &cfg, true);
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    pwm_set_gpio_level(AUDIO_PIN, AUDIO_PWM_MID);

    /* DMA：内存自增，写到 PWM 的 CC 寄存器；DREQ = PWM wrap */
    s_dma_ch = (uint8_t)dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pwm_get_dreq(s_slice));

    dma_channel_configure(s_dma_ch, &c,
                          &pwm_hw->slice[s_slice].cc,
                          s_ring, GB_AUDIO_RING, false);

    dma_channel_set_irq0_enabled(s_dma_ch, true);
    irq_set_exclusive_handler(DMA_IRQ_0, audio_dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_transfer_from_buffer_now(s_dma_ch, s_ring, GB_AUDIO_RING);

    /* 关键：把写指针先"预热"到半满。否则从开机到第一帧之间 DMA 已经消耗掉
     * 几百个样本，fill 会长时间偏负，播放指针会绕着写指针跑一圈，头半秒是旧数据。 */
    s_wp = RING_HALF;
}

void audio_set_mute(int mute)
{
    s_muted = mute;
    if(mute)
        pwm_set_gpio_level(AUDIO_PIN, AUDIO_PWM_MID);
}

void audio_set_volume(int v)
{
    if(v < 0) v = 0;
    if(v > 10) v = 10;
    s_volume = v;
}

int audio_get_volume(void)
{
    return s_volume;
}

/* 软件增益：minigb 内部各通道峰值只到 int16 满量程的约 2/3，整体放大 4/3 倍
 * 再映射，在响度和爆音之间取平衡；末尾的 ±MID 钳位保证 PWM 值始终合法，
 * 极响峰值只会轻微饱和，不会写出越界值。 */
#define AUDIO_GAIN_NUM  4
#define AUDIO_GAIN_DEN  3

static inline uint16_t to_level(int16_t v)
{
    /* int16 ±32768，先放大 1.5 倍 -> ±2047 的 PWM 占空，再按音量档缩放 */
    int32_t boosted = (int32_t)v * AUDIO_GAIN_NUM / AUDIO_GAIN_DEN;
    int32_t scaled = boosted / (1 << (16 - GB_AUDIO_PWM_BITS));
    scaled = scaled * s_volume / 10;
    if(scaled >  (int32_t)AUDIO_PWM_MID) scaled =  AUDIO_PWM_MID;
    if(scaled < -(int32_t)AUDIO_PWM_MID) scaled = -AUDIO_PWM_MID;
    return (uint16_t)((int32_t)AUDIO_PWM_MID + scaled);
}

void audio_push_frame(const int16_t *stereo, size_t words)
{
    size_t frames = words / 2;
    if(frames == 0 || frames > GB_AUDIO_RING) return;

    int32_t fill = audio_fill();

    /* 缓冲快满了：直接丢弃这一帧，让 DMA 追上来（比覆盖正在播的数据安全） */
    if(fill + (int32_t)frames > (int32_t)GB_AUDIO_RING - 4)
        return;

    /* 橡胶带校正方向：偏少补、偏多丢，每帧上限 8 个样本（约 0.8%） */
    int corr = 0;
    if(fill < RING_HALF - 128)      corr = +1;
    else if(fill > RING_HALF + 128) corr = -1;

    for(size_t i = 0; i < frames; i++)
    {
        int16_t l = stereo[i * 2 + 0];
        int16_t r = stereo[i * 2 + 1];
        int16_t mono = (int16_t)(((int32_t)l + (int32_t)r) >> 1);
        uint16_t lvl = s_muted ? AUDIO_PWM_MID : to_level(mono);

        if(corr < 0 && (i & 127u) == 0)
            continue;                     /* 丢一个样本，整体提前 */

        s_ring[s_wp & RING_MASK] = lvl;
        s_wp++;

        if(corr > 0 && (i & 127u) == 0)
        {
            s_ring[s_wp & RING_MASK] = lvl;   /* 重复一个样本，整体延后 */
            s_wp++;
        }
    }
}

void audio_apu_frame(void)
{
    static int16_t pcm[AUDIO_PCM_WORDS];

    minigb_apu_audio_callback(&s_apu, pcm);
    audio_push_frame(pcm, AUDIO_SAMPLES_TOTAL);
}
