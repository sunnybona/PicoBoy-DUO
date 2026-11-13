/*
 * display.cpp — ST7789 240×240，4 线 SPI，**PIO 位操作**推像素。
 *
 * ★ 传输方式（参考 pico-examples/pio/st7789_lcd）
 * ------------------------------------------------------------------
 * 用 PIO 状态机代替硬件 SPI：
 *
 *   .wrap_target
 *       out pins, 1   side 0    ; 没数据就停在这里（SCK 保持低）
 *       nop           side 1
 *   .wrap
 *
 * 两条指令 = 一个 bit，所以 bit 时钟 = clk_sys / (2 * clkdiv)。
 * 250MHz 下 clkdiv = 1.0 → 125MHz（PIO 上限，精确）；1.5625 → 80MHz；15.625 → 8MHz。
 * （PIO 一条指令一个周期，两条指令一个 bit，所以位时钟理论上限是 clk_sys/2。）
 *
 * 一个状态机按"相位"重配：
 *   命令相位：shift_left(MSB first) + autopull 8bit。CPU 写 1 个字节，
 *             窄写（byte store）在 RP2040 上会复制到 32 位寄存器的四个字节，
 *             正好左对齐，MSB-first 出去就是对的（和官方例子同一招）。
 *   像素相位：shift_left(MSB first) + autopull 32bit。一个 32 位字 = 两个像素：
 *             word = (c0 << 16) | c1  →  bit31..24 = c0 高字节，依次发出 ✓
 *
 * ★ 像素由 DMA 送进 PIO 的 TX FIFO
 * ------------------------------------------------------------------
 *   dma_channel: 32 位、读地址递增、写地址固定 &pio->txf[sm]、
 *                DREQ = pio_get_dreq(pio, sm, true)
 *   于是"帧缓冲"直接就是 PIO 要吃的字流：每个 uint32 存一对像素
 *   （高 16 位 = 左边那个），一帧 160×144 = 11520 个字，一条 DMA 搬完。
 *   core1 只需要：开窗口 → 起 DMA → 等完成 → 抬 CS。
 *
 *   注意：因为 DMA 是 32 位直搬，帧缓冲不再是 uint16 RGB565 数组，
 *   而是"成对的字"。命令行缓冲（fill/text/splash 用）仍然对外保持 uint16
 *   像素接口，在 display_kick_row() 里现打包成字再交给 DMA。
 *
 * 为什么不用硬件 SPI 的 DMA：
 *   - 16-bit SPI 帧 + DMA_SIZE_16 在高速下容易出现"某一行多/少几个字节"，
 *     后面每行起点累积错位（斜接缝、竖线打成点阵）；
 *   - 8-bit 帧每像素要发两帧，帧数翻倍；
 *   - PIO 一个 bit 两条指令，节拍由硬件给死，软件只需要保证 FIFO 里有字，
 *     不存在"抬 CS 前 FIFO 没排空"这类时序问题。
 *
 * ★ 关于"没有 VSYNC/TE 引脚"
 * ------------------------------------------------------------------
 * 1) 帧节拍只能靠时间轴：主循环按 GB 的 16742.706µs/帧 定时，
 *    用 time_us_64() + 小数累加器，保证长时间不漂移（见 main.cpp）。
 * 2) 面板刷新的相位不可见，因此不可能做到"零撕裂"。策略是把撕裂压到最小：
 *    整帧数据连续写下去（CASET/RASET 只发一次，全程 CS 低）。
 * 3) 尽量把面板自己的刷新率调到接近 GB 的 59.73Hz（0xC6 FRCTRL2）。
 * 4) 真想完全消除撕裂，只有两条路：给面板引一根 TE 线接 GPIO
 *    （把 GB_PIN_LCD_VSYNC 指过去，本文件会自动探测并等 TE 再开写）；
 *    或者双缓冲整帧（本文件在 160×144 下就是这么干的，见下）。
 */
#include "display.h"
#include "font3x5.h"
#include "font_8x8.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"

#include "st7789_lcd.pio.h"

#define LCD_CS         GB_PIN_LCD_CS
#define LCD_DC         GB_PIN_LCD_DC
#define LCD_RST        GB_PIN_LCD_RST
#define LCD_BL         GB_PIN_LCD_BL

/* 一行像素缓冲（CPU 按 uint16 像素填，kick 时打包成字交给 DMA）。
 * PIO 一次发 2 个像素，所以宽度取偶数。 */
static uint16_t s_rowbuf[GB_SCREEN_W];
/* 打包好的字（两个字像素），DMA 从这里搬 */
static uint32_t s_rowword[GB_SCREEN_W / 2];

/* ---- PIO 状态机 ---------------------------------------------------- */
static PIO   s_pio = pio0;
static uint  s_sm;
static uint  s_prog_off;
static int   s_dma_chan = -1;       /* 喂 PIO TX FIFO 的 DMA 通道 */
static float s_div_pix = 15.625f;   /* 像素相位分频 */
static float s_div_cmd = 15.625f;   /* 命令相位分频（8MHz） */
static unsigned int s_bit_hz = GB_LCD_SPI_HZ;
static unsigned int s_cmd_hz = 8000000;
static int   s_pixel_mode;         /* 当前 SM 是否处于像素配置 */

