#include <stdio.h>
#include <string.h>

#include "FrensHelpers.h"
#include <pico.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "InfoNES_System.h"

#include "hardware/watchdog.h"
#include "InfoNES.h"
#include "font_8x8.h"
#include "RomLister.h"
#include "gpbuttons.h"
#include "menu.h"

// hardware.hpp 头文件内含定义，包含会导致 _io 多重定义；这里只前置声明用到的接口
namespace picosystem {
void backlight(uint8_t b);
}

#define FONT_CHAR_WIDTH 16
#define FONT_CHAR_HEIGHT 16
#define FONT_N_CHARS 95
#define FONT_FIRST_ASCII 32
#define SCREEN_COLS 15
#define SCREEN_ROWS 15

#define STARTROW 1
#define ENDROW 7
#define PAGESIZE (ENDROW - STARTROW + 1)

#define VISIBLEPATHSIZE (SCREEN_COLS - 3)

/* 菜单像素色（格式 aaaarrrrggggbbbb，alpha 固定 15） */
#define MCR(r,g,b) ((uint16_t)((r) | 0x00F0 | ((b) << 8) | ((g) << 12)))
#define M_BLACK  MCR(0,0,0)
#define M_WHITE  MCR(15,15,15)
#define M_BAR    MCR(1,2,11)    /* 深蓝标题栏 */
#define M_BOX    MCR(2,2,2)     /* 未选中深灰 */
#define M_SEL    MCR(2,12,2)    /* 选中绿 */
#define M_DIM    MCR(8,8,8)     /* 灰色提示 */

/* 当前界面：卡片库 / 设置 / 通用单元格（splash、报错） */
enum { MV_CELL, MV_LIBRARY, MV_SETTINGS };
static int g_menuView = MV_CELL;
static int g_settingsSel = 0;
static char g_setLine[3][24];
static char g_pageInfo[8];

void screenMode(int incr);
extern const WORD __not_in_flash_func(NesPalette)[];

#define CBLACK 15
#define CWHITE 48
#define CRED 6
#define CGREEN 10
#define CBLUE 2
#define CLIGHTBLUE 0x2C
#define DEFAULT_FGCOLOR CBLACK // 60
#define DEFAULT_BGCOLOR CWHITE

static int fgcolor = DEFAULT_FGCOLOR;
static int bgcolor = DEFAULT_BGCOLOR;
DWORD PAD1_Latch_Menu, PAD1_Prev;
ROMSelector romSelector_;
struct charCell
{
    uint8_t fgcolor : 4;
    uint8_t bgcolor : 4;
    char charvalue;
};

#define SCREENBUFCELLS SCREEN_ROWS *SCREEN_COLS
static charCell *screenBuffer;

static WORD *WorkLineRom = nullptr;
void RomSelect_SetLineBuffer(WORD *p, WORD size)
{
    WorkLineRom = p;
}

static DWORD prevButtons{};

void RomSelect_PadState(DWORD *pdwPad1, bool ignorepushed = false)
{

    auto &dst = *pdwPad1;

    int v = getbuttons();

    *pdwPad1 = 0;

    unsigned long pushed;

    if (ignorepushed == false)
    {
        pushed = v & ~prevButtons;
    }
    else
    {
        pushed = v;
    }
    // if (p1 & SELECT)
    // {
    //     if (pushed & UP)
    //     {
    //         screenMode(-1);
    //         v = 0;
    //     }
    //     else if (pushed & DOWN)
    //     {
    //         screenMode(+1);
    //         v = 0;
    //     }
    // }
    if (pushed)
    {
        *pdwPad1 = v;
    }
    prevButtons = v;
}
/* ---- 像素级辅助：可见区 = WorkLineRom[8..247]，对应屏幕 x 0..239 ---- */
static void fill_vis(uint16_t *vis, int x0, int x1, uint16_t v)
{
    for (int x = x0; x < x1; x++)
        if (x >= 0 && x < 240) vis[x] = v;
}

