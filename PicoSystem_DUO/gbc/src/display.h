#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "board_config.h"

/* 初始化 SPI + ST7789 面板（清屏为黑）。 */
void display_init(void);

/* 打开一个矩形窗口并把 CS 拉低进入持续写显存状态。
 * GB 帧和文字界面都走这一套。 */
void display_stream_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* 取一行可写的像素缓冲（GB_SCREEN_W 个 RGB565 值，原生字节序，不用自己拼字节），
 * 填好后调用 display_kick_row()。内部双缓冲 + DMA，调用方不必关心等待。 */
uint16_t *display_next_row(void);
void      display_kick_row(uint16_t w);

/* 结束一次 stream：等最后一行发完，拉高 CS。 */
void display_stream_end(void);

/* 整屏填一个 RGB565 颜色。 */
void display_fill(uint16_t rgb565);

/* 矩形填充（自动裁剪、宽度凑偶），用于选中高亮条。 */
void display_rect(int x, int y, int w, int h, uint16_t rgb565);

/* 只把 GB 显示区（GB_FRAME_X/Y 起 160×144 或 240×216）刷成纯色。
 * 开始推第一帧之前调一次（黑），清掉面板 GRAM 里的残留内容。 */
void display_clear_gb_area(uint16_t rgb565);

/* 开始一帧 GB 画面。内部只是清空离屏帧缓冲的行计数（不碰 SPI）。 */
void display_begin_gb_frame(void);

/* 一帧模拟跑完后调用：把离屏帧缓冲整帧连续推给面板。
 * 之所以先攒满再推，是要让"推流"这段波形和测试页完全一致——
 * 边模拟边推时 CPU 的 XIP 停顿会插进 SPI 流中间，高速下会丢字节。 */
void display_end_gb_frame(void);

/* 启动 core1 的"推屏线程"。必须在 display_init()（和上电自检画面）之后调用。
 * 之后 display_end_gb_frame() 只负责交付，真正的 SPI 写全在 core1 上跑。 */
void display_start_pusher(void);

/* 等 core1 把当前这一帧推完。core0 上要直接画屏时（提示文字/整屏填色）先调这个，
 * 否则会和 core1 抢 SPI。 */
void display_wait_idle(void);

/* 上一帧在 core1 上推屏花了多少微秒（8MHz + 160×144 大约 46ms 左右）。 */
uint32_t display_push_us(void);

/* 上一帧实际写进帧缓冲的行数（正常 = 216 或 144），调试用。 */
int  display_gb_rows(void);

/* 上一帧真正推给面板的行数（开了"只推变化行"时会小于行总数）。 */
int  display_pushed_rows(void);

/* 在 (x,y) 画 3x5 点阵字符串，scale 为放大倍数（建议 4~8）。
 * 返回占用的宽度。 */
int display_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/* 在 (x,y) 画 8x8 大字体字符串，scale 为放大倍数（1=8px，2=16px）。
 * 返回占用的宽度。 */
int display_text8(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

/* 居中显示 1~3 行文字（黑底），用于无 ROM / 出错提示。 */
void display_message(const char *l1, const char *l2, const char *l3);

/* 上电自检图：四色横向彩条（白/红/绿/蓝）+ 6px 黑边框 + 中间黑带上的标题。
 * 刷完看不到这屏，问题就一定在面板/引脚/SPI，而不是模拟器没画东西。 */
void display_splash(const char *txt);

/* 面板 TE 是否可用（GB_PIN_LCD_VSYNC = -1 时恒为 0）。可用时每帧会等 TE 再开写。 */
int  display_has_te(void);

/* 排查用：运行期切换推流方式，用来把"SPI 模式 / DMA / 时钟"三个变量分开验证。
 * display_set_blocking(1)  -> 不用 DMA，直接 spi_write_blocking（慢，但绝对不丢字节）
 * display_set_spi_mode3(1) -> SPI mode3(CPOL=1,CPHA=1)，默认 mode0
 * display_set_spi_hz(x)    -> 改 SPI 时钟 */
void display_set_blocking(int on);
void display_set_spi_mode3(int on);
void display_set_spi_hz(unsigned int hz);

/* RGB565 颜色与字节序辅助 */
static inline uint16_t display_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t display_swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

/* 把 GB 的一行 160 像素推进屏幕（peanut_gb / walnut 的 lcd_draw_line 用它）。
 * pixels[i] 低 2 位 = 0..3 号色阶。 */
void display_push_gb_line(const uint8_t *pixels, uint8_t line);

/* 颜色已算好的行（RGB565，160 像素，原生字节序）：CGB 模式用这个。 */
void display_push_rgb_line(const uint16_t *rgb, uint8_t line);

/* 换 DMG 的 4 级色表（默认 DMG 绿）。 */
void display_set_dmg_palette(const uint16_t pal4[4]);

#endif /* DISPLAY_H */