/* GB 画面在 240x240 里的位置 */
#if GB_DISPLAY_SCALE15
#define GB_FRAME_W  240
#define GB_FRAME_H  216
#define GB_FRAME_X  0
#define GB_FRAME_Y  12
#else
#define GB_FRAME_W  160
#define GB_FRAME_H  144
#define GB_FRAME_X  ((GB_SCREEN_W - 160) / 2)
#define GB_FRAME_Y  ((GB_SCREEN_H - 144) / 2)
#endif

/* 4 级色阶（经典 DMG 绿）：0 最亮 -> 3 最暗 */
static uint16_t s_pal565[4];

void display_set_dmg_palette(const uint16_t pal4[4])
{
    for(int i = 0; i < 4; i++)
        s_pal565[i] = pal4[i];
}

/* ---- 半幅乒乓 --------------------------------------------------------
 * 一帧切成上下两半，每半一块缓冲，两块乒乓：
 *   core0 画下半的时候，core1 正在推上半。
 *
 *   160×144：每半 72 行 → 2 × (72×80 字)      = 2 × 23040B = 46080B
 *   240×216：每半 108 行 → 2 × (108×120 字)   = 2 × 51840B = 103680B
 *
 * 缓冲是"像素对"数组：一个字 = (左像素 << 16) | 右像素，DMA 32 位直搬。
 * 因为 144 / 216 都能被 2 整除，带边界正好落在帧中线和帧尾，
 * 每条带推一次 window（CASET/RASET 一次），几何上不会错位。
 */
#define GB_WORDS_PER_ROW (GB_FRAME_W / 2)
#define GB_BAND_ROWS     (GB_FRAME_H / 2)
#define GB_BAND_SLOTS    2

static uint32_t s_band_pool[GB_BAND_SLOTS][(size_t)GB_BAND_ROWS * GB_WORDS_PER_ROW];

/* core0 侧：正在填的那条带 */
static int       s_band_id = -1;        /* 最近一次分配的带序号，跨帧单调递增 */
static int       s_band_active;         /* 1 = 有一条带正在填 */
static int       s_band_base_row;       /* 这条带在帧内的起始行 */
static int       s_band_rows;           /* 这条带已经填了几行 */
static uint32_t *s_band;                /* = s_band_pool[s_band_id % 2] */

/* core0 -> core1 的交付环（两项，正好对应两块缓冲） */
typedef struct { int slot, row, rows; } band_order_t;
static volatile band_order_t s_ord[GB_BAND_SLOTS];
static volatile int s_head;             /* 已发布的带数 */
static volatile int s_tail;             /* 已推完的带数 */

static int s_gb_row;
static volatile int s_pushed_rows;
static volatile uint32_t s_push_us;    /* 上一条带推屏耗时（core1 量） */

static inline uint32_t mk_pair(uint16_t c0, uint16_t c1)
{
    return ((uint32_t)c0 << 16) | (uint32_t)c1;
}

/* TE(VSYNC) 探测结果：1 = 这一脚真有 30~70Hz 脉冲 */
static int s_te_ok;

/* ------------------------------------------------------------------ */
/* PIO 低层                                                            */
/* ------------------------------------------------------------------ */
/* 由目标位时钟算 PIO 分频。PIO 的分频只有"整数 + 1/256 小数"两级精度，
 * 所以不是每个位时钟都能精确表达：这里取最接近的可表示值，别让 SDK 直接截断。
 *   250MHz 下 125MHz 位时钟 → clkdiv = 1.0        （精确，PIO 上限 clk_sys/2）
 *   250MHz 下  80MHz    → 1.5625                  （精确）
 *   250MHz 下 120MHz    → 1.0417 → 1 + 10/256     （≈120.3MHz，差 0.25%） */
static float lcd_div_for(unsigned int bit_hz)
{
    if(!bit_hz) bit_hz = 8000000u;
    float d = (float)clock_get_hz(clk_sys) / (2.0f * (float)bit_hz);
    if(d < 1.0f) d = 1.0f;
    if(d > 255.0f + 255.0f / 256.0f) d = 255.0f + 255.0f / 256.0f;

    float di = (float)(int)d;
    int frac = (int)((d - di) * 256.0f + 0.5f);
    if(frac > 255) { frac = 0; di += 1.0f; }
    return di + (float)frac / 256.0f;
}

static void lcd_update_div(void)
{
    unsigned int pix_hz = s_bit_hz ? s_bit_hz : 8000000u;
    s_cmd_hz = pix_hz > 8000000u ? 8000000u : pix_hz;   /* 命令相位永远封顶 8MHz */

    s_div_pix = lcd_div_for(pix_hz);
    s_div_cmd = lcd_div_for(s_cmd_hz);
}

/* 等"最后一个 bit 也真的移出去了"。
 * 不用 fdebug 的 TXSTALL 标志：SM 空闲时那个粘滞位已经是置位的，先清再等会直接卡死。
 * 这里改成：FIFO 空之后，再等够 40 个 bit 的时间（OSR 最多剩 32 bit，够）。 */