/* 在 (x0,y0) 以 scale 倍渲染字符串（8x8 字体，9 列字距），仅画当前 scanline */
static void render_str(uint16_t *vis, int x0, int y0, const char *s,
                       int scale, uint16_t fg, uint16_t bg, int line)
{
    if (line < y0 || line >= y0 + 8 * scale) return;
    int fr = (line - y0) / scale;
    int gx = 0;
    for (int i = 0; s[i]; i++)
    {
        char c = s[i];
        uint8_t bits = 0;
        if (c >= 32 && c < 127)
            bits = (uint8_t)font_8x8[(c - 32) + fr * 95];
        for (int col = 0; col < 8; col++)
        {
            uint16_t v = (bits & (1 << col)) ? fg : bg;
            for (int sx = 0; sx < scale; sx++)
            {
                int px = x0 + gx + col * scale + sx;
                if (px >= 0 && px < 240) vis[px] = v;
            }
        }
        gx += 9 * scale;
    }
}

/* 读取逻辑单元格某行的字符（去掉尾部空格） */
static void cell_row_text(int logicalRow, char *out, int maxn)
{
    for (int k = 0; k < maxn; k++)
        out[k] = screenBuffer[logicalRow * SCREEN_COLS + k].charvalue;
    out[maxn] = 0;
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

void RomSelect_DrawLine(int line, int selectedRow)
{
    /* 隐藏左 margin 先填黑 */
    for (int i = 0; i < 8; ++i)
        WorkLineRom[i] = M_BLACK;
    uint16_t *vis = WorkLineRom + 8;

    if (g_menuView == MV_CELL)
    {
        /* 通用单元格：15 列 × 15 行，行高 16px、字距 9px */
        fill_vis(vis, 0, 240, M_BLACK);
        for (int r = 0; r < SCREEN_ROWS; r++)
        {
            if (line < r * 16 || line >= r * 16 + 8) continue;
            for (int k = 0; k < SCREEN_COLS; k++)
            {
                charCell *cell = &screenBuffer[r * SCREEN_COLS + k];
                char one[2] = {cell->charvalue, 0};
                render_str(vis, k * 9, r * 16, one, 1,
                          NesPalette[cell->fgcolor],
                          NesPalette[cell->bgcolor], line);
            }
        }
        return;
    }

    /* 两个卡片视图：黑底 + 深蓝标题栏 */
    fill_vis(vis, 0, 240, M_BLACK);
    if (line < 32) fill_vis(vis, 0, 240, M_BAR);

    if (g_menuView == MV_LIBRARY)
    {
        render_str(vis, 21, 8, "NES LIBRARY", 2, M_WHITE, M_BAR, line);
        render_str(vis, 196, 12, g_pageInfo, 1, M_WHITE, M_BAR, line);

        for (int i = 0; i < PAGESIZE; i++)
        {
            int by = 40 + i * 26;
            if (line < by || line >= by + 24) continue;
            int logicalRow = STARTROW + i;
            bool on = (logicalRow == selectedRow);
            uint16_t boxc = on ? M_SEL : M_BOX;
            fill_vis(vis, 8, 232, boxc);
            char txt[16];
            cell_row_text(logicalRow, txt, 15);
            render_str(vis, 18, by + 8, txt, 1,
                      on ? M_BLACK : M_WHITE, boxc, line);
        }
        render_str(vis, 66, 226, "A OK B EXIT Y", 1, M_DIM, M_BLACK, line);
    }
    else /* MV_SETTINGS */
    {
        render_str(vis, 48, 8, "SETTINGS", 2, M_WHITE, M_BAR, line);
        const int sy[3] = {48, 104, 160};
        for (int i = 0; i < 3; i++)
        {
            if (line < sy[i] || line >= sy[i] + 44) continue;
            bool on = (i == g_settingsSel);
            uint16_t boxc = on ? M_SEL : M_BOX;
            fill_vis(vis, 8, 232, boxc);
            int tw = (int)strlen(g_setLine[i]);
            int tx = (240 - tw * 9) / 2;
            render_str(vis, tx, sy[i] + 18, g_setLine[i], 1,
                      on ? M_BLACK : M_WHITE, boxc, line);
        }
        render_str(vis, 61, 226, "L/R CHG B BACK", 1, M_DIM, M_BLACK, line);
    }
}

void drawline(int scanline, int selectedRow)
{
    RomSelect_PreDrawLine(scanline);
    RomSelect_DrawLine(scanline, selectedRow);
    InfoNES_PostDrawLine(scanline, true);
}

static void putText(int x, int y, const char *text, int fgcolor, int bgcolor)
{

    if (text != nullptr)
    {
        auto index = y * SCREEN_COLS + x;
        auto maxLen = strlen(text);
        if (strlen(text) > SCREEN_COLS - x)
        {
            maxLen = SCREEN_COLS - x;
        }
        while (index < SCREENBUFCELLS && *text && maxLen > 0)
        {
            screenBuffer[index].charvalue = *text++;
            screenBuffer[index].fgcolor = fgcolor;
            screenBuffer[index].bgcolor = bgcolor;
            index++;
            maxLen--;
        }
    }
}

void DrawScreen(int selectedRow)
{
    for (auto line = 0; line < 240; line++)
    {
        drawline(line, selectedRow);
    }
}

void ClearScreen(charCell *screenBuffer, int color)
{
    for (auto i = 0; i < SCREENBUFCELLS; i++)
    {
        screenBuffer[i].bgcolor = color;
        screenBuffer[i].fgcolor = color;
        screenBuffer[i].charvalue = ' ';
    }
}

void displayRoms(Frens::RomLister romlister, int startIndex)
{
    char buffer[ROMLISTER_MAXPATH + 4];
    auto y = STARTROW;

    auto entries = romlister.GetEntries();
    ClearScreen(screenBuffer, bgcolor);

    g_menuView = MV_LIBRARY;
    int pageCount = (romlister.Count() + PAGESIZE - 1) / PAGESIZE;
    int currentPage = (startIndex / PAGESIZE) + 1;
    snprintf(g_pageInfo, sizeof(g_pageInfo), "P%d/%d", currentPage,
             pageCount > 0 ? pageCount : 1);

    for (auto index = startIndex; index < romlister.Count(); index++)
    {
        if (y <= ENDROW)
        {
            auto info = entries[index];
            if (info.IsDirectory)
                snprintf(buffer, sizeof(buffer), "[D] %s", info.Path);
            else
                snprintf(buffer, sizeof(buffer), "%s", info.Path);
            putText(0, y, buffer, fgcolor, bgcolor);
            y++;
        }
    }
}

// 由 main.cpp 提供的全局设置
extern int volume;            // 0-100
extern uint8_t backlight_value; // 0-100
extern bool g_invert;         // 屏幕反色开关

// 设置菜单：亮度 / 反色 / 音量
void settings_menu()
{
    const int SETTINGS_ROWS = 3;
    int row = 0;
    g_menuView = MV_SETTINGS;
    g_settingsSel = 0;

    auto refresh_lines = [&]()
    {
        snprintf(g_setLine[0], sizeof(g_setLine[0]), "BRIGHT %d", backlight_value);
        snprintf(g_setLine[1], sizeof(g_setLine[1]), "INVERT %s", g_invert ? "ON" : "OFF");
        snprintf(g_setLine[2], sizeof(g_setLine[2]), "VOLUME %d", volume);
    };
    refresh_lines();

    while (true)
    {
        InfoNES_LoadFrame();
        DrawScreen(-1);

        DWORD padcheck = prevButtons;
        RomSelect_PadState(&PAD1_Latch_Menu);

        if (PAD1_Latch_Menu > 0)
        {
            if ((PAD1_Latch_Menu & GPUP) && !(padcheck & GPUP))
            {
                row = (row - 1 < 0) ? SETTINGS_ROWS - 1 : row - 1;
                g_settingsSel = row;
            }
            else if ((PAD1_Latch_Menu & GPDOWN) && !(padcheck & GPDOWN))
            {
                row = (row + 1 >= SETTINGS_ROWS) ? 0 : row + 1;
                g_settingsSel = row;
            }
            else if ((PAD1_Latch_Menu & GPB) && !(padcheck & GPB))
            {
                persist_settings(); // 退出前保存设置到 flash
                return;
            }
            else if ((PAD1_Latch_Menu & GPLEFT) && !(padcheck & GPLEFT))
            {
                if (row == 0)
                {
                    backlight_value = (backlight_value <= 10) ? 0 : backlight_value - 10;
                    picosystem::backlight(backlight_value);
                }
                else if (row == 2)
                {
                    volume = (volume <= 5) ? 0 : volume - 5;
                }
                else
                {
                    g_invert = false;
                }
                refresh_lines();
            }
            else if ((PAD1_Latch_Menu & GPRIGHT) && !(padcheck & GPRIGHT))
            {
                if (row == 0)
                {
                    backlight_value = (backlight_value >= 90) ? 100 : backlight_value + 10;
                    picosystem::backlight(backlight_value);
                }
                else if (row == 2)
                {
                    volume = (volume >= 95) ? 100 : volume + 5;
                }
                else
                {
                    g_invert = true;
                }
                refresh_lines();
            }
        }
    }
}

void DisplayFatalError(char *error)
{
    g_menuView = MV_CELL;
    ClearScreen(screenBuffer, bgcolor);
    putText(1, 0, "FATAL ERROR", fgcolor, bgcolor);
    putText(1, 3, error, fgcolor, bgcolor);
    while (true)
    {
        auto frameCount = InfoNES_LoadFrame();
        DrawScreen(-1);
    }
}

void DisplayEmulatorErrorMessage(char *error)
{
    //DWORD PAD1_Latch;
    g_menuView = MV_CELL;
    ClearScreen(screenBuffer, bgcolor);
    putText(1, 0, "ERROR OCCURED", fgcolor, bgcolor);
    putText(1, 3, error, fgcolor, bgcolor);
    putText(1, ENDROW, "PRESS ANY KEY", fgcolor, bgcolor);
    while (true)
    {
        auto frameCount = InfoNES_LoadFrame();
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch);
        if (PAD1_Latch_Menu> 0)
        {
            return;
        }
    }
}

