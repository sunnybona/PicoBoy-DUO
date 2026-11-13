/*
 * host_test_gbc.c 鈥斺€?鍦?PC 涓婃妸 Walnut-CGB 璺戜竴閬嶏紝楠岃瘉鍥轰欢閲岄偅濂楁帴绾垮涓嶅锛? *   - gb_init 鐨勪笁妗?ROM 鍥炶皟锛?/16/32 浣嶅彇鎸囷級
 *   - CGB 璋冭壊鏉跨储寮?-> fixPalette 鏌ヨ〃锛堝墠绔?lcd_draw_line 鐨勭湡瀹炲垎鏀級
 *   - DMG 12 鑹诧紙鑹查樁 + 鍥惧眰锛? *   - 姣忓抚 update_pad锛氬啓 direct.joypad銆侀噸绠?P1 涓ゅ崐銆佹寜浣忔椂琛?IF bit4
 * 杈撳嚭 frame_*.ppm锛?60脳144 RGB锛夛紝鍐嶇敱 ppm2png.py 杞?PNG銆? *
 * 缂栵細gcc -O2 -std=c11 -o host_test_gbc.exe host_test_gbc.c
 * 璺戯細host_test_gbc.exe <rom> <out_prefix>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define WALNUT_FULL_GBC_SUPPORT 1
#define WALNUT_GB_12_COLOUR 1
#define WALNUT_GB_32BIT_DMA 1
#define WALNUT_GB_32BIT_ALIGNED 1

/* 澶存枃浠跺紑澶寸殑鍑芥暟鍘熷瀷鐢ㄥ埌浜?struct gb_s锛屼絾瀹氫箟鍦ㄥ悗闈細
 * C 閲屽繀椤诲厛缁欎釜鍓嶇疆澹版槑锛屽惁鍒欐瘡涓師鍨嬮兘浼?鍙戞槑"涓€涓柊鐨勫眬閮ㄧ被鍨嬨€?*/
struct gb_s;

#include "walnut_cgb.h"

#define IO_IDX_JOYP 0x00
#define IO_IDX_IF   0x0F
#define IO_IDX_IE   0xFF

static uint8_t *g_rom;
static size_t   g_rom_size;
static uint8_t  g_ram[0x8000];
static uint16_t g_pal12[12];
static uint8_t  g_fb[144][160][3];

/* ---- 鍗″甫鍥炶皟 ---- */
static uint8_t rd8(struct gb_s *gb, const uint_fast32_t a)
{ (void)gb; return (a < g_rom_size) ? g_rom[a] : 0xFF; }

static uint16_t rd16(struct gb_s *gb, const uint_fast32_t a)
{ (void)gb;
  uint8_t b0 = (a < g_rom_size) ? g_rom[a] : 0xFF;
  uint8_t b1 = (a + 1 < g_rom_size) ? g_rom[a + 1] : 0xFF;
  return (uint16_t)(b0 | (b1 << 8)); }

static uint32_t rd32(struct gb_s *gb, const uint_fast32_t a)
{ (void)gb; uint32_t v = 0;
  for(int i = 0; i < 4; i++)
      v |= (uint32_t)((a + i < g_rom_size) ? g_rom[a + i] : 0xFF) << (8 * i);
  return v; }

static uint8_t rram(struct gb_s *gb, const uint_fast32_t a)
{ (void)gb; return (a < sizeof(g_ram)) ? g_ram[a] : 0xFF; }

static void wram_cb(struct gb_s *gb, const uint_fast32_t a, const uint8_t v)
{ (void)gb; if(a < sizeof(g_ram)) g_ram[a] = v; }

static void err_cb(struct gb_s *gb, const enum gb_error_e e, const uint16_t a)
{ (void)gb; printf("!! gb error %d at 0x%04X\n", (int)e, a); }

/* ---- 璋冭壊鏉匡細DMG 12 鑹?---- */
static uint16_t rgb565(int r, int g, int b)
{ return (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255)); }

static void init12(void)
{
    g_pal12[0] = rgb565(255,255,255); g_pal12[1] = rgb565(176,176,176);
    g_pal12[2] = rgb565( 88, 88, 88); g_pal12[3] = rgb565(  0,  0,  0);
    g_pal12[4] = rgb565(255,200,200); g_pal12[5] = rgb565(200,120,120);
    g_pal12[6] = rgb565(140, 60, 60); g_pal12[7] = rgb565( 60, 20, 20);
    g_pal12[8] = rgb565(155,188, 15); g_pal12[9] = rgb565(139,172, 15);
    g_pal12[10]= rgb565( 48, 98, 48); g_pal12[11]= rgb565( 15, 56, 15);
}