static void pio_tx_idle(void)
{
    while(!pio_sm_is_tx_fifo_empty(s_pio, s_sm))
        tight_loop_contents();

    unsigned int hz = s_pixel_mode ? s_bit_hz : s_cmd_hz;
    if(!hz) hz = 8000000u;
    uint32_t wait_us = (uint32_t)(40000000ull / hz) + 1u;
    busy_wait_us(wait_us);
}

static void lcd_pio_configure(int pixel)
{
    pio_sm_set_enabled(s_pio, s_sm, false);

    pio_sm_config c = st7789_lcd_program_get_default_config(s_prog_off);
    sm_config_set_sideset_pins(&c, GB_PIN_LCD_SCK);
    sm_config_set_out_pins(&c, GB_PIN_LCD_MOSI, 1);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    /* false = 左移 = MSB first；命令吃 8bit，像素吃 32bit */
    sm_config_set_out_shift(&c, false, true, pixel ? 32 : 8);
    sm_config_set_clkdiv(&c, pixel ? s_div_pix : s_div_cmd);

    pio_sm_init(s_pio, s_sm, s_prog_off, &c);
    pio_sm_set_enabled(s_pio, s_sm, true);
    s_pixel_mode = pixel;
}

static inline void pio_put_byte(uint8_t b)
{
    while(pio_sm_is_tx_fifo_full(s_pio, s_sm))
        tight_loop_contents();
    /* 窄写：RP2040 会把这一字节复制到 32 位寄存器的四个字节，正好左对齐 */
    *((volatile uint8_t *)&s_pio->txf[s_sm]) = b;
}

/* ---- DMA：把"像素对"字流灌进 PIO 的 TX FIFO -------------------------
 * DREQ 用 PIO 的 TX DREQ，DMA 只在 FIFO 有空位时才写，
 * 所以节奏完全由 PIO 的位时钟决定，不依赖 CPU 喂得多快。 */
static void dma_init_once(void)
{
    if(s_dma_chan >= 0) return;

    s_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));
    dma_channel_configure(s_dma_chan, &c,
                          &s_pio->txf[s_sm],   /* 写地址固定：TX FIFO */
                          NULL,                /* 读地址按次设置 */
                          0, false);
}

/* 把 nwords 个像素对直接搬进 TX FIFO，并等它搬完（只搬完，不代表已经发完） */
static void push_words(const uint32_t *src, size_t nwords)
{
    if(nwords == 0) return;
    dma_init_once();

    dma_channel_set_read_addr(s_dma_chan, src, false);
    dma_channel_set_trans_count(s_dma_chan, (uint32_t)nwords, true);
    dma_channel_wait_for_finish_blocking(s_dma_chan);
}

/* 一个 32 位字 = 两个像素，MSB-first 发出去正好是 c0 高/低、c1 高/低。
 * 只剩"命令/极小片段"还会走这条路，整行/整帧都交给 DMA。 */
static inline void pio_put_pixels(uint16_t c0, uint16_t c1)
{
    while(pio_sm_is_tx_fifo_full(s_pio, s_sm))
        tight_loop_contents();
    s_pio->txf[s_sm] = mk_pair(c0, c1);
}

/* ------------------------------------------------------------------ */
/* 命令与窗口                                                          */
/* ------------------------------------------------------------------ */
static void lcd_cmd(uint8_t cmd)
{
    pio_tx_idle();
    if(s_pixel_mode) lcd_pio_configure(0);

    gpio_put(LCD_DC, 0);
    gpio_put(LCD_CS, 0);
    pio_put_byte(cmd);
    pio_tx_idle();
    gpio_put(LCD_CS, 1);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    if(len == 0) return;
    pio_tx_idle();
    if(s_pixel_mode) lcd_pio_configure(0);

    gpio_put(LCD_DC, 1);
    gpio_put(LCD_CS, 0);
    for(size_t i = 0; i < len; i++)
        pio_put_byte(data[i]);
    pio_tx_idle();
    gpio_put(LCD_CS, 1);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_cmd(cmd);
    lcd_data(data, len);
}

static void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint8_t caset[4] = { (uint8_t)(x >> 8), (uint8_t)x,
                         (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1) };
    uint8_t raset[4] = { (uint8_t)(y >> 8), (uint8_t)y,
                         (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1) };
    lcd_cmd_data(0x2A, caset, 4);
    lcd_cmd_data(0x2B, raset, 4);
}

/* 开窗口 + RAMWR + 切到像素相位 + CS 拉低，之后连续写像素 */
static void lcd_begin_pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    lcd_set_window(x, y, w, h);
    lcd_cmd(0x2C);                  /* RAMWR */

    pio_tx_idle();
    lcd_pio_configure(1);           /* 切到 32bit 像素相位 */

    gpio_put(LCD_DC, 1);
    gpio_put(LCD_CS, 0);
}

static void lcd_end_pixels(void)
{
    pio_tx_idle();
    gpio_put(LCD_CS, 1);
}