void showSplashScreen()
{
    //DWORD PAD1_Latch;
    char s[SCREEN_COLS];
    g_menuView = MV_CELL;
    ClearScreen(screenBuffer, bgcolor);

    auto centered = [&](int y, const char *txt, int col = fgcolor)
    {
        int n = (int)strlen(txt);
        if (n > SCREEN_COLS - 1) n = SCREEN_COLS - 1;
        putText(1 + ((SCREEN_COLS - 1 - n) / 2), y, txt, col, bgcolor);
    };

    centered(1, "PICOSYSTEM");
    putText(4, 2, "INFO", CRED, bgcolor);
    putText(9, 2, "NES", CGREEN, bgcolor);
    centered(4, "EMULATOR");
    centered(5, "JAY KUMOGATA", CLIGHTBLUE);
    centered(7, "PICO PORT");
    centered(8, "SHUICHI TAKANO", CLIGHTBLUE);
    centered(10, "F. HOEDEMAKERS");
    strncpy(s, PICO_PROGRAM_VERSION_STRING, SCREEN_COLS - 1);
    s[SCREEN_COLS - 1] = 0;
    centered(12, s, CLIGHTBLUE);
    centered(13, "PRESS ANY KEY");

    int startFrame = -1;
    while (true)
    {
        auto frameCount = InfoNES_LoadFrame();
        if (startFrame == -1)
        {
            startFrame = frameCount;
        }
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch_Menu);
        if (PAD1_Latch_Menu> 0 || (frameCount - startFrame) > 500)
        {
            return;
        }
        if ((frameCount % 30) == 0)
        {
            for (auto i = 0; i < SCREEN_COLS; i++)
            {
                auto col = rand() % 63;
                putText(i, 0, " ", col, col);
                col = rand() % 63;
                putText(i, SCREEN_ROWS - 1, " ", col, col);
            }
            for (auto i = 1; i < SCREEN_ROWS - 1; i++)
            {
                auto col = rand() % 63;
                putText(1, i, " ", col, col);
                col = rand() % 63;
                putText(SCREEN_COLS - 1, i, " ", col, col);
            }
        }
    }
}

