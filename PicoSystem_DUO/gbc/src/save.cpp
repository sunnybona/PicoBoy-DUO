/*
 * save.c — 把卡带 RAM 落到 flash 末尾的 32KB 区块。
 *
 * 注意两点：
 * 1) flash 擦写期间 XIP 不可用，所以必须关中断（audio 的 DMA 中断在 flash 里，
 *    不关会跑飞）；PWM 硬件继续输出，先把声音静音，避免出现杂音。
 * 2) 一次 32KB 擦除大约 100ms，这一帧会被"拉长"，主循环随后会重新对齐节拍。
 */
#include "save.h"
#include "board_config.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"

#define SAVE_BLOCK   32768u
#define SAVE_OFFSET  (PICO_FLASH_SIZE_BYTES - SAVE_BLOCK)
#define SAVE_MAGIC   0x56534247u   /* "GBSV" */

static uint8_t *s_ram;
static size_t   s_size;
static int      s_dirty;
static uint64_t s_last_ms;
static uint8_t  s_buf[SAVE_BLOCK];

void save_init(uint8_t *ram, size_t size)
{
    s_ram = ram;
    s_size = size;
    s_dirty = 0;
    s_last_ms = to_ms_since_boot(get_absolute_time());
}

static uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t s = 0x12345678u;
    for(size_t i = 0; i < n; i++)
        s = (s ^ p[i]) * 16777619u;
    return s;
}

void save_load(void)
{
    if(!s_ram || s_size == 0) return;

    const uint8_t *flash = (const uint8_t *)(XIP_BASE + SAVE_OFFSET);
    const uint32_t *hdr = (const uint32_t *)flash;
    /* magic, size, checksum */
    if(hdr[0] != SAVE_MAGIC) return;
    if(hdr[1] != (uint32_t)s_size) return;

    const uint8_t *payload = flash + 16;
    if(checksum(payload, s_size) != hdr[2]) return;

    for(size_t i = 0; i < s_size; i++)
        s_ram[i] = payload[i];
}

void save_mark_dirty(void)
{
    s_dirty = 1;
}

void save_flush(void)
{
#if GB_ENABLE_FLASH_SAVE
    if(!s_ram || s_size == 0 || !s_dirty) return;

    for(size_t i = 0; i < SAVE_BLOCK; i++)
        s_buf[i] = 0xFF;

    s_buf[0] = (uint8_t)(SAVE_MAGIC & 0xFF);
    s_buf[1] = (uint8_t)((SAVE_MAGIC >> 8) & 0xFF);
    s_buf[2] = (uint8_t)((SAVE_MAGIC >> 16) & 0xFF);
    s_buf[3] = (uint8_t)((SAVE_MAGIC >> 24) & 0xFF);
    s_buf[4] = (uint8_t)(s_size);
    s_buf[5] = (uint8_t)(s_size >> 8);
    s_buf[6] = (uint8_t)(s_size >> 16);
    s_buf[7] = (uint8_t)(s_size >> 24);
    for(size_t i = 0; i < s_size; i++)
        s_buf[16 + i] = s_ram[i];
    uint32_t sum = checksum(s_buf + 16, s_size);
    s_buf[8]  = (uint8_t)(sum);
    s_buf[9]  = (uint8_t)(sum >> 8);
    s_buf[10] = (uint8_t)(sum >> 16);
    s_buf[11] = (uint8_t)(sum >> 24);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(SAVE_OFFSET, SAVE_BLOCK);
    flash_range_program(SAVE_OFFSET, s_buf, SAVE_BLOCK);
    restore_interrupts(ints);

    s_dirty = 0;
    s_last_ms = to_ms_since_boot(get_absolute_time());
#endif
}

void save_poll(void)
{
#if GB_ENABLE_FLASH_SAVE
    if(!s_dirty) return;
    uint64_t now = to_ms_since_boot(get_absolute_time());
    if(now - s_last_ms >= 60000u)
        save_flush();
#endif
}