/* ------------------------------------------------------------------ */
/* TE 探测                                                             */
/* ------------------------------------------------------------------ */
#if GB_PIN_LCD_VSYNC >= 0
/* 在 display_init 末尾调用：把 VSYNC 脚当输入(下拉)，观察最长 120ms。
 * 必须看到 >=3 个上升沿、且间隔落在 8ms~40ms 才算真 TE，悬空脚噪声不会满足。 */
static void te_probe(void)
{
    gpio_init(GB_PIN_LCD_VSYNC);
    gpio_set_dir(GB_PIN_LCD_VSYNC, GPIO_IN);
    gpio_pull_down(GB_PIN_LCD_VSYNC);
    sleep_us(200);

    uint64_t t0 = time_us_64();
    int edges = 0;
    uint64_t last = 0, min_gap = UINT64_MAX, max_gap = 0;

    while(time_us_64() - t0 < 120000u)
    {
        if(gpio_get(GB_PIN_LCD_VSYNC))
        {
            uint64_t now = time_us_64();
            edges++;
            if(last && now - last < min_gap) min_gap = now - last;
            if(last && now - last > max_gap) max_gap = now - last;
            last = now;
            uint64_t w = time_us_64();
            while(gpio_get(GB_PIN_LCD_VSYNC) && time_us_64() - w < 20000u)
                tight_loop_contents();
        }
    }

    s_te_ok = (edges >= 3) && (min_gap >= 8000u) && (max_gap <= 40000u) ? 1 : 0;
}

/* 用 TE 做帧同步：等一个上升沿再开写。带 25ms 超时，接歪/断线也不会卡死主循环。 */
static void te_wait_rising(void)
{
    if(!s_te_ok) return;
    uint64_t t0 = time_us_64();
    while(gpio_get(GB_PIN_LCD_VSYNC))
    {
        if(time_us_64() - t0 > 25000u) return;
        tight_loop_contents();
    }
}
#endif

int display_has_te(void)
{
    return s_te_ok;
}

/* ------------------------------------------------------------------ */
/* 对外：流式写入                                                      */
/* ------------------------------------------------------------------ */
void display_stream_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if(w & 1) w++;                  /* PIO 一次发两个像素，宽度凑偶 */
    lcd_begin_pixels(x, y, w, h);
}

uint16_t *display_next_row(void)
{
    /* 只有一块行缓冲：先等上一行彻底发完再覆盖 */
    pio_tx_idle();
    return s_rowbuf;
}

void display_kick_row(uint16_t w)
{
    if(w == 0) return;
    if(w & 1) w &= (uint16_t)~1u;

    size_t nwords = (size_t)w / 2;
    for(size_t i = 0; i < nwords; i++)
        s_rowword[i] = mk_pair(s_rowbuf[2 * i], s_rowbuf[2 * i + 1]);

    push_words(s_rowword, nwords);
}

void display_stream_end(void)
{
    lcd_end_pixels();
}

void display_set_blocking(int on)
{
    /* PIO 已经取代了 DMA/阻塞写两种方式：两个都由硬件节拍保证不乱序，
     * 这个开关保留只为不破坏老的排查代码。 */
    (void)on;
}

void display_set_spi_mode3(int on)
{
    /* st7789_lcd.pio 是固定的 mode0（SCK 闲低、上升沿采样）。
     * 这块 panel 在 mode0 下就是对的，mode3 不再提供。 */
    (void)on;
}

void display_set_spi_hz(unsigned int hz)
{
    if(hz == 0) return;
    pio_tx_idle();
    s_bit_hz = hz;
    lcd_update_div();
    if(s_pixel_mode) lcd_pio_configure(1);   /* 立刻生效 */
}

/* ------------------------------------------------------------------ */
/* 对外：整屏/区域填色                                                  */
/* ------------------------------------------------------------------ */
void display_fill(uint16_t rgb565)
{
    display_wait_idle();          /* core0 直接写屏前先让 core1 收手 */
    lcd_begin_pixels(0, 0, GB_SCREEN_W, GB_SCREEN_H);
    for(int y = 0; y < GB_SCREEN_H; y++)
    {
        for(int i = 0; i < GB_SCREEN_W; i++)
            s_rowbuf[i] = rgb565;
        display_kick_row(GB_SCREEN_W);
    }
    lcd_end_pixels();
}

