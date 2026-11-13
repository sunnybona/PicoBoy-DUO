#ifndef SAVE_H
#define SAVE_H

#include <stddef.h>
#include <stdint.h>

/* 卡带 RAM 存档：放在 flash 末尾的 32KB 区块。
 * 只有 MBС 带电池存档的卡带（gb.cart_ram != 0）才需要。 */
void save_init(uint8_t *ram, size_t size);
void save_load(void);
void save_mark_dirty(void);
void save_flush(void);

/* 主循环里每帧调一次：脏数据且超过 20s 就落盘。 */
void save_poll(void);

#endif
