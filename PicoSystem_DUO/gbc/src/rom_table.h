#ifndef ROM_TABLE_H
#define ROM_TABLE_H

#include <stdint.h>

/* 内嵌 ROM 表：构建期由 tools/make_roms_c.py 依 CMake 的清单生成。
 * 数组本身放在 flash，peanut_gb 通过 XIP 直接读，不占 RAM。 */
typedef struct {
    const char     *name;   /* 屏/串口上显示的名字（大写，3x5 字体） */
    const uint8_t  *data;
    uint32_t        size;
} gb_rom_entry_t;

extern const gb_rom_entry_t gb_roms[];
extern const unsigned int   gb_rom_count;

#endif /* ROM_TABLE_H */