/* 矩形填充（自动裁剪到屏幕、宽度凑偶），用于选中高亮条。 */
void display_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if(x + w > GB_SCREEN_W) w = GB_SCREEN_W - x;
    if(y + h > GB_SCREEN_H) h = GB_SCREEN_H - y;
    if(w & 1) w--;
    if(w <= 0 || h <= 0) return;

    display_wait_idle();
    lcd_begin_pixels((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    for(int r = 0; r < h; r++)
    {
        for(int i = 0; i < w; i++)
            s_rowbuf[i] = rgb565;
        display_kick_row((uint16_t)w);
    }
    lcd_end_pixels();
}

/* 把 GB 显示区（GB_FRAME_X/Y 起、GB_FRAME_W×GB_FRAME_H）刷成纯色。
 * 开始推第一帧之前先清一次黑，避免面板 GRAM 里的残留内容混进画面。 */
void display_clear_gb_area(uint16_t rgb565)
{
    display_wait_idle();
    lcd_begin_pixels((uint16_t)GB_FRAME_X, (uint16_t)GB_FRAME_Y,
                     (uint16_t)GB_FRAME_W, (uint16_t)GB_FRAME_H);
    for(int y = 0; y < GB_FRAME_H; y++)
    {
        for(int i = 0; i < GB_FRAME_W; i++)
            s_rowbuf[i] = rgb565;
        display_kick_row(GB_FRAME_W);
    }
    lcd_end_pixels();
}

/* ------------------------------------------------------------------ */
/* 双核：core0 一条带一条带填，core1 一条带一条带推                      */
/* ------------------------------------------------------------------ */

/* 把 buf 的 rows 行推给面板（面板上的起始行是 GB_FRAME_Y + row0）。
 * 一次 window 开好，然后用一条 DMA 把整条带的像素对灌进 PIO FIFO。
 * 只在 core1 上调用。 */
static void push_band(const uint32_t *buf, int row0, int rows)
{
    if(rows <= 0) return;
    lcd_begin_pixels((uint16_t)GB_FRAME_X, (uint16_t)(GB_FRAME_Y + row0),
                     (uint16_t)GB_FRAME_W, (uint16_t)rows);
    push_words(buf, (size_t)rows * GB_WORDS_PER_ROW);
    lcd_end_pixels();
}

/* 开一条新带：这块缓冲上一次装的是 id-GB_BAND_SLOTS 那条带，
 * 必须等它推完才能覆盖。 */
static void band_open(void)
{
    int id = s_band_id + 1;
    while(s_tail < id - GB_BAND_SLOTS + 1)
        __asm volatile ("nop");

    s_band_id   = id;
    s_band      = s_band_pool[id % GB_BAND_SLOTS];
    s_band_rows = 0;
}

/* 把当前这条带交给 core1 */
static void band_publish(void)
{
    if(!s_band_active || s_band_rows <= 0) return;

    int id = s_band_id;                  /* 注意：不要动 s_band_id，
                                          * 它是"已分配到第几条带"的单调计数，
                                          * 复位会让下一条带又拿到同一个 id/slot，
                                          * 表现为只有上半屏在刷、下半屏永远不动 */
    int slot = id % GB_BAND_SLOTS;
    s_ord[slot].slot = slot;
    s_ord[slot].row  = s_band_base_row;
    s_ord[slot].rows = s_band_rows;
    __dmb();                    /* 交付单写完再造 head，别让 core1 读到半新半旧 */
    s_head = id + 1;
    __sev();

    s_band_active = 0;
    s_band_rows   = 0;
}

/* 取"输出第 r 行"要写的那一行缓冲。行是顺序写的（GB 逐行出画），
 * 写满 GB_BAND_ROWS 行就自动把这条带交出去、开下一条。 */
static uint32_t *band_row_ptr(int r)
{
    if(!s_band_active || s_band_rows >= GB_BAND_ROWS ||
       r != s_band_base_row + s_band_rows)
    {
        if(s_band_active) band_publish();
        band_open();
        s_band_base_row = r;
        s_band_active   = 1;
    }
    s_band_rows++;
    return s_band + (size_t)(s_band_rows - 1) * GB_WORDS_PER_ROW;
}

void display_begin_gb_frame(void)
{
    /* 上一帧如果没写满一条带（144/216 都能被 2 整除，正常不会），先把它交出去 */
    if(s_band_active) band_publish();
    s_gb_row = 0;
    s_pushed_rows = 0;
}

int display_gb_rows(void)
{
    return s_gb_row;
}

void display_end_gb_frame(void)
{
#if GB_PIN_LCD_VSYNC >= 0
    te_wait_rising();      /* 有 TE 就踩着面板的帧消隐开写 */
#endif
    /* 最后一条带可能不满（144/216 都能被 2 整除，正常正好满），补交一次 */
    if(s_band_active) band_publish();
}

int display_pushed_rows(void)
{
    return s_pushed_rows;
}

uint32_t display_push_us(void)
{
    return s_push_us;
}

/* ------------------------------------------------------------------ */
/* core1：唯一职责就是把 core0 交付的每一条带推给面板                   */
/* ------------------------------------------------------------------ */
static void core1_push_worker(void)
{
    while(true)
    {
        while(s_head == s_tail)
            __wfe();               /* head 由 core0 改写后 __sev()，不会漏唤醒 */

        __dmb();
        int id   = s_tail;
        int slot = s_ord[id % GB_BAND_SLOTS].slot;
        int row  = s_ord[id % GB_BAND_SLOTS].row;
        int rows = s_ord[id % GB_BAND_SLOTS].rows;

        uint32_t t0 = time_us_64();
        push_band(s_band_pool[slot], row, rows);
        s_push_us = (uint32_t)(time_us_64() - t0);

        s_pushed_rows += rows;
        __dmb();
        s_tail = id + 1;
        __sev();                   /* 叫醒在等这条带推完的 core0 */
    }
}

void display_start_pusher(void)
{
    multicore_reset_core1();
    multicore_launch_core1(core1_push_worker);
}

void display_wait_idle(void)
{
    /* 这里故意用忙等而不是 __wfi()：RP2040 上默认没有周期性的 tick 中断，
     * 若 USB 主机没接上就没有 SOF 中断，WFI 可能一直等不到唤醒。 */
    while(s_tail < s_head)
        __asm volatile ("nop");
}

/* ------------------------------------------------------------------ */
/* 文字：把 3x5 点阵直接画进流式行缓冲，不需要整帧 framebuffer           */
/* ------------------------------------------------------------------ */
int display_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if(scale < 1) scale = 1;
    int len = (int)strlen(s);
    int h = 5 * scale;
    int w = len * 4 * scale;                 /* 3 列字形 + 1 列字距 */
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(w > GB_SCREEN_W - x) w = GB_SCREEN_W - x;
    if(w & 1) w--;                           /* PIO 成对发像素，宽度凑偶 */
    if(y + h > GB_SCREEN_H) h = GB_SCREEN_H - y;
    if(w <= 0 || h <= 0) return 0;

    display_wait_idle();          /* 别和 core1 抢 SPI */
    display_stream_begin((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    for(int r = 0; r < h; r++)
    {
        uint16_t *dst = display_next_row();
        for(int i = 0; i < w; i++)
            dst[i] = bg;

        int rowg = r / scale;
        int gx = 0;
        for(int i = 0; i < len && gx + 4 * scale <= w; i++)
        {
            const uint8_t *g = font3x5_glyph(s[i]);
            if(g)
            {
                int bits = g[rowg];
                for(int col = 0; col < 3; col++)
                {
                    if(!((bits >> (2 - col)) & 1)) continue;
                    for(int sx = 0; sx < scale; sx++)
                    {
                        int px = gx + col * scale + sx;
                        if(px >= 0 && px < w) dst[px] = fg;
                    }
                }
            }
            gx += 4 * scale;
        }
        display_kick_row((uint16_t)w);
    }
    display_stream_end();
    return w;
}

/* 8x8 大字体版（font_8x8，bit0=最左列，每字符 8 列 + 1 列间距），scale 为放大倍数。 */
int display_text8(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    if(scale < 1) scale = 1;
    int len = (int)strlen(s);
    int h = 8 * scale;
    int w = len * 9 * scale;                 /* 8 列字形 + 1 列字距 */
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(w > GB_SCREEN_W - x) w = GB_SCREEN_W - x;
    if(w & 1) w--;                           /* PIO 成对发像素，宽度凑偶 */
    if(y + h > GB_SCREEN_H) h = GB_SCREEN_H - y;
    if(w <= 0 || h <= 0) return 0;

    display_wait_idle();          /* 别和 core1 抢 SPI */
    display_stream_begin((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    for(int r = 0; r < h; r++)
    {
        uint16_t *dst = display_next_row();
        for(int i = 0; i < w; i++)
            dst[i] = bg;

        int rowg = r / scale;
        int gx = 0;
        /* 只要求字形本身的 8 列落在窗口内即可绘制；宽度凑偶可能截掉最后一列，
         * 但那是字距列，不能因此丢掉整个末字符。 */
        for(int i = 0; i < len && gx + 8 * scale <= w; i++)
        {
            int c = s[i];
            if(c >= 32 && c < 32 + 95)
            {
                uint8_t bits = (uint8_t)font_8x8[(c - 32) + rowg * 95];
                for(int col = 0; col < 8; col++)
                {
                    if(!((bits >> col) & 1)) continue;
                    for(int sx = 0; sx < scale; sx++)
                    {
                        int px = gx + col * scale + sx;
                        if(px >= 0 && px < w) dst[px] = fg;
                    }
                }
            }
            gx += 9 * scale;
        }
        display_kick_row((uint16_t)w);
    }
    display_stream_end();
    return w;
}

void display_message(const char *l1, const char *l2, const char *l3)
{
    display_fill(0x0000);
    const char *lines[3] = { l1, l2, l3 };
    int used = 0;
    for(int i = 0; i < 3; i++)
    {
        if(!lines[i] || !lines[i][0]) continue;
        int scale = 6;
        int w = (int)strlen(lines[i]) * 4 * scale;
        while(scale > 3 && w > GB_SCREEN_W - 16)
        {
            scale--;
            w = (int)strlen(lines[i]) * 4 * scale;
        }
        int x = (GB_SCREEN_W - w) / 2;
        if(x < 0) x = 0;
        display_text(x, 60 + used * 44, lines[i], 0xFFE0, 0x0000, (uint8_t)scale);
        used++;
    }
}

/* ------------------------------------------------------------------ */
/* 上电自检图                                                          */
/* ------------------------------------------------------------------ */
void display_splash(const char *txt)
{
    /* 四色横条，从上到下：白 / 红 / 绿 / 蓝 */
    static const uint16_t band[4] = { 0xFFFF, 0xF800, 0x07E0, 0x001F };

    display_wait_idle();
    lcd_begin_pixels(0, 0, GB_SCREEN_W, GB_SCREEN_H);
    for(int y = 0; y < GB_SCREEN_H; y++)
    {
        uint16_t c = band[(y * 4) / GB_SCREEN_H];
        for(int x = 0; x < GB_SCREEN_W; x++)
            s_rowbuf[x] = (x < 6 || x >= GB_SCREEN_W - 6) ? 0x0000u : c;
        display_kick_row(GB_SCREEN_W);
    }
    lcd_end_pixels();

    /* 中间压一条黑带放标题，让字不压在色条上 */
    uint16_t y0 = GB_SCREEN_H / 2 - 28;
    uint16_t hh = 56;
    lcd_begin_pixels(0, y0, GB_SCREEN_W, hh);
    for(int r = 0; r < (int)hh; r++)
    {
        for(int i = 0; i < GB_SCREEN_W; i++) s_rowbuf[i] = 0x0000;
        display_kick_row(GB_SCREEN_W);
    }
    lcd_end_pixels();

    if(txt && txt[0])
    {
        int scale = 5;
        int w = (int)strlen(txt) * 4 * scale;
        while(scale > 2 && w > GB_SCREEN_W - 8)
        {
            scale--;
            w = (int)strlen(txt) * 4 * scale;
        }
        int x = (GB_SCREEN_W - w) / 2;
        if(x < 0) x = 0;
        int y = GB_SCREEN_H / 2 - (5 * scale) / 2;
        display_text(x, y, txt, 0xFFFF, 0x0000, (uint8_t)scale);
    }
}

/* ------------------------------------------------------------------ */
/* GB 扫描线推送                                                        */
/* ------------------------------------------------------------------ */
#if GB_DISPLAY_SCALE15
/* 240 个目标像素 -> 160 个源像素的最近邻映射，第一次用的时候算一遍 */
static uint8_t s_xmap[240];
static int s_xmap_ready;

static void build_xmap(void)
{
    for(int x = 0; x < 240; x++)
        s_xmap[x] = (uint8_t)((x * 2) / 3);
    s_xmap_ready = 1;
}
#endif

void display_push_gb_line(const uint8_t *pixels, uint8_t line)
{
#if GB_DISPLAY_SCALE15
    if(!s_xmap_ready) build_xmap();

    /* 垂直 1.5 倍：第 i 条源扫描线输出 1 或 2 行，累计正好 216 行 */
    int r0 = (3 * line + 1) / 2;
    if(r0 >= GB_FRAME_H) return;
    int r1 = (3 * (line + 1) + 1) / 2;
    if(r1 > GB_FRAME_H) r1 = GB_FRAME_H;
    for(int r = r0; r < r1; r++)
    {
        uint32_t *dst = band_row_ptr(r);
        for(int x = 0; x < GB_FRAME_W; x += 2)
            dst[x / 2] = mk_pair(s_pal565[pixels[s_xmap[x]] & 3],
                                 s_pal565[pixels[s_xmap[x + 1]] & 3]);
    }
    if(r1 > s_gb_row) s_gb_row = r1;
#else
    if(line >= GB_FRAME_H) return;
    uint32_t *dst = band_row_ptr(line);
    for(int x = 0; x < GB_FRAME_W; x += 2)
        dst[x / 2] = mk_pair(s_pal565[pixels[x] & 3],
                             s_pal565[pixels[x + 1] & 3]);
    s_gb_row = line + 1;
#endif
}

/* 颜色已经算好的行（RGB565，160 像素）：CGB 模式用它。
 * CGB 给 front-end 的是调色板索引（0x00-0x1F 背景 / 0x20-0x3F 精灵），
 * 颜色只有上层查完 fixPalette 才知道，所以这里不再查 4 级色表。 */
void display_push_rgb_line(const uint16_t *rgb, uint8_t line)
{
#if GB_DISPLAY_SCALE15
    if(!s_xmap_ready) build_xmap();

    int r0 = (3 * line + 1) / 2;
    if(r0 >= GB_FRAME_H) return;
    int r1 = (3 * (line + 1) + 1) / 2;
    if(r1 > GB_FRAME_H) r1 = GB_FRAME_H;
    for(int r = r0; r < r1; r++)
    {
        uint32_t *dst = band_row_ptr(r);
        for(int x = 0; x < GB_FRAME_W; x += 2)
            dst[x / 2] = mk_pair(rgb[s_xmap[x]], rgb[s_xmap[x + 1]]);
    }
    if(r1 > s_gb_row) s_gb_row = r1;
#else
    if(line >= GB_FRAME_H) return;
    uint32_t *dst = band_row_ptr(line);
    for(int x = 0; x < GB_FRAME_W; x += 2)
        dst[x / 2] = mk_pair(rgb[x], rgb[x + 1]);
    s_gb_row = line + 1;
#endif
}

/* ------------------------------------------------------------------ */
void display_init(void)
{
    s_pal565[0] = display_rgb(0x9B, 0xBC, 0x0F);
    s_pal565[1] = display_rgb(0x8B, 0xAC, 0x0F);
    s_pal565[2] = display_rgb(0x30, 0x62, 0x30);
    s_pal565[3] = display_rgb(0x0F, 0x38, 0x0F);

    gpio_init(LCD_CS);  gpio_set_dir(LCD_CS, GPIO_OUT);  gpio_put(LCD_CS, 1);
    gpio_init(LCD_DC);  gpio_set_dir(LCD_DC, GPIO_OUT);  gpio_put(LCD_DC, 1);

    if(LCD_RST >= 0)
    {
        gpio_init(LCD_RST); gpio_set_dir(LCD_RST, GPIO_OUT);
        gpio_put(LCD_RST, 1); sleep_ms(5);
        gpio_put(LCD_RST, 0); sleep_ms(20);
        gpio_put(LCD_RST, 1); sleep_ms(120);
    }

    if(LCD_BL >= 0)
    {
        gpio_init(LCD_BL); gpio_set_dir(LCD_BL, GPIO_OUT);
        gpio_put(LCD_BL, GB_PIN_LCD_BL_ACTIVE_HIGH ? 1 : 0);
    }

    /* ---- PIO 初始化 ---- */
    s_pio = pio0;
    s_sm  = 0;
    s_prog_off = pio_add_program(s_pio, &st7789_lcd_program);

    pio_gpio_init(s_pio, GB_PIN_LCD_MOSI);
    pio_gpio_init(s_pio, GB_PIN_LCD_SCK);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, GB_PIN_LCD_MOSI, 1, true);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, GB_PIN_LCD_SCK, 1, true);

    /* 面包板长线的边沿会被拖成斜坡还会振铃，用最高驱动 + 最快 slew 能抬上限 */
    gpio_set_drive_strength(GB_PIN_LCD_SCK, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GB_PIN_LCD_MOSI, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(GB_PIN_LCD_SCK, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(GB_PIN_LCD_MOSI, GPIO_SLEW_RATE_FAST);

    s_bit_hz = GB_LCD_SPI_HZ;
    lcd_update_div();
    lcd_pio_configure(0);          /* 先按命令相位起来（8MHz） */
    dma_init_once();               /* 在 core0 上先把 DMA 通道占好，core1 直接用 */

    /* ---- ST7789 初始化 ---- *
     * 参数照 Pimoroni PicoSystem 的 hardware.cpp（同一块面板），
     * 只把 COLMOD 换成 0x55（16bpp RGB565）。 */
    lcd_cmd(0x01);                     /* SWRESET */
    sleep_ms(20);
    { uint8_t d[1] = { GB_LCD_MADCTL }; lcd_cmd_data(0x36, d, 1); } /* MADCTL */
    { uint8_t d[1] = { 0x00 }; lcd_cmd_data(0x35, d, 1); }           /* TEON */
    { uint8_t d[5] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
      lcd_cmd_data(0xB2, d, 5); }                                    /* FRMCTR2 */
    { uint8_t d[1] = { 0x55 }; lcd_cmd_data(0x3A, d, 1); }           /* COLMOD 16bpp */
    { uint8_t d[1] = { 0x01 }; lcd_cmd_data(0x26, d, 1); }           /* GAMSET */
    { uint8_t d[1] = { 0x14 }; lcd_cmd_data(0xB7, d, 1); }           /* GCTRL */
    { uint8_t d[1] = { 0x25 }; lcd_cmd_data(0xBB, d, 1); }           /* VCOMS */
    { uint8_t d[1] = { 0x2C }; lcd_cmd_data(0xC0, d, 1); }           /* LCMCTRL */
    { uint8_t d[1] = { 0x01 }; lcd_cmd_data(0xC2, d, 1); }           /* VDVVRHEN */
    { uint8_t d[1] = { 0x12 }; lcd_cmd_data(0xC3, d, 1); }           /* VRHS */
    { uint8_t d[1] = { 0x20 }; lcd_cmd_data(0xC4, d, 1); }           /* VDVS */
    { uint8_t d[2] = { 0xA4, 0xA1 }; lcd_cmd_data(0xD0, d, 2); }     /* PWRCTRL1 */
    { uint8_t d[1] = { GB_LCD_FRCTRL2 }; lcd_cmd_data(0xC6, d, 1); } /* FRCTRL2 */
    { uint8_t d[14] = { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                        0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 };
      lcd_cmd_data(0xE0, d, 14); }                                   /* GMCTRP1 */
    { uint8_t d[14] = { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                        0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 };
      lcd_cmd_data(0xE1, d, 14); }                                   /* GMCTRN1 */
    if(GB_LCD_INVERT)
        lcd_cmd(0x21);
    else
        lcd_cmd(0x20);
    sleep_ms(115);
    lcd_cmd(0x11);                     /* SLPOUT */
    sleep_ms(120);
    lcd_cmd(0x29);                     /* DISPON */
    sleep_ms(20);

    display_fill(0x0000);

#if GB_PIN_LCD_VSYNC >= 0
    te_probe();
#endif
}