void screenSaver()
{
    //DWORD PAD1_Latch;
    WORD frameCount;
    while (true)
    {
        frameCount = InfoNES_LoadFrame();
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch_Menu);
        if (PAD1_Latch_Menu> 0)
        {
            return;
        }
        if ((frameCount % 3) == 0)
        {
            auto color = rand() % 63;
            auto row = rand() % SCREEN_ROWS;
            auto column = rand() % SCREEN_COLS;
            putText(column, row, " ", color, color);
        }
    }
}
// Global instances of local vars in romselect() some used in Lambda expression later on
static char *selectedRomOrFolder;
static bool errorInSavingRom = false;
static char *globalErrorMessage;
static uintptr_t FLASH_ADDRESS;
static bool showSplash = true;
 
int menu(uintptr_t NES_FILE_ADDR, char *errorMessage, bool nosplash)
{

    FLASH_ADDRESS = NES_FILE_ADDR;
    int firstVisibleRowINDEX = 0;
    int selectedRow = STARTROW;

    int totalFrames = -1;

    globalErrorMessage = errorMessage;

   

    int horzontalScrollIndex = 0;
    printf("Starting Menu\n");
    size_t ramsize;
    // Borrow Emulator RAM buffer for screen.
    screenBuffer = (charCell *)InfoNes_GetRAM(&ramsize);
    size_t chr_size;
    // Borrow ChrBuffer to store directory contents
    void *buffer = InfoNes_GetChrBuf(&chr_size);
    Frens::RomLister romlister(NES_FILE_ADDR, buffer, chr_size);
    romlister.list();
    if (romlister.Count() == 0)
    {
        return -1;
    }
    if (strlen(errorMessage) > 0)
    {

        DisplayEmulatorErrorMessage(errorMessage); // Emulator cannot start, show error

        showSplash = false;
    }
    if (showSplash)
    {
        showSplash = false;
        if ( nosplash == false ) {
            showSplashScreen();
        }
    }
    Frens::RomLister::RomEntry *entries;
    displayRoms(romlister, firstVisibleRowINDEX);
    int index = -1;

    while (1)
    {
        
        auto frameCount = InfoNES_LoadFrame();
        index = selectedRow - STARTROW + firstVisibleRowINDEX;
        entries = romlister.GetEntries();
        selectedRomOrFolder = (romlister.Count() > 0) ? entries[index].Path : nullptr;
        errorInSavingRom = false;
        DrawScreen(selectedRow);
        DWORD padcheck = prevButtons; // is reset in romselect. 
        
        RomSelect_PadState(&PAD1_Latch_Menu);
       // if (PAD1_Latch_Menu == padcheck) // no change
         //   continue;
       

        if (PAD1_Latch_Menu> 0)
        {
            totalFrames = frameCount; // Reset screenSaver
            // reset horizontal scroll of highlighted row
            horzontalScrollIndex = 0;
            putText(0, selectedRow, selectedRomOrFolder, fgcolor, bgcolor);
            if (selectedRomOrFolder)
            {

            }
            if ((PAD1_Latch_Menu& GPUP) && !(padcheck & GPUP))
            {
                if (selectedRow > STARTROW)
                {
                    selectedRow--;
                }
                else
                {
                    if (firstVisibleRowINDEX > 0)
                    {
                        firstVisibleRowINDEX--;
                        displayRoms(romlister, firstVisibleRowINDEX);
                    }
                }
            }
            else if ((PAD1_Latch_Menu& GPDOWN) && !(padcheck & GPDOWN))
            {
                if (selectedRow < ENDROW && (index) < romlister.Count() - 1)
                {
                    selectedRow++;
                }
                else
                {
                    if (index < romlister.Count() - 1)
                    {
                        firstVisibleRowINDEX++;
                        displayRoms(romlister, firstVisibleRowINDEX);
                    }
                }
            }
            if ((PAD1_Latch_Menu& GPLEFT) && !(padcheck & GPLEFT))
            {
                firstVisibleRowINDEX -= PAGESIZE;
                if (firstVisibleRowINDEX < 0)
                {
                    int index = romlister.Count() / (float)PAGESIZE;
                    int lastpage = romlister.Count() - (index * PAGESIZE);
                    if (lastpage > 0)
                    {
                        firstVisibleRowINDEX = romlister.Count() - lastpage - 1;
                    }
                    else
                    {

                        firstVisibleRowINDEX = romlister.Count() - (PAGESIZE );//Wrap attempt
                    }
                }
                selectedRow = STARTROW;
                displayRoms(romlister, firstVisibleRowINDEX);
            }
            else if ((PAD1_Latch_Menu& GPRIGHT) && !(padcheck & GPRIGHT))
            {
                if (firstVisibleRowINDEX + PAGESIZE < romlister.Count())
                {
                    firstVisibleRowINDEX += PAGESIZE;
                }
                else
                {
                    firstVisibleRowINDEX = 0;
                }
                selectedRow = STARTROW;
                displayRoms(romlister, firstVisibleRowINDEX);
            }
            if ((PAD1_Latch_Menu& GPB) && !(padcheck & GPB))
            {
                // start emulator with currently loaded game
                index = -1;
                break;
            }
            if ((PAD1_Latch_Menu& GPY) && !(padcheck & GPY))
            {
                // 打开设置菜单（亮度/反色/音量）
                settings_menu();
                displayRoms(romlister, firstVisibleRowINDEX);
            }
            // else if ((PAD1_Latch_Menu& GPY) == GPY && (PAD1_Latch_Menu& GPX) != GPX)
            // {
            //     // start emulator with currently loaded game
            //     index = -1;
            //     break;
            // }
            if ((PAD1_Latch_Menu& GPA) && !(padcheck & GPA)) //prevent button hold from micro menu exit. 
            {
                break;

                // if (entries[index].IsDirectory)
                // {
                //     romlister.list(selectedRomOrFolder);
                //     firstVisibleRowINDEX = 0;
                //     selectedRow = STARTROW;
                //     displayRoms(romlister, firstVisibleRowINDEX);
                // }
            }
        }
        // scroll selected row horizontally if textsize exceeds rowlength
        if (selectedRomOrFolder)
        {
            if ((frameCount % 30) == 0)
            {
                if (strlen(selectedRomOrFolder + horzontalScrollIndex) > VISIBLEPATHSIZE)
                {
                    horzontalScrollIndex++;
                }
                else
                {
                    horzontalScrollIndex = 0;
                }
                putText(0, selectedRow, selectedRomOrFolder + horzontalScrollIndex, fgcolor, bgcolor);
            }
        }
        if (totalFrames == -1)
        {
            totalFrames = frameCount;
        }
        if ((frameCount - totalFrames) > 1200)
        {
            printf("Starting screensaver\n");
            totalFrames = -1;
            screenSaver();
            displayRoms(romlister, firstVisibleRowINDEX);
        }
    }
    // Wait until user has released all buttons
    //what is the purpose?
    /*
    while (1)
    {
        InfoNES_LoadFrame();
        DrawScreen(-1);
        RomSelect_PadState(&PAD1_Latch_Menu, true);
        if (PAD1_Latch_Menu== 0)
        {
            break;
        }
    }*/
    InfoNES_LoadFrame();
    DrawScreen(-1);
    
    if ( index != -1) {
        index = entries[index].Index;
    }
    return index;
}