static void lcd_cb(struct gb_s *gb, const uint8_t *px, const uint_fast8_t line)
{
    if(line >= 144) return;
    for(int i = 0; i < 160; i++)
    {
        uint16_t c;
        if(gb->cgb.cgbMode)
            c = gb->cgb.fixPalette[px[i] & 0x3F];
        else
        {
            uint8_t layer = (px[i] >> 4) & 3, shade = px[i] & 3;
            c = (layer == 2) ? g_pal12[8 + shade]
              : (layer == 1) ? g_pal12[4 + shade] : g_pal12[shade];
        }
        g_fb[line][i][0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        g_fb[line][i][1] = (uint8_t)(((c >>  5) & 0x3F) * 255 / 63);
        g_fb[line][i][2] = (uint8_t)(( c        & 0x1F) * 255 / 31);
    }
}

/* ---- 姣忓抚 update_pad锛堝拰鍥轰欢閲屽畬鍏ㄥ悓涓€濂楅€昏緫锛?---- */
static void update_pad(struct gb_s *gb, uint8_t pad)
{
    gb->direct.joypad = (uint8_t)~pad;

    uint8_t sel = gb->hram_io[IO_IDX_JOYP] & 0x30;
    uint8_t lo = 0x0F;
    if(!(sel & 0x10)) lo &= (uint8_t)~(pad >> 4);
    if(!(sel & 0x20)) lo &= (uint8_t)~(pad & 0x0F);
    gb->hram_io[IO_IDX_JOYP] = (uint8_t)(0xC0 | sel | lo);

    if(pad && (gb->hram_io[IO_IDX_IE] & 0x10))
        gb->hram_io[IO_IDX_IF] |= 0x10;
}

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if(!f) { printf("cannot write %s\n", path); return; }
    fprintf(f, "P6\n160 144\n255\n");
    fwrite(g_fb, 1, sizeof(g_fb), f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if(argc < 3) { printf("usage: host_test_gbc <rom> <out_prefix> [frames]\n"); return 1; }
    int total = (argc > 3) ? atoi(argv[3]) : 160;
    if(total < 2) total = 2;

    FILE *f = fopen(argv[1], "rb");
    if(!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); g_rom_size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_rom = malloc(g_rom_size);
    fread(g_rom, 1, g_rom_size, f);
    fclose(f);

    init12();

    struct gb_s gb;
    memset(&gb, 0, sizeof(gb));
    enum gb_init_error_e ie = gb_init(&gb, rd8, rd16, rd32,
                                      rram, wram_cb, err_cb, NULL);
    if(ie != GB_INIT_NO_ERROR) { printf("gb_init failed: %d\n", (int)ie); return 1; }
    gb_init_lcd(&gb, lcd_cb);

    char title[32] = {0};
    const char *name = gb_get_rom_name(&gb, title);
    printf("rom=%s size=%zu name=%s mode=%s mbc=%d cart_ram=%d banks_mask=%u\n",
           argv[1], g_rom_size, name ? name : "?",
           gb.cgb.cgbMode ? "CGB" : "DMG",
           (int)gb.mbc, (int)gb.cart_ram,
           (unsigned)gb.num_rom_banks_mask);

    /* 60 甯ч潤缃?-> 鎸?START 涓ゅ抚 -> 鍐嶈窇 90 甯?*/
    uint8_t pad = 0;
    for(int frame = 0; frame < total; frame++)
    {
        if(frame == total / 3) pad = 0x08;          /* START锛屼綅瀹氫箟瑙?JOYPAD_START */
        if(frame == total / 3 + 2) pad = 0;
        update_pad(&gb, pad);
        gb_run_frame_dualfetch(&gb);

        if(frame == total / 3 - 1 || frame == total - 1)
        {
            char path[512];
            snprintf(path, sizeof(path), "%s_%s.ppm", argv[2],
                     frame == total / 3 - 1 ? "idle" : "end");
            dump_ppm(path);
            printf("dumped %s\n", path);
        }
    }
    printf("OK frames=%d\\n", total);
    return 0;
}
