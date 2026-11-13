/*
 * board_config.h — RP2040 + ST7789（无 TE/VSYNC 引脚）掌机硬件配置
 *
 * 所有引脚都可以在编译时用 -D 覆盖，例如：
 *   cmake .. -DGB_PIN_LCD_SCK=6 -DGB_PIN_LCD_MOSI=7 ...
 * 也可以直接在下面改默认值。
 *
 * 注意：本板没有 TE(VSYNC) 引脚，显示同步完全走"时间轴 + 整帧连续写入"，
 * 详见 display.c 顶部注释与 README。
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* ------------------------------------------------------------------ */
/* 主频：RP2040 官方 133MHz，跑 GB 需要更高主频。                        */
/* 250MHz 是 PicoSystem 一类的出厂值；如果板子供电/散热一般，改成 200000。 */
/* 改主频后音频 PWM 分频会自动按 clock_get_hz() 重算，不用手工改。      */
/* ------------------------------------------------------------------ */
#ifndef GB_SYS_CLOCK_KHZ
#define GB_SYS_CLOCK_KHZ 250000
#endif

/* ------------------------------------------------------------------ */
/* 屏幕：ST7789 240x240，4 线 SPI（SCK/MOSI/CS/DC + 可选 RST/BL）        */
/* ------------------------------------------------------------------ */
#ifndef GB_LCD_SPI          /* 0 = spi0, 1 = spi1 */
#define GB_LCD_SPI 0
#endif
#ifndef GB_PIN_LCD_SCK
#define GB_PIN_LCD_SCK 6
#endif
#ifndef GB_PIN_LCD_MOSI
#define GB_PIN_LCD_MOSI 7
#endif
#ifndef GB_PIN_LCD_CS
#define GB_PIN_LCD_CS 5
#endif
#ifndef GB_PIN_LCD_DC
#define GB_PIN_LCD_DC 9
#endif
#ifndef GB_PIN_LCD_RST
#define GB_PIN_LCD_RST 4       /* 没有 RST 时定义成 -1，改用软件复位 */
#endif
#ifndef GB_PIN_LCD_BL
#define GB_PIN_LCD_BL 12      /* 背光，-1 表示没有（或一直点亮） */
#endif
#ifndef GB_PIN_LCD_BL_ACTIVE_HIGH
#define GB_PIN_LCD_BL_ACTIVE_HIGH 1
#endif

/* 面板 TE(VSYNC) 输出脚：本板**没有**（用户已确认）。
 * 代码里保留探测/门控逻辑，接上 TE 的板子把这里改成对应 GPIO 即可自动启用；
 * -1 = 不探测，纯时间轴同步。 */
#ifndef GB_PIN_LCD_VSYNC
#define GB_PIN_LCD_VSYNC -1
#endif

/* ST7789 规格书 SPI 写周期 16ns -> 62.5MHz 是安全上限。
 * 飞线/长排线降到 40MHz 或 31.25MHz 更稳。 */
#ifndef GB_LCD_SPI_HZ
#define GB_LCD_SPI_HZ 62500000
#endif

/* 面板方向：0x00=正常，0x60=横屏旋转，0xC0=上下翻，0xA0=左右翻。
 * 颜色发反（黑变白）时把 GB_LCD_INVERT 改成 0，反之改 1。 */
#ifndef GB_LCD_MADCTL
#define GB_LCD_MADCTL 0x00
#endif
#ifndef GB_LCD_INVERT
#define GB_LCD_INVERT 1
#endif
/* 面板刷新率寄存器 0xC6（ST7789 FRCTRL2）。0x1E 是原厂 PicoSystem 用的值；
 * 无 TE 时把它调到和 GB 的 59.73Hz 接近可减少撕裂（0x0C~0x1E 之间可试）。 */
#ifndef GB_LCD_FRCTRL2
#define GB_LCD_FRCTRL2 0x1E
#endif

#ifndef GB_SCREEN_W
#define GB_SCREEN_W 240
#endif
#ifndef GB_SCREEN_H
#define GB_SCREEN_H 240
#endif

/* 显示缩放： 1 = 160x144 原尺寸居中（SPI 只推 46KB/帧，最省带宽、最稳）
 *           2 = 1.5 倍最近邻铺满宽度，240x216（103KB/帧，画面大但吃 SPI 带宽）
 * 默认 1.5 倍。若实测掉帧（串口打印 fps < 59），改成 1。 */
#ifndef GB_DISPLAY_SCALE15
#define GB_DISPLAY_SCALE15 1
#endif

/* ------------------------------------------------------------------ */
/* 音频：单 GPIO + PWM 当 DAC。                                           */
/* ------------------------------------------------------------------ */
#ifndef GB_PIN_AUDIO
#define GB_PIN_AUDIO 11
#endif
#ifndef GB_AUDIO_ENABLE
#define GB_AUDIO_ENABLE 1
#endif
/* PWM 位宽：wrap = (1<<bits)-1。12 位 + 主频 250MHz -> 载波 61.0kHz，
 * 正好拿来做 61kHz 采样率（见 audio_config.h）。 */
#ifndef GB_AUDIO_PWM_BITS
#define GB_AUDIO_PWM_BITS 12
#endif
/* 音频环形缓冲（样本数，必须是 2 的幂）。2048 ≈ 33ms。 */
#ifndef GB_AUDIO_RING
#define GB_AUDIO_RING 2048
#endif

/* ------------------------------------------------------------------ */
/* 按键：全部低有效 + 内部上拉。没有的键填 -1。                          */
/* GB 映射： A->A键, B->B键, START->X, SELECT->Y                      */
/* ------------------------------------------------------------------ */
#ifndef GB_PIN_BTN_A
#define GB_PIN_BTN_A 18
#endif
#ifndef GB_PIN_BTN_B
#define GB_PIN_BTN_B 19
#endif
#ifndef GB_PIN_BTN_X
#define GB_PIN_BTN_X 17
#endif
#ifndef GB_PIN_BTN_Y
#define GB_PIN_BTN_Y 16
#endif
#ifndef GB_PIN_BTN_UP
#define GB_PIN_BTN_UP 23
#endif
#ifndef GB_PIN_BTN_DOWN
#define GB_PIN_BTN_DOWN 20
#endif
#ifndef GB_PIN_BTN_LEFT
#define GB_PIN_BTN_LEFT 22
#endif
#ifndef GB_PIN_BTN_RIGHT
#define GB_PIN_BTN_RIGHT 21
#endif

/* ------------------------------------------------------------------ */
/* 存档：把卡带 RAM 写进 flash 尾部。                                    */
/* ------------------------------------------------------------------ */
#ifndef GB_ENABLE_FLASH_SAVE
#define GB_ENABLE_FLASH_SAVE 1
#endif

#endif /* BOARD_CONFIG_H */
