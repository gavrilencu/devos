#include "gui.h"
#include "fb.h"
#include "vga.h"
#include "pit.h"
#include "pmm.h"
#include "fs.h"
#include "task.h"
#include "io.h"
#include "string.h"
#include "browser.h"

#define TB_H 54                 /* inaltimea taskbar-ului (stil Win11) */

/* Paleta in stil Windows 11 (Fluent, dark): accent albastru, suprafete
 * "Mica" gri-albastrui, colturi rotunjite generos. */
#define COL_SPLASH_BG 0x0A0E17
#define COL_ACCENT    0x1284E4  /* accentul Win11 */
#define COL_ACCENT_HI 0x54B4FF
#define COL_TASKBAR   0x1B1D22
#define COL_TASKBAR_2 0x2A2D34
#define COL_TAB       0x2A2E36
#define COL_TAB_HOV   0x363B45
#define COL_TITLE_F   0x2C313B  /* titlul ferestrei focusate */
#define COL_TITLE_U   0x23272E  /* nefocusata */
#define COL_TEXT      0xF2F4F8
#define COL_DIM       0xA7B0BE
#define COL_MENU      0x24272E
#define COL_CARD      0x2C313B  /* butoane/carduri */
#define COL_CARD_HOV  0x39404B

/* geometria ferestrelor: continut de baza 640x400 (80x25 celule de 8x16).
 * Ferestrele pot fi maximizate: dimensiunea reala e per-fereastra (cwv/chv).
 * In functiile de continut, WCONT_W/WCONT_H = dimensiunea ferestrei "curente"
 * (g_curwin), setata la inceputul fiecarei functii de desen/hit-test. */
#define WCONT_BASE_W 640
#define WCONT_BASE_H 400
#define WCONT_W cwv(g_curwin)
#define WCONT_H chv(g_curwin)

static int g_curwin;             /* fereastra pt. care se evalueaza WCONT_W/H */
static int set_tab;              /* Setari: 0 = Display, 1 = Despre */

static int W, H, tby;
static int ready;
static int menu_open;
static int hover_id = -1;       /* -1 nimic, 0..2 tab, 10 = butonul MyOS */
static int prev_btn;

/* window manager: 0..2 = terminalele, 3 = File Manager, 4 = Notepad,
 * 5 = Task Manager, 6 = Browser, 7 = Setari. zord[0] = jos ... zord[NWIN-1] = deasupra. */
#define NWIN   (CON_COUNT + 5)
#define FM_WIN CON_COUNT
#define NP_WIN (CON_COUNT + 1)
#define TM_WIN (CON_COUNT + 2)
#define BR_WIN (CON_COUNT + 3)
#define SET_WIN (CON_COUNT + 4)

/* meniul Start (centrat, stil Windows 11) */
#define MENU_W 540
#define MENU_H 380
#define MENU_X ((W - MENU_W) / 2)

/* rezolutiile oferite in Setari (prima = maxima, setata la boot) */
static const struct { int w, h; const char *label; } set_res[3] = {
    { 1920, 1080, "1920 x 1080  Full HD" },
    { 1280,  720, "1280 x 720   HD" },
    { 1024,  768, "1024 x 768" },
};

/* cw/ch = dimensiunea continutului (variabila: maximizare). Initializate
 * la baza in gui_init_sizes(). scx/scy/scw/sch = geometria salvata la
 * maximizare (pt. restaurare). maxed = 1 daca fereastra e maximizata. */
static struct {
    int cx, cy, cw, ch;
    int maxed, scx, scy, scw, sch;
} wins[NWIN] = {
    { 120, 70 }, { 190, 130 }, { 260, 190 }, { 230, 120 }, { 200, 100 },
    { 170, 90 }, { 150, 80 }, { 240, 110 },
};
static int zord[NWIN] = { 7, 6, 5, 4, 3, 2, 1, 0 };
static int win_vis[NWIN] = { 0, 0, 0, 0, 0, 0, 0, 0 };  /* nimic pornit la boot */
static int fwin = -1;                             /* fereastra focusata (-1 = nimic) */

/* iconuri colorate (RGBA 40x40) incarcate de pe disc — librarie de iconuri
 * randata pe host din fontul Segoe (vezi scripts/genicons.ps1). */
#define ICON_PX   40                 /* dimensiunea unui icon */
#define ICON_SLOT 8192               /* spatiu rezervat per icon (sectoare intregi) */
enum { IC_TERMINAL, IC_EXPLORER, IC_EDITOR, IC_TASKMGR, IC_BROWSER,
       IC_SETTINGS, IC_REBOOT, IC_POWER, IC_START, IC_COUNT };
static const char *icon_files[IC_COUNT] = {
    "ic_terminal.raw", "ic_explorer.raw", "ic_editor.raw", "ic_taskmgr.raw",
    "ic_browser.raw", "ic_settings.raw", "ic_reboot.raw", "ic_power.raw",
    "ic_start.raw",
};
static uint8_t *icons_buf;           /* buffer contiguu pt. toate iconurile */
static int icons_ok;                 /* 1 daca s-au incarcat */

static void load_icons(void)
{
    if (icons_buf)
        return;
    uint64_t phys = pmm_alloc_contig((IC_COUNT * ICON_SLOT + PMM_FRAME_SIZE - 1) /
                                     PMM_FRAME_SIZE);
    if (!phys)
        return;
    icons_buf = (uint8_t *)phys;
    icons_ok = 1;
    for (int i = 0; i < IC_COUNT; i++)
        if (fs_read_into(icon_files[i], icons_buf + (uint64_t)i * ICON_SLOT,
                         ICON_SLOT) != ICON_PX * ICON_PX * 4)
            icons_ok = 0;
}

static const uint32_t *icon_px(int idx)
{
    if (!icons_ok || idx < 0 || idx >= IC_COUNT)
        return 0;
    return (const uint32_t *)(icons_buf + (uint64_t)idx * ICON_SLOT);
}

/* deseneaza un icon centrat intr-un patrat (cx,cy,side) */
static void draw_icon_c(int cx, int cy, int side, int idx)
{
    const uint32_t *px = icon_px(idx);
    if (!px)
        return;
    fb_blit_rgba(cx + (side - ICON_PX) / 2, cy + (side - ICON_PX) / 2,
                 px, ICON_PX, ICON_PX);
}

static int drag_term = -1;      /* fereastra trasa cu mouse-ul */
static int drag_dx, drag_dy;

/* wallpaper-ul (imagine raw 32bpp, WxH) tinut in RAM; incarcat de pe disc */
static uint32_t *wallpap;
static int desk_bg;             /* 1 = wallpap contine imaginea de desktop */

/* pozitiile elementelor de splash (depind de existenta imaginii de fundal) */
static int sp_spin_cy, sp_bar_y;
static int splash_img;          /* 1 = splash-ul are imagine (pt. restaurare) */

static void dims(void)
{
    W = fb_width();
    H = fb_height();
    tby = H - TB_H;
}

/* aloca bufferul de wallpaper (WxHx4) daca nu exista deja */
static int wall_alloc(void)
{
    if (wallpap)
        return 1;
    uint32_t bytes = (uint32_t)W * (uint32_t)H * 4u;
    uint64_t phys = pmm_alloc_contig((bytes + PMM_FRAME_SIZE - 1) /
                                     PMM_FRAME_SIZE);
    if (phys == 0)
        return 0;
    wallpap = (uint32_t *)phys;
    return 1;
}

/* incarca o imagine raw de pe disc in bufferul de wallpaper.
 * Acceptam doar o imagine exact de dimensiunea ecranului (WxHx4). */
static int load_bg(const char *name)
{
    if (!wall_alloc())
        return 0;
    uint32_t bytes = (uint32_t)W * (uint32_t)H * 4u;
    return fs_read_into(name, wallpap, bytes) == (int64_t)bytes;
}

/* fundal procedural modern: gradient inchis + halou radial + vigneta.
 * Generat o singura data in `wallpap` (independent de rezolutie). */
static void wall_generate(void)
{
    int gx = W * 3 / 10, gy = H / 4;      /* centrul haloului */
    int vx = W / 2, vy = H / 2;           /* centrul vignetei */
    for (int y = 0; y < H; y++) {
        int t = y * 255 / (H > 0 ? H : 1);
        int r0 = 0x12 - (0x0A * t) / 255;
        int g0 = 0x1A - (0x10 * t) / 255;
        int b0 = 0x2E - (0x1C * t) / 255;
        uint32_t *row = wallpap + (uint64_t)y * W;
        for (int x = 0; x < W; x++) {
            int dx = x - gx, dy = y - gy;
            int glow = 46 - (dx * dx + dy * dy) / 16000;
            if (glow < 0) glow = 0;
            int wx = x - vx, wy = y - vy;
            int vig = (wx * wx + wy * wy) / 60000;
            int r = r0 + glow / 2 - vig / 3;
            int g = g0 + (glow * 2) / 3 - vig / 3;
            int b = b0 + glow - vig / 2;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* pregateste `wallpap` pentru desktop: imaginea de pe disc daca se
 * potriveste exact, altfel fundalul procedural. Cache-uit pentru W/H curent. */
static int wall_prepared;
static void wall_prepare(void)
{
    if (wall_prepared)
        return;
    if (!wall_alloc()) {
        desk_bg = 0;
        return;
    }
    uint32_t bytes = (uint32_t)W * (uint32_t)H * 4u;
    if (fs_read_into("desk.raw", wallpap, bytes) != (int64_t)bytes)
        wall_generate();
    desk_bg = 1;
    wall_prepared = 1;
}

/* adancimea repaint-urilor imbricate: la 0 suntem "top-level" si putem
 * face AA cu restaurarea colturilor; in interior desenam dur (clip-uit) */
static int repaint_depth;

/* File Manager (definit mai jos) */
static void fm_tb_draw(int hov);
static void np_tb_draw(int hov);
static void term_btn_draw(int hov);
static void tm_btn_draw(int hov);
static void tm_content_draw(void);
static void br_btn_draw(int hov);
static void set_tb_draw(int hov);
int gui_terminal_count(void);
static volatile int term_req = -1;  /* -1 nimic, -2 primul liber, 0..N anume */
static void fm_button(int i, int hov);
static void fm_arrow(int down, int hov);
static void fm_content_draw(void);

/* Setari (definit mai jos) */
static int  win_at(int x, int y);
static void set_content_draw(void);
static void set_content_click(int mx, int my);
static void gui_set_resolution(int w, int h);
static int  set_hover_at(int x, int y);
static void set_btn_row(int idx, int hov);

/* Notepad (definit mai jos) */
static void np_open_window(const char *name);
static void np_content_draw(void);
static void np_toolbar_btn(int i, int hov);
static void np_toolbar_action(int i);
static void np_content_click(int mx, int my);
static const char *np_title(void);
static void np_save(void);

/* ------------------------------------------------------------------ */
/* Iconite 16x16 (bitul 15 = pixelul din stanga) — setul nostru propriu. */

static const uint16_t ic_logo[16] = {
    0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0000,
    0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE, 0x0000,
};
static const uint16_t ic_clock[16] = {
    0x07E0, 0x1818, 0x2004, 0x4002, 0x4002, 0x8001, 0x8101, 0x8101,
    0x81E1, 0x8001, 0x4002, 0x4002, 0x2004, 0x1818, 0x07E0, 0x0000,
};
static const uint16_t ic_chip[16] = {
    0x2AA8, 0x2AA8, 0x3FFC, 0x3FFC, 0x3FFC, 0x3FFC, 0x330C, 0x330C,
    0x330C, 0x3FFC, 0x3FFC, 0x3FFC, 0x3FFC, 0x2AA8, 0x2AA8, 0x0000,
};
static const uint16_t ic_term[16] = {
    0xFFFF, 0x8001, 0x8001, 0x9001, 0x8801, 0x8401, 0x8801, 0x9001,
    0x8001, 0x87C1, 0x8001, 0x8001, 0x8001, 0x8001, 0xFFFF, 0x0000,
};
static const uint16_t ic_folder[16] = {
    0x0000, 0x3E00, 0x7FFC, 0x7FFC, 0x7FFC, 0x7FFC, 0x7FFC, 0x7FFC,
    0x7FFC, 0x7FFC, 0x7FFC, 0x7FFC, 0x0000, 0x0000, 0x0000, 0x0000,
};
static const uint16_t ic_file[16] = {
    0x3F00, 0x3F80, 0x3FC0, 0x3FE0, 0x3FE0, 0x3FE0, 0x3FE0, 0x3FE0,
    0x3FE0, 0x3FE0, 0x3FE0, 0x3FE0, 0x3FE0, 0x3FE0, 0x0000, 0x0000,
};
static const uint16_t ic_app[16] = {
    0x0000, 0x0000, 0x0C00, 0x0F00, 0x0FC0, 0x0FF0, 0x0FFC, 0x0FFC,
    0x0FF0, 0x0FC0, 0x0F00, 0x0C00, 0x0000, 0x0000, 0x0000, 0x0000,
};
static const uint16_t ic_note[16] = {
    0x3FF8, 0x2008, 0x2AA8, 0x2008, 0x2AA8, 0x2008, 0x2AA8, 0x2008,
    0x2228, 0x2008, 0x2AA8, 0x2008, 0x2008, 0x3FF8, 0x0000, 0x0000,
};
/* iconite mici pentru toolbar-ul Notepad */
static const uint16_t ic_new[16] = {   /* pagina goala cu colt indoit */
    0x0000, 0x1F00, 0x1180, 0x1140, 0x11E0, 0x1020, 0x1020, 0x1020,
    0x1020, 0x1020, 0x1020, 0x1020, 0x1020, 0x1FE0, 0x0000, 0x0000,
};
static const uint16_t ic_open[16] = {  /* folder deschis */
    0x0000, 0x0000, 0x7C00, 0x8200, 0x83FE, 0x8002, 0xFFFE, 0x8002,
    0x4004, 0x4004, 0x2008, 0x1FF0, 0x0000, 0x0000, 0x0000, 0x0000,
};
static const uint16_t ic_save[16] = {  /* discheta */
    0x0000, 0xFFF8, 0x8C18, 0x8C28, 0x8C48, 0x8C08, 0x8008, 0x9FC8,
    0xA028, 0xA028, 0xA028, 0x9FC8, 0x8008, 0xFFF8, 0x0000, 0x0000,
};
static const uint16_t ic_chart[16] = {  /* Task Manager: grafic de bare */
    0x0000, 0x0080, 0x0080, 0x0480, 0x0480, 0x04A0, 0x14A0, 0x14A0,
    0x14A8, 0x54A8, 0x54A8, 0x54AA, 0x54AA, 0x7FFE, 0x0000, 0x0000,
};
static const uint16_t ic_globe[16] = {  /* Browser: glob cu meridiane */
    0x0000, 0x07E0, 0x0DB0, 0x1998, 0x318C, 0x2184, 0x7FFE, 0x2184,
    0x2184, 0x318C, 0x1998, 0x0DB0, 0x07E0, 0x0000, 0x0000, 0x0000,
};
static const uint16_t ic_gear[16] = {   /* Setari: rotita dintata */
    0x0180, 0x0180, 0x0DB0, 0x7FFE, 0x3FFC, 0x1FF8, 0x1E78, 0x3C3C,
    0x3C3C, 0x1E78, 0x1FF8, 0x3FFC, 0x7FFE, 0x0DB0, 0x0180, 0x0180,
};
static const uint16_t ic_power[16] = {  /* Oprire: simbol power */
    0x0180, 0x0180, 0x0180, 0x0180, 0x3DBC, 0x6DB6, 0xCDB3, 0xCC03,
    0xCC03, 0xCDB3, 0x6C36, 0x3C3C, 0x0FF0, 0x0000, 0x0000, 0x0000,
};
static const uint16_t ic_reboot[16] = { /* Repornire: sageata circulara */
    0x0000, 0x0FE0, 0x1FF8, 0x3838, 0x600C, 0x6006, 0x000E, 0x003F,
    0x001E, 0x600C, 0x6006, 0x300C, 0x1C38, 0x0FF0, 0x0000, 0x0000,
};

static void icon16(int x, int y, const uint16_t *ic, uint32_t color)
{
    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++)
            if (ic[j] & (0x8000 >> i))
                fb_putpixel(x + i, y + j, color);
}

/* ------------------------------------------------------------------ */
/* Cursorul de mouse (sageata cu umbra, fundal salvat sub ea) */

#define CUR_W 12
#define CUR_H 19
static const uint16_t cur_shape[CUR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780,
    0x03C0, 0x03C0, 0x0180,
};
static uint32_t under[(CUR_W + 1) * (CUR_H + 1)];
static int ux = -1, uy;

static void cursor_hide(void)
{
    if (ux < 0)
        return;
    for (int j = 0; j <= CUR_H; j++)
        for (int i = 0; i <= CUR_W; i++)
            fb_putpixel(ux + i, uy + j, under[j * (CUR_W + 1) + i]);
    ux = -1;
}

static void cursor_show(int x, int y)
{
    for (int j = 0; j <= CUR_H; j++)
        for (int i = 0; i <= CUR_W; i++)
            under[j * (CUR_W + 1) + i] = fb_getpixel(x + i, y + j);
    ux = x;
    uy = y;
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            if (cur_shape[j] & (0x8000 >> i)) {
                fb_putpixel(x + i + 1, y + j + 1, 0x10141B);
                fb_putpixel(x + i, y + j, 0xF2F5FA);
            }
}

/* ------------------------------------------------------------------ */
/* Splash */

void splash_show(void)
{
    if (!fb_active())
        return;
    dims();

    splash_img = load_bg("splash.raw");
    if (splash_img) {
        /* imaginea de fundal a utilizatorului, pe tot ecranul */
        for (int y = 0; y < H; y++)
            fb_copy_row(0, y, wallpap + (uint64_t)y * W, W);
        sp_spin_cy = H - 250;
        sp_bar_y   = H - 180;
    } else {
        /* fallback: gradientul + logo-ul */
        for (int y = 0; y < H; y++) {
            int t = y * 255 / H;
            uint32_t r = (uint32_t)(0x0E - (0x09 * t) / 255);
            uint32_t g = (uint32_t)(0x14 - (0x0D * t) / 255);
            uint32_t b = (uint32_t)(0x26 - (0x18 * t) / 255);
            fb_fill(0, y, W, 1, (r << 16) | (g << 8) | b);
        }
        int lw = 5 * 8 * 7;
        int lx = (W - lw) / 2, ly = H / 2 - 170;
        fb_text_scaled(lx + 4, ly + 4, "DevOS", 0x101E3A, 7);
        fb_text_scaled(lx, ly, "DevOS", COL_ACCENT, 7);
        sp_spin_cy = H / 2 + 20;
        sp_bar_y   = H / 2 + 96;
    }

    /* chip-urile (bara + procent) se deseneaza O DATA, cu AA pe fundal;
     * cadrele urmatoare umplu doar interiorul lor solid */
    int bx = (W - 340) / 2;
    fb_fill_round2(bx - 4, sp_bar_y - 4, 348, 20, 10, 0x10141B, 2, 0);
    fb_fill_round2(bx, sp_bar_y, 340, 12, 6, 0x1C2842, 1, 0x10141B);
    fb_fill_round2(W / 2 - 28, sp_bar_y + 24, 56, 20, 10, 0x10141B, 2, 0);
}

void splash_progress(int pct)
{
    if (!fb_active())
        return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int bx = (W - 340) / 2;
    fb_fill_round2(bx + 2, sp_bar_y + 2, (336 * pct) / 100, 8, 4,
                   COL_ACCENT, 1, 0x1C2842);

    /* procentul, in chipul deja desenat de splash_show */
    char s[5];
    int p = 0;
    if (pct == 100) s[p++] = '1';
    if (pct >= 10)  s[p++] = (char)('0' + (pct / 10) % 10);
    s[p++] = (char)('0' + pct % 10);
    s[p++] = '%';
    s[p] = 0;
    fb_fill(W / 2 - 20, sp_bar_y + 26, 40, 16, 0x10141B);
    fb_text(W / 2 - p * 4, sp_bar_y + 26, s, COL_DIM, 0x10141B);
}

/* inelul de puncte rotitor (ca la Windows): 8 puncte, coada care se stinge */
static void splash_spinner(int frame)
{
    static const int px[8] = {  0,  16,  22,  16,   0, -16, -22, -16 };
    static const int py[8] = { -22, -16,   0,  16,  22,  16,   0, -16 };
    static const uint32_t shade[8] = {
        0x7FB8FF, 0x3D8BFF, 0x2B62B8, 0x1E4380,
        0x16305C, 0x102344, 0x0C1A33, 0x0A1528,
    };
    int cx = W / 2, cy = sp_spin_cy;

    /* restauram zona inelului din imagine (altfel AA-ul s-ar "ingrosa"
     * de la un cadru la altul) si desenam punctele cu AA pe fundal curat */
    if (splash_img)
        for (int y = cy - 28; y <= cy + 28; y++)
            fb_copy_row(cx - 28, y, wallpap + (uint64_t)y * W + cx - 28, 57);

    for (int i = 0; i < 8; i++) {
        int k = (i - frame) & 7;          /* distanta fata de punctul "cap" */
        fb_fill_round2(cx + px[i] - 3, cy + py[i] - 3, 7, 7, 3, shade[k],
                       splash_img ? 2 : 0, 0);
    }
}

/* animatia de incarcare: bara 0->100 + spinner, ~2.5 secunde */
void splash_animate(void)
{
    if (!fb_active())
        return;
    for (int f = 0; f <= 60; f++) {
        splash_progress((100 * f) / 60);
        splash_spinner(f);
        uint64_t t0 = pit_ticks();
        while (pit_ticks() - t0 < 4)
            __asm__ volatile("hlt");
    }
}

/* ------------------------------------------------------------------ */
/* Desktop: wallpaper + taskbar */

static void wallpaper_rect(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0)
            continue;
        if (yy >= tby)
            break;
        if (desk_bg) {
            /* imaginea utilizatorului, din bufferul RAM */
            fb_copy_row(x, yy, wallpap + (uint64_t)yy * W + x, w);
        } else {
            int t = yy * 255 / tby;
            uint32_t r = (uint32_t)(0x20 - (0x16 * t) / 255);
            uint32_t g = (uint32_t)(0x60 - (0x40 * t) / 255);
            uint32_t b = (uint32_t)(0xA8 - (0x70 * t) / 255);
            fb_fill(x, yy, w, 1, (r << 16) | (g << 8) | b);
        }
    }
}

/* ---- taskbar centrat, stil Windows 11, cu iconuri colorate ---- */
#define TB_N    7
#define TB_SLOT 52
#define TB_BTN  44
static const int tb_id[TB_N]   = { 10, 50, FM_WIN, NP_WIN, TM_WIN, BR_WIN, SET_WIN };
static const int tb_icon[TB_N] = { IC_START, IC_TERMINAL, IC_EXPLORER, IC_EDITOR,
                                   IC_TASKMGR, IC_BROWSER, IC_SETTINGS };

static int tb_start_x(void) { return (W - TB_N * TB_SLOT) / 2; }

static void tb_slot_rect(int i, int *x, int *y, int *w, int *h)
{
    *x = tb_start_x() + i * TB_SLOT + (TB_SLOT - TB_BTN) / 2;
    *y = tby + (TB_H - TB_BTN) / 2;
    *w = TB_BTN;
    *h = TB_BTN;
}

static int tb_active(int i)
{
    int id = tb_id[i];
    if (id == 10) return menu_open;
    if (id == 50) return gui_terminal_count() > 0;
    return win_vis[id];
}

static void tb_draw_slot(int i, int hov)
{
    if (!fb_active())
        return;
    int x, y, w, h;
    tb_slot_rect(i, &x, &y, &w, &h);
    int sx = x - (TB_SLOT - TB_BTN) / 2;
    fb_fill(sx, tby + 1, TB_SLOT, TB_H - 1, COL_TASKBAR);   /* curata slotul */
    int act = tb_active(i);
    if (hov || act) {
        uint32_t bg = hov ? 0x3A414E : 0x2C323C;
        fb_fill_round2(x, y, w, h, 12, bg, 1, COL_TASKBAR);
    }
    draw_icon_c(x, y, w, tb_icon[i]);
    if (act && tb_id[i] != 10) {       /* indicator Win11 sub aplicatia activa */
        int focused = (tb_id[i] == 50) ? (fwin >= 0 && fwin < CON_COUNT)
                                       : (fwin == tb_id[i]);
        int lw = focused ? 18 : 8;
        fb_fill_round2(x + (w - lw) / 2, tby + TB_H - 5, lw, 3, 1,
                       COL_ACCENT_HI, 1, COL_TASKBAR);
    }
}

static int tb_slot_of_id(int id)
{
    for (int i = 0; i < TB_N; i++)
        if (tb_id[i] == id)
            return i;
    return -1;
}

static void tb_draw_by_id(int id, int hov)
{
    int i = tb_slot_of_id(id);
    if (i >= 0)
        tb_draw_slot(i, hov);
}

/* wrappere pastrate pentru apelurile existente */
static void start_button(int hov) { tb_draw_by_id(10, hov); }

void desktop_draw(int active_term)
{
    if (!fb_active())
        return;
    dims();
    load_icons();
    wall_prepare();                   /* wallpaper: disc daca se potriveste, altfel procedural */
    wallpaper_rect(0, 0, W, tby);
    fb_fill(0, tby, W, TB_H, COL_TASKBAR);
    fb_fill(0, tby, W, 1, COL_TASKBAR_2);
    (void)active_term;
    for (int i = 0; i < TB_N; i++)
        tb_draw_slot(i, hover_id == tb_id[i]);
}

/* ------------------------------------------------------------------ */
/* Window manager */

int gui_win_x(int term) { return wins[term].cx; }
int gui_win_y(int term) { return wins[term].cy; }

/* dimensiunea continutului ferestrei t (0 in structura = dimensiunea de baza) */
static int cwv(int t) { return wins[t].cw ? wins[t].cw : WCONT_BASE_W; }
static int chv(int t) { return wins[t].ch ? wins[t].ch : WCONT_BASE_H; }

static void win_full_rect(int t, int *x, int *y, int *w, int *h)
{
    *x = wins[t].cx - 2;
    *y = wins[t].cy - 30;
    *w = cwv(t) + 4;
    *h = chv(t) + 34;
}

static int rect_hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int rect_isect(int ax, int ay, int aw, int ah,
                      int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static int zpos(int t)
{
    for (int i = 0; i < NWIN; i++)
        if (zord[i] == t)
            return i;
    return 0;
}

int gui_cell_visible(int term, int px, int py)
{
    if (!win_vis[term])
        return 0;                /* fereastra e minimizata/inchisa */

    /* acoperita de o fereastra vizibila aflata deasupra? */
    int zp = zpos(term);
    for (int i = zp + 1; i < NWIN; i++) {
        if (!win_vis[zord[i]])
            continue;
        int x, y, w, h;
        win_full_rect(zord[i], &x, &y, &w, &h);
        if (rect_isect(px, py, 8, 16, x, y, w, h))
            return 0;
    }
    /* sau de meniul Start (mereu deasupra)? */
    if (menu_open && rect_isect(px, py, 8, 16, MENU_X, tby - MENU_H - 6, MENU_W, MENU_H))
        return 0;
    return 1;
}

/* deseneaza doar continutul barei de titlu (fundalul vine de la rama) */
static void title_draw(int t)
{
    int cx = wins[t].cx, cy = wins[t].cy;
    int focused = (t == fwin);
    uint32_t bg = focused ? COL_TITLE_F : COL_TITLE_U;

    if (t == FM_WIN) {
        icon16(cx + 10, cy - 24, ic_folder, focused ? 0xF0C060 : COL_DIM);
        fb_text(cx + 34, cy - 24, "Fisiere - MyFS",
                focused ? COL_TEXT : COL_DIM, bg);
    } else if (t == NP_WIN) {
        icon16(cx + 10, cy - 24, ic_note, focused ? 0x8FD0FF : COL_DIM);
        fb_text(cx + 34, cy - 24, np_title(),
                focused ? COL_TEXT : COL_DIM, bg);
    } else if (t == TM_WIN) {
        icon16(cx + 10, cy - 24, ic_chart, focused ? 0x9FE0B0 : COL_DIM);
        fb_text(cx + 34, cy - 24, "Task Manager",
                focused ? COL_TEXT : COL_DIM, bg);
    } else if (t == BR_WIN) {
        icon16(cx + 10, cy - 24, ic_globe, focused ? 0x6FB4FF : COL_DIM);
        char tt[34] = "Browser";
        const char *pt = browser_title();
        if (pt && pt[0]) {
            int k = 7;
            const char *sep = " - ";
            for (int j = 0; sep[j] && k < 33; j++, k++) tt[k] = sep[j];
            for (int j = 0; pt[j] && k < 33; j++, k++) tt[k] = pt[j];
            tt[k] = '\0';
        }
        fb_text(cx + 34, cy - 24, tt, focused ? COL_TEXT : COL_DIM, bg);
    } else if (t == SET_WIN) {
        icon16(cx + 10, cy - 24, ic_gear, focused ? 0xCFD8E4 : COL_DIM);
        fb_text(cx + 34, cy - 24, "Setari",
                focused ? COL_TEXT : COL_DIM, bg);
    } else {
        icon16(cx + 10, cy - 24, ic_term, focused ? COL_ACCENT_HI : COL_DIM);
        char title[32] = "Terminal DevOS - F ";
        title[18] = (char)('1' + t);
        fb_text(cx + 34, cy - 24, title, focused ? COL_TEXT : COL_DIM, bg);
    }
    /* butoane rotunde (cercuri cu AA pe fundalul cunoscut al titlului) */
    int frw = cwv(t) + 4;
    fb_fill_round2(cx + frw - 26, cy - 22, 12, 12, 6, 0xE0554D, 1, bg);
    fb_fill_round2(cx + frw - 44, cy - 22, 12, 12, 6, 0xE0B04D, 1, bg);
    fb_fill_round2(cx + frw - 62, cy - 22, 12, 12, 6, 0x58C15A, 1, bg);
}

static void repaint_rect(int x, int y, int w, int h, int skip);
static void fm_content_draw(void);

static void win_draw(int t)
{
    if (!win_vis[t])
        return;
    int cx = wins[t].cx, cy = wins[t].cy;
    int focused = (t == fwin);
    int fx = cx - 2, fy = cy - 30;
    int frw = cwv(t) + 4, frh = chv(t) + 34;
    uint32_t bg = focused ? COL_TITLE_F : COL_TITLE_U;

    if (repaint_depth == 0) {
        /* AA determinist: restauram intai fundalul din colturi (wallpaper
         * sau ferestrele de sub noi), apoi rama se amesteca cu el */
        repaint_rect(fx, fy, 10, 10, t);
        repaint_rect(fx + frw - 10, fy, 10, 10, t);
        repaint_rect(fx, fy + frh - 10, 10, 10, t);
        repaint_rect(fx + frw - 10, fy + frh - 10, 10, 10, t);
        fb_fill_round2(fx, fy, frw, frh, 10, bg, 2, 0);
    } else {
        fb_fill_round2(fx, fy, frw, frh, 10, bg, 0, 0);
    }
    g_curwin = t;
    title_draw(t);
    if (t == FM_WIN)
        fm_content_draw();
    else if (t == NP_WIN)
        np_content_draw();
    else if (t == TM_WIN)
        tm_content_draw();
    else if (t == BR_WIN) {
        browser_set_size(cwv(BR_WIN), chv(BR_WIN));
        browser_draw(cx, cy);
    }
    else if (t == SET_WIN)
        set_content_draw();
    else
        console_repaint_term(t);
}

static void menu_draw(void);

/* redeseneaza o regiune, TAIAT la ea (clipping): wallpaper + ferestrele
 * vizibile care o ating, de jos in sus. skip = fereastra pe care
 * apelantul o deseneaza singur (ex. cea trasa cu mouse-ul). */
static void repaint_rect(int x, int y, int w, int h, int skip)
{
    if (w <= 0 || h <= 0)
        return;
    repaint_depth++;
    fb_set_clip(x, y, w, h);
    wallpaper_rect(x, y, w, h);
    for (int i = 0; i < NWIN; i++) {
        int t = zord[i];
        if (t == skip || !win_vis[t])
            continue;
        int wx, wy, ww, wh;
        win_full_rect(t, &wx, &wy, &ww, &wh);
        if (rect_isect(x, y, w, h, wx, wy, ww, wh))
            win_draw(t);
    }
    if (menu_open && rect_isect(x, y, w, h, MENU_X, tby - MENU_H - 6, MENU_W, MENU_H))
        menu_draw();
    fb_clear_clip();
    repaint_depth--;
}

static void fm_tb_draw(int hov);

/* muta focusul (si tastatura, pentru terminale) pe fereastra `t` */
static void focus_window(int t)
{
    fwin = t;
    if (t < CON_COUNT)
        console_set_active(t);

    /* ridicam fereastra in varful z-order-ului */
    int zp = zpos(t);
    for (int i = zp; i < NWIN - 1; i++)
        zord[i] = zord[i + 1];
    zord[NWIN - 1] = t;

    /* redesenam ferestrele vizibile de jos in sus (culorile de focus
     * s-au schimbat, iar rama are AA care cere fundal proaspat) */
    for (int i = 0; i < NWIN; i++)
        if (win_vis[zord[i]])
            win_draw(zord[i]);

    start_button(hover_id == 10);
    term_btn_draw(hover_id == 50);
    fm_tb_draw(hover_id == FM_WIN);
    np_tb_draw(hover_id == NP_WIN);
    tm_btn_draw(hover_id == TM_WIN);
    br_btn_draw(hover_id == BR_WIN);
    set_tb_draw(hover_id == SET_WIN);
    gui_clock();
}

void gui_focus_notify(int term)
{
    if (!fb_active())
        return;
    focus_window(term);
}

void gui_windows_open(void)
{
    if (!fb_active())
        return;

    /* la boot nu e nimic deschis: doar desktopul gol */
    for (int i = 0; i < NWIN; i++)
        if (win_vis[zord[i]])
            win_draw(zord[i]);
}

/* Deschiderea unei ferestre. Fereastra e desenata direct de apelant
 * (focus_window); aici doar pregatim fundalul, fara animatie cu busy-wait —
 * aceasta functie poate fi chemata din context de intrerupere (click/Alt+Fx),
 * unde o bucla lunga cu IF=0 ar bloca procesarea si ar corupe afisajul. */
static void win_open_anim(int t)
{
    (void)t;   /* fara animatie: focus_window deseneaza fereastra imediat */
}

void gui_desktop_ready(void)
{
    ready = 1;
}

static void taskbar_refresh(void);

/* focusul trece la cea mai de sus fereastra vizibila (sau nicaieri) */
static void focus_top_visible(void)
{
    for (int i = NWIN - 1; i >= 0; i--) {
        if (win_vis[zord[i]]) {
            focus_window(zord[i]);
            return;
        }
    }
    fwin = -1;                 /* nimic deschis */
    taskbar_refresh();
}

/* galben = minimizare; rosu = inchidere definitiva (curata terminalul) */
static void hide_win(int t, int definitive)
{
    win_vis[t] = 0;
    wins[t].maxed = 0;                 /* iese din maximizare la inchidere/minimizare */
    int x, y, w, h;
    win_full_rect(t, &x, &y, &w, &h);
    repaint_rect(x, y, w, h, -1);
    if (definitive && t < CON_COUNT)
        console_clear(t);
    taskbar_refresh();
    if (t == fwin)
        focus_top_visible();
}

/* verde = trimite fereastra in fundal (la baza z-order-ului) */
static void send_back(int t)
{
    int zp = zpos(t);
    for (int i = zp; i > 0; i--)
        zord[i] = zord[i - 1];
    zord[0] = t;

    int x, y, w, h;
    win_full_rect(t, &x, &y, &w, &h);
    repaint_rect(x, y, w, h, -1);
    focus_top_visible();
}

/* rosu = inchide fereastra complet; pentru terminale, termina si procesele
 * utilizator care ruleaza pe ele. */
static void close_win(int t)
{
    if (t < CON_COUNT) {
        struct task_info info;
        for (int i = 0; i < task_count_max(); i++)
            if (task_get_info(i, &info) && info.term == t)
                task_kill_id(i);
    }
    hide_win(t, 1);          /* win_vis=0, curata terminalul, iese din maximizare */
}

/* verde = maximizeaza / restaureaza fereastra */
static void toggle_maximize(int t)
{
    if (wins[t].maxed) {
        wins[t].cx = wins[t].scx; wins[t].cy = wins[t].scy;
        wins[t].cw = wins[t].scw; wins[t].ch = wins[t].sch;
        wins[t].maxed = 0;
    } else {
        wins[t].scx = wins[t].cx; wins[t].scy = wins[t].cy;
        wins[t].scw = wins[t].cw; wins[t].sch = wins[t].ch;
        wins[t].cx = 6;          wins[t].cy = 40;
        wins[t].cw = W - 12;     wins[t].ch = tby - 50;
        wins[t].maxed = 1;
    }
    /* redesenare completa: fundal + toate ferestrele (dimensiuni schimbate) */
    desktop_draw(0);
    for (int i = 0; i < NWIN; i++)
        if (win_vis[zord[i]])
            win_draw(zord[i]);
    gui_clock();
}

/* ------------------------------------------------------------------ */
/* Oprire / repornire */

static void power_reboot(void)
{
    /* reset prin controllerul 8042: pulsul liniei de reset a CPU-ului */
    uint8_t t = 0x02;
    while (t & 0x02)
        t = inb(0x64);
    outb(0x64, 0xFE);
    /* daca n-a mers, triple fault: IDT invalid + int */
    __asm__ volatile("cli");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idt0 = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(idt0));
    for (;;) __asm__ volatile("hlt");
}

static void power_shutdown(void)
{
    /* ACPI shutdown pe QEMU/Bochs (porturi cunoscute) */
    outw(0x604, 0x2000);     /* QEMU >= 2.0 */
    outw(0xB004, 0x2000);    /* Bochs / QEMU vechi */
    outw(0x600, 0x2000);     /* cloud-hypervisor */
    /* daca nu s-a oprit, oprim procesorul */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* Meniul Start */

static void fmt_u(char *dst, uint64_t v)
{
    char tmp[24];
    int i = 0;
    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    int p = 0;
    while (i--)
        dst[p++] = tmp[i];
    dst[p] = '\0';
}

/* elementele meniului Start (launcher). Id de hover = 80 + index. */
#define MENU_N    8
#define MENU_APPS 6
static const char *menu_labels[MENU_N] = {
    "Terminal", "Explorer", "Editor", "Task Manager", "Browser", "Setari",
    "Repornire", "Oprire",
};
static const int menu_app_icon[MENU_N] = {
    IC_TERMINAL, IC_EXPLORER, IC_EDITOR, IC_TASKMGR, IC_BROWSER, IC_SETTINGS,
    IC_REBOOT, IC_POWER,
};

/* dreptunghiul elementului `idx` (0..5 = grid aplicatii, 6=reboot, 7=power) */
static void menu_item_rect(int idx, int *x, int *y, int *w, int *h)
{
    int my0 = tby - MENU_H - 6;
    if (idx < MENU_APPS) {
        int cw = (MENU_W - 60) / 3;
        int col = idx % 3, row = idx / 3;
        *x = MENU_X + 30 + col * cw;
        *y = my0 + 84 + row * 108;
        *w = cw - 12;
        *h = 98;
    } else {
        int k = idx - 6;            /* 0=reboot, 1=power */
        *x = MENU_X + MENU_W - 30 - (2 - k) * 52;
        *y = my0 + MENU_H - 54;
        *w = 44;
        *h = 42;
    }
}

static void menu_item_draw(int idx, int hov)
{
    int bx, by, bw, bh;
    menu_item_rect(idx, &bx, &by, &bw, &bh);
    if (idx < MENU_APPS) {
        if (hov)
            fb_fill_round2(bx, by, bw, bh, 12, COL_CARD_HOV, 1, COL_MENU);
        else
            fb_fill_round2(bx, by, bw, bh, 12, COL_MENU, 1, COL_MENU);
        draw_icon_c(bx + (bw - ICON_PX) / 2, by + 12, ICON_PX, menu_app_icon[idx]);
        const char *lb = menu_labels[idx];
        int tw = (int)strlen(lb) * 8;
        uint32_t lbg = hov ? COL_CARD_HOV : COL_MENU;
        fb_text(bx + (bw - tw) / 2, by + bh - 26, lb, COL_TEXT, lbg);
    } else {
        uint32_t bg = hov ? COL_CARD_HOV : COL_MENU;
        fb_fill_round2(bx, by, bw, bh, 12, bg, 1, COL_MENU);
        draw_icon_c(bx, by + 1, bw, menu_app_icon[idx]);
    }
}

static void menu_draw(void)
{
    int my0 = tby - MENU_H - 6;
    fb_fill_round2(MENU_X, my0, MENU_W, MENU_H, 16, COL_MENU, 2, 0);

    draw_icon_c(MENU_X + 24, my0 + 18, 34, IC_START);
    fb_text_scaled(MENU_X + 64, my0 + 22, "DevOS", COL_TEXT, 2);
    fb_text(MENU_X + 24, my0 + 60, "Aplicatii", COL_DIM, COL_MENU);

    for (int i = 0; i < MENU_APPS; i++)
        menu_item_draw(i, hover_id == 80 + i);

    fb_fill(MENU_X + 24, my0 + MENU_H - 66, MENU_W - 48, 1, 0x2C3542);
    fb_text(MENU_X + 24, my0 + MENU_H - 44, "Gavrilencu Grigore", COL_DIM, COL_MENU);
    menu_item_draw(6, hover_id == 86);
    menu_item_draw(7, hover_id == 87);
}

static int menu_hover_at(int x, int y)
{
    if (!menu_open)
        return -1;
    for (int i = 0; i < MENU_N; i++) {
        int bx, by, bw, bh;
        menu_item_rect(i, &bx, &by, &bw, &bh);
        if (rect_hit(x, y, bx, by, bw, bh))
            return 80 + i;
    }
    return -1;
}

static void menu_close(void)
{
    menu_open = 0;
    repaint_rect(MENU_X, tby - MENU_H - 6, MENU_W, MENU_H, -1);
}

/* lanseaza aplicatia / actiunea aleasa din meniul Start */
static void menu_action(int idx)
{
    menu_close();
    switch (idx) {
    case 0:                         /* Terminal */
        if (gui_terminal_count() == 0) {
            term_req = -2;
        } else {
            for (int k = 0; k < CON_COUNT; k++)
                if (win_vis[k]) { focus_window(k); break; }
        }
        break;
    case 1: gui_fm_toggle();  break; /* Explorer */
    case 2: gui_np_toggle();  break; /* Editor */
    case 3: gui_tm_toggle();  break; /* Task Manager */
    case 4: gui_br_toggle();  break; /* Browser */
    case 5: gui_set_toggle(); break; /* Setari */
    case 6: power_reboot();   break; /* Repornire */
    case 7: power_shutdown(); break; /* Oprire */
    }
}

/* ------------------------------------------------------------------ */
/* File Manager — Explorer-ul lui MyOS (fereastra FM_WIN) */

#define FM_ROWS   14            /* randuri vizibile in lista */
#define FM_SIDE_W 140           /* latimea sidebar-ului cu categorii */
#define FM_LIST_X 148           /* unde incepe lista (dupa sidebar) */

static int fm_sel = -1;         /* pozitia selectata IN LISTA FILTRATA */
static int fm_off;              /* offsetul de scroll al listei */
static int fm_cat;              /* categoria: 0=Toate 1=Programe 2=Documente */
static int fm_input;            /* 0 nimic, 1 = fisier nou, 2 = redenumire */
static char fm_buf[24];
static int fm_len;
static char fm_status[44];
static uint64_t fm_click_t;     /* pentru dublu-click */
static int fm_click_row = -1;

/* Intrarile din vederea curenta a File Manager-ului: fie foldere virtuale
 * (prefixe de cale), fie fisiere. Folderele = nume MyFS cu "/" ca separator
 * (MyFS e plat, dar afisam ierarhic dupa cale). */
struct fm_entry {
    char name[24];      /* eticheta afisata (numele folderului sau fisierului) */
    int  is_dir;        /* 1 = folder (sau ".."), 0 = fisier */
    int  is_up;         /* 1 = intrarea ".." de urcare */
    int  fs_idx;        /* pentru fisiere: indexul in MyFS; altfel -1 */
};
static struct fm_entry fm_ent[80];
static int fm_n;

static char fm_path[48];    /* calea curenta, "" = radacina, altfel "docs/" */

/* meniul contextual (click dreapta): 0 inchis, 1 pe fisier, 2 pe gol */
static int cm_open;
static int cm_x, cm_y, cm_hover = -1;
#define CM_W  168
#define CM_IH 26

static uint8_t fm_copybuf[32768];   /* pentru "Copiaza" (max 32 KiB) */

static int name_has_dot(const char *s);

static int str_prefix(const char *pre, const char *s)
{
    while (*pre) {
        if (*pre != *s)
            return 0;
        pre++;
        s++;
    }
    return 1;
}

static void fm_add_entry(const char *name, int is_dir, int is_up, int fs_idx)
{
    if (fm_n >= 80)
        return;
    /* folderele nu se dubleaza */
    if (is_dir && !is_up)
        for (int i = 0; i < fm_n; i++)
            if (fm_ent[i].is_dir && strcmp(fm_ent[i].name, name) == 0)
                return;
    struct fm_entry *e = &fm_ent[fm_n++];
    int i = 0;
    for (; name[i] && i < 23; i++)
        e->name[i] = name[i];
    e->name[i] = '\0';
    e->is_dir = is_dir;
    e->is_up = is_up;
    e->fs_idx = fs_idx;
}

/* construieste vederea curenta: folderele in navigare (cat 0) sau lista
 * plata filtrata (cat 1/2) */
static void fm_build(void)
{
    int n = fs_count();
    fm_n = 0;

    if (fm_cat == 0) {
        /* modul "Acasa": navigare pe foldere din calea fm_path */
        int plen = (int)strlen(fm_path);
        if (plen > 0)
            fm_add_entry("..", 1, 1, -1);

        /* intai folderele (segmente de cale sub fm_path) */
        for (int i = 0; i < n; i++) {
            const char *nm = fs_get(i)->name;
            if (!str_prefix(fm_path, nm))
                continue;
            const char *rest = nm + plen;
            const char *sl = 0;
            for (const char *p = rest; *p; p++)
                if (*p == '/') { sl = p; break; }
            if (sl) {
                char folder[24];
                int k = 0;
                for (const char *p = rest; p < sl && k < 23; p++)
                    folder[k++] = *p;
                folder[k] = '\0';
                fm_add_entry(folder, 1, 0, -1);
            }
        }
        /* apoi fisierele direct in acest folder */
        for (int i = 0; i < n; i++) {
            const char *nm = fs_get(i)->name;
            if (!str_prefix(fm_path, nm))
                continue;
            const char *rest = nm + plen;
            int has_slash = 0;
            for (const char *p = rest; *p; p++)
                if (*p == '/') { has_slash = 1; break; }
            if (!has_slash && *rest)
                fm_add_entry(rest, 0, 0, i);
        }
    } else {
        /* Programe / Documente: lista plata, filtrata pe tot MyFS */
        for (int i = 0; i < n; i++) {
            const struct fs_file *f = fs_get(i);
            int isdoc = name_has_dot(f->name);
            if (fm_cat == 1 && isdoc)
                continue;
            if (fm_cat == 2 && !isdoc)
                continue;
            fm_add_entry(f->name, 0, 0, i);
        }
    }

    if (fm_sel >= fm_n)
        fm_sel = fm_n - 1;
    if (fm_off > 0 && fm_off + FM_ROWS > fm_n)
        fm_off = (fm_n - FM_ROWS < 0) ? 0 : fm_n - FM_ROWS;
}

static void fm_set_status(const char *s)
{
    int i = 0;
    for (; s[i] && i < 43; i++)
        fm_status[i] = s[i];
    fm_status[i] = '\0';
}

/* fisierul MyFS al randului selectat (0 daca e folder sau nimic selectat) */
static const struct fs_file *fm_sel_file(void)
{
    if (fm_sel < 0 || fm_sel >= fm_n || fm_ent[fm_sel].is_dir)
        return 0;
    return fs_get(fm_ent[fm_sel].fs_idx);
}

static const char *fm_sel_name(void)
{
    const struct fs_file *f = fm_sel_file();
    return f ? f->name : 0;
}

static int name_has_dot(const char *s)
{
    for (; *s; s++)
        if (*s == '.')
            return 1;
    return 0;
}

static void fm_status_draw(void)
{
    g_curwin = FM_WIN;
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;
    int sy = cy + WCONT_H - 26;
    fb_fill(cx, sy, WCONT_W, 26, 0x141821);

    if (fm_input) {
        const char *prompt = (fm_input == 1) ? "Nume fisier nou: "
                                             : "Redenumeste in: ";
        fb_text(cx + 10, sy + 5, prompt, COL_TEXT, 0x141821);
        char tmp[26];
        int i;
        for (i = 0; i < fm_len; i++)
            tmp[i] = fm_buf[i];
        tmp[i++] = '_';
        tmp[i] = '\0';
        fb_text(cx + 10 + (int)strlen(prompt) * 8, sy + 5, tmp,
                0xFFE9A8, 0x141821);
        fb_text(cx + WCONT_W - 186, sy + 5, "Enter=ok  Esc=anuleaza",
                COL_DIM, 0x141821);
        return;
    }

    char line[40], num[24];
    int p = 0;
    fmt_u(num, (uint64_t)fm_n);
    for (int i = 0; num[i]; i++)
        line[p++] = num[i];
    const char *suf = " elemente";
    while (*suf)
        line[p++] = *suf++;
    line[p] = '\0';
    fb_text(cx + 10, sy + 5, line, COL_DIM, 0x141821);

    if (fm_status[0])
        fb_text(cx + WCONT_W - (int)strlen(fm_status) * 8 - 10, sy + 5,
                fm_status, 0x9FD49F, 0x141821);
}

static const char *fm_btn_lbl[5] = {
    "+ Nou", "Redenum.", "Copiaza", "Sterge", "Editeaza",
};

static void fm_btn_rect(int i, int *x, int *y, int *w, int *h)
{
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;
    int xx = cx + 8;
    for (int k = 0; k < i; k++)
        xx += (int)strlen(fm_btn_lbl[k]) * 8 + 26;
    *x = xx;
    *y = cy + 30;
    *w = (int)strlen(fm_btn_lbl[i]) * 8 + 20;
    *h = 26;
}

static void fm_button(int i, int hov)
{
    int x, y, w, h;
    fm_btn_rect(i, &x, &y, &w, &h);
    uint32_t bg = hov ? 0x3A4356 : 0x2A3140;
    fb_fill_round2(x, y, w, h, 13, bg, 1, 0x1E232D);
    uint32_t fg = (i == 3) ? 0xF08A80 : COL_TEXT;
    fb_text(x + 10, y + 5, fm_btn_lbl[i], fg, bg);
}

static const char *fm_cat_lbl[3] = { "Toate", "Programe", "Documente" };

static void fm_side_item(int i, int hov)
{
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;
    int x = cx + 8, y = cy + 72 + i * 34;
    uint32_t bg = (fm_cat == i) ? 0x2E5C9E : (hov ? 0x262D3A : 0x1B202A);
    fb_fill_round2(x, y, FM_SIDE_W - 16, 28, 9, bg, 1, 0x1B202A);
    const uint16_t *ic = (i == 0) ? ic_folder : (i == 1 ? ic_app : ic_file);
    icon16(x + 8, y + 6, ic,
           i == 0 ? 0xF0C060 : (i == 1 ? 0x76C7FF : 0x9FB6D4));
    fb_text(x + 32, y + 6, fm_cat_lbl[i], COL_TEXT, bg);
}

static void fm_arrow(int down, int hov)
{
    g_curwin = FM_WIN;
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;
    int x = cx + WCONT_W - 30;
    int y = down ? cy + WCONT_H - 26 - 24 : cy + 66;
    uint32_t bg = hov ? 0x3A4356 : 0x242A36;
    fb_fill_round2(x, y, 22, 22, 8, bg, 1, 0x171B23);
    fb_text(x + 7, y + 3, down ? "v" : "^", COL_DIM, bg);
}

static void fm_content_draw(void)
{
    g_curwin = FM_WIN;
    fm_build();
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;

    /* zonele: breadcrumb / toolbar / sidebar / lista / status */
    fb_fill(cx, cy, WCONT_W, 26, 0x141821);
    fb_fill(cx, cy + 26, WCONT_W, 36, 0x1E232D);
    fb_fill(cx, cy + 62, FM_SIDE_W, WCONT_H - 62 - 26, 0x1B202A);
    fb_fill(cx + FM_SIDE_W, cy + 62, WCONT_W - FM_SIDE_W,
            WCONT_H - 62 - 26, 0x171B23);

    /* breadcrumb: MyFS > cale/categorie (clicabil pentru urcare) */
    icon16(cx + 8, cy + 5, ic_folder, 0xF0C060);
    fb_text(cx + 30, cy + 5, "MyFS", fm_cat == 0 ? COL_ACCENT_HI : COL_TEXT,
            0x141821);
    int bx = cx + 72;
    if (fm_cat != 0) {
        fb_text(bx, cy + 5, ">", COL_DIM, 0x141821);
        fb_text(bx + 16, cy + 5, fm_cat_lbl[fm_cat], COL_ACCENT_HI, 0x141821);
    } else {
        /* segmentele caii */
        int start = 0;
        for (int i = 0; fm_path[i]; i++) {
            if (fm_path[i] == '/') {
                char seg[24];
                int k = 0;
                for (int j = start; j < i && k < 23; j++)
                    seg[k++] = fm_path[j];
                seg[k] = '\0';
                fb_text(bx, cy + 5, ">", COL_DIM, 0x141821);
                fb_text(bx + 16, cy + 5, seg, COL_ACCENT_HI, 0x141821);
                bx += 16 + k * 8 + 8;
                start = i + 1;
            }
        }
    }

    for (int i = 0; i < 5; i++)
        fm_button(i, hover_id == 20 + i);
    for (int i = 0; i < 3; i++)
        fm_side_item(i, hover_id == 26 + i);

    int ry = cy + 66;
    for (int r = 0; r < FM_ROWS && fm_off + r < fm_n; r++) {
        const struct fm_entry *e = &fm_ent[fm_off + r];
        int y = ry + r * 22;
        int is_sel = (fm_off + r == fm_sel);
        uint32_t bg = is_sel ? 0x2E5C9E : ((r & 1) ? 0x1B1F28 : 0x171B23);
        fb_fill_round2(cx + FM_LIST_X, y, WCONT_W - FM_LIST_X - 36, 20, 7,
                       bg, 1, 0x171B23);

        if (e->is_up) {
            icon16(cx + FM_LIST_X + 6, y + 2, ic_folder, 0xF0C060);
            fb_text(cx + FM_LIST_X + 30, y + 2, ".. (inapoi)",
                    is_sel ? 0xFFFFFF : COL_DIM, bg);
            continue;
        }
        if (e->is_dir) {
            icon16(cx + FM_LIST_X + 6, y + 2, ic_folder, 0xF0C060);
            fb_text(cx + FM_LIST_X + 30, y + 2, e->name,
                    is_sel ? 0xFFFFFF : COL_TEXT, bg);
            fb_text(cx + WCONT_W - 92, y + 2, "folder",
                    is_sel ? 0xD9E6F7 : COL_DIM, bg);
            continue;
        }

        const struct fs_file *f = fs_get(e->fs_idx);
        int prot = fs_is_protected(f->name);
        int isdoc = name_has_dot(e->name);
        icon16(cx + FM_LIST_X + 6, y + 2, isdoc ? ic_file : ic_app,
               isdoc ? 0x9FB6D4 : 0x76C7FF);
        uint32_t fg = is_sel ? 0xFFFFFF : (prot ? 0x8FA0B4 : COL_TEXT);
        fb_text(cx + FM_LIST_X + 30, y + 2, e->name, fg, bg);
        if (prot)
            fb_text(cx + FM_LIST_X + 30 + (int)strlen(e->name) * 8 + 6, y + 2,
                    "(sistem)", is_sel ? 0xCBD8EA : 0x6C7A8E, bg);

        char num[24];
        fmt_u(num, f->size);
        int nl2 = (int)strlen(num);
        fb_text(cx + WCONT_W - 52 - nl2 * 8, y + 2, num,
                is_sel ? 0xD9E6F7 : COL_DIM, bg);
        fb_text(cx + WCONT_W - 48, y + 2, "B",
                is_sel ? 0xD9E6F7 : COL_DIM, bg);
    }

    if (fm_n > FM_ROWS) {
        fm_arrow(0, hover_id == 30);
        fm_arrow(1, hover_id == 31);
    }
    fm_status_draw();
}

/* copiaza fisierul selectat sub numele "c_<nume>" */
static void fm_copy(void)
{
    const struct fs_file *f = fm_sel_file();
    if (!f) {
        fm_set_status("selecteaza un fisier");
        fm_status_draw();
        return;
    }
    if (((f->size + 511) / 512) * 512 > sizeof(fm_copybuf)) {
        fm_set_status("prea mare pentru copiere");
        fm_status_draw();
        return;
    }
    char nn[24];
    nn[0] = 'c';
    nn[1] = '_';
    int p = 2;
    for (int i = 0; f->name[i] && p < 23; i++)
        nn[p++] = f->name[i];
    nn[p] = '\0';
    if (fs_find(nn)) {
        fm_set_status("copia exista deja");
        fm_status_draw();
        return;
    }
    int64_t n2 = fs_read_into(f->name, fm_copybuf, sizeof(fm_copybuf));
    if (n2 < 0 || fs_save(nn, fm_copybuf, (uint32_t)n2) != 0)
        fm_set_status("eroare la copiere");
    else
        fm_set_status("copiat (c_...)");
    fm_content_draw();
}

/* butoanele din taskbar: acum sunt sloturi centrate cu iconuri colorate;
 * pastram numele vechi ca wrappere spre tb_draw_by_id. */
static void fm_tb_draw(int hov)   { tb_draw_by_id(FM_WIN, hov); }
static void np_tb_draw(int hov)   { tb_draw_by_id(NP_WIN, hov); }
static void term_btn_draw(int hov){ tb_draw_by_id(50, hov); }
static void tm_btn_draw(int hov)  { tb_draw_by_id(TM_WIN, hov); }
static void br_btn_draw(int hov)  { tb_draw_by_id(BR_WIN, hov); }
static void set_tb_draw(int hov)  { tb_draw_by_id(SET_WIN, hov); }

static void taskbar_refresh(void)
{
    start_button(hover_id == 10);
    term_btn_draw(hover_id == 50);
    fm_tb_draw(hover_id == FM_WIN);
    np_tb_draw(hover_id == NP_WIN);
    tm_btn_draw(hover_id == TM_WIN);
    br_btn_draw(hover_id == BR_WIN);
    set_tb_draw(hover_id == SET_WIN);
}

/* urca un nivel in ierarhie (taie ultimul segment din fm_path) */
static void fm_go_up(void)
{
    int len = (int)strlen(fm_path);
    if (len == 0)
        return;
    int i = len - 2;                /* sar peste "/"-ul final */
    while (i >= 0 && fm_path[i] != '/')
        i--;
    fm_path[i + 1] = '\0';          /* pastram pana la "/"-ul precedent */
    fm_sel = -1;
    fm_off = 0;
    fm_content_draw();
}

/* intra intr-un folder (adauga segmentul la fm_path) */
static void fm_enter_dir(const char *folder)
{
    int len = (int)strlen(fm_path);
    for (int i = 0; folder[i] && len < 45; i++)
        fm_path[len++] = folder[i];
    if (len < 46)
        fm_path[len++] = '/';
    fm_path[len] = '\0';
    fm_sel = -1;
    fm_off = 0;
    fm_content_draw();
}

/* activeaza randul selectat: folder -> intra/urca; fisier -> deschide */
static void fm_open_selected(void)
{
    if (fm_sel < 0 || fm_sel >= fm_n) {
        fm_set_status("selecteaza ceva");
        fm_status_draw();
        return;
    }
    struct fm_entry *e = &fm_ent[fm_sel];
    if (e->is_up) {
        fm_go_up();
        return;
    }
    if (e->is_dir) {
        fm_enter_dir(e->name);
        return;
    }

    const struct fs_file *f = fs_get(e->fs_idx);
    if (!f) {
        fm_set_status("selecteaza un fisier");
        fm_status_draw();
        return;
    }
    if (f->size > 16000) {
        fm_set_status("prea mare pentru editor");
        fm_status_draw();
        return;
    }
    /* deschide fisierul in Notepad (editorul grafic) */
    np_open_window(f->name);
}

static void fm_commit_input(void)
{
    fm_buf[fm_len] = '\0';
    if (fm_input == 1) {
        /* fisier nou in folderul curent: fm_path + nume */
        char full[48];
        int p = 0;
        for (int i = 0; fm_path[i] && p < 46; i++)
            full[p++] = fm_path[i];
        for (int i = 0; fm_buf[i] && p < 46; i++)
            full[p++] = fm_buf[i];
        full[p] = '\0';
        if (fm_len == 0)
            fm_set_status("nume gol");
        else if (fs_find(full))
            fm_set_status("exista deja");
        else if (fs_save(full, "\n", 1) == 0)
            fm_set_status("fisier creat");
        else
            fm_set_status("eroare la creare");
    } else if (fm_input == 2) {
        /* redenumire pastrand folderul curent */
        const char *nm = fm_sel_name();
        char full[48];
        int p = 0;
        for (int i = 0; fm_path[i] && p < 46; i++)
            full[p++] = fm_path[i];
        for (int i = 0; fm_buf[i] && p < 46; i++)
            full[p++] = fm_buf[i];
        full[p] = '\0';
        int r = nm ? fs_rename(nm, full) : -1;
        fm_set_status(r == 0 ? "redenumit"
                     : (r == -3 ? "fisier de sistem, protejat"
                     : (r == -2 ? "nume invalid sau ocupat" : "eroare")));
    }
    fm_input = 0;
    fm_content_draw();
}

static void fm_action(int id)
{
    switch (id) {
    case 20:                            /* + Nou */
        fm_input = 1;
        fm_len = 0;
        fm_status_draw();
        break;
    case 21: {                          /* Redenumeste */
        const char *nm = fm_sel_name();
        if (!nm) {
            fm_set_status("selecteaza un fisier");
            fm_status_draw();
        } else if (fs_is_protected(nm)) {
            fm_set_status("fisier de sistem, protejat");
            fm_status_draw();
        } else {
            fm_input = 2;
            fm_len = 0;
            fm_status_draw();
        }
        break;
    }
    case 22:                            /* Copiaza */
        fm_copy();
        break;
    case 23: {                          /* Sterge */
        const char *nm = fm_sel_name();
        if (!nm) {
            fm_set_status("selecteaza un fisier");
            fm_status_draw();
        } else if (fs_is_protected(nm)) {
            fm_set_status("fisier de sistem, protejat");
            fm_status_draw();
        } else {
            fs_delete(nm);
            fm_set_status("sters");
            fm_sel = -1;
            fm_content_draw();
        }
        break;
    }
    case 24:                            /* Editeaza */
        fm_open_selected();
        break;
    case 26:                            /* categoriile din sidebar */
    case 27:
    case 28:
        fm_cat = id - 26;
        fm_path[0] = '\0';              /* reincepem de la radacina */
        fm_sel = -1;
        fm_off = 0;
        fm_content_draw();
        break;
    case 30:                            /* scroll sus */
        if (fm_off > 0) {
            fm_off--;
            fm_content_draw();
        }
        break;
    case 31:                            /* scroll jos */
        if (fm_off + FM_ROWS < fm_n) {
            fm_off++;
            fm_content_draw();
        }
        break;
    case 32:                            /* breadcrumb "MyFS" = radacina */
        fm_cat = 0;
        fm_path[0] = '\0';
        fm_sel = -1;
        fm_off = 0;
        fm_content_draw();
        break;
    }
}

/* randul listei de sub punctul (mx,my), sau -1 */
static int fm_row_at(int mx, int my)
{
    g_curwin = FM_WIN;
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;
    int ry = cy + 66;
    if (my < ry || my >= ry + FM_ROWS * 22 ||
        mx < cx + FM_LIST_X || mx >= cx + WCONT_W - 36)
        return -1;
    int idx = fm_off + (my - ry) / 22;
    return (idx < fm_n) ? idx : -1;
}

static void fm_content_click(int mx, int my)
{
    int idx = fm_row_at(mx, my);
    if (idx < 0)
        return;

    uint64_t now = pit_ticks();
    if (idx == fm_click_row && now - fm_click_t < 45) {
        fm_sel = idx;                  /* dublu-click = deschide */
        fm_open_selected();
    } else {
        fm_sel = idx;
        fm_content_draw();
    }
    fm_click_row = idx;
    fm_click_t = now;
}

/* ------------------------------------------------------------------ */
/* Meniul contextual (click dreapta) */

static const char *cm_file_items[4] = {
    "Deschide", "Redenumeste", "Copiaza", "Sterge",
};
static const char *cm_empty_items[2] = {
    "Fisier nou", "Reimprospateaza",
};
static const char *cm_term_items[1] = {
    "Terminal nou",
};

static int cm_count(void)
{
    if (cm_open == 1)
        return 4;
    if (cm_open == 3)
        return 1;
    return 2;
}

static void cm_draw(void)
{
    int n = cm_count();
    fb_fill_round2(cm_x, cm_y, CM_W, n * CM_IH + 12, 10, 0x232936, 2, 0);
    for (int i = 0; i < n; i++) {
        int y = cm_y + 6 + i * CM_IH;
        uint32_t bg = (i == cm_hover) ? 0x2E5C9E : 0x232936;
        fb_fill_round2(cm_x + 6, y, CM_W - 12, CM_IH - 2, 8, bg, 1, 0x232936);
        const char *lbl = (cm_open == 1) ? cm_file_items[i]
                        : (cm_open == 3) ? cm_term_items[i]
                                         : cm_empty_items[i];
        uint32_t fg = (cm_open == 1 && i == 3) ? 0xF08A80 : COL_TEXT;
        fb_text(cm_x + 16, y + 4, lbl, fg, bg);
    }
}

static void cm_close(void)
{
    int n = cm_count();
    cm_open = 0;
    repaint_rect(cm_x - 2, cm_y - 2, CM_W + 4, n * CM_IH + 16, -1);
}

static void cm_show(int kind, int x, int y)
{
    cm_open = kind;
    cm_hover = -1;
    int n = cm_count();
    if (x + CM_W > W - 4)
        x = W - 4 - CM_W;
    if (y + n * CM_IH + 12 > tby - 4)
        y = tby - 4 - (n * CM_IH + 12);
    cm_x = x;
    cm_y = y;
    cm_draw();
}

static int cm_item_at(int x, int y)
{
    if (!cm_open)
        return -1;
    int n = cm_count();
    if (x < cm_x + 6 || x >= cm_x + CM_W - 6)
        return -1;
    if (y < cm_y + 6 || y >= cm_y + 6 + n * CM_IH)
        return -1;
    return (y - cm_y - 6) / CM_IH;
}

static void cm_exec(int i)
{
    int kind = cm_open;
    cm_close();
    if (kind == 1) {
        if (i == 0)
            fm_open_selected();
        else if (i == 1)
            fm_action(21);
        else if (i == 2)
            fm_copy();
        else if (i == 3)
            fm_action(23);
    } else if (kind == 3) {
        if (i == 0)              /* Terminal nou */
            term_req = -2;       /* init deschide urmatorul terminal liber */
    } else {
        if (i == 0)
            fm_action(20);
        else
            fm_content_draw();
    }
}

static void fm_open_window(void)
{
    win_vis[FM_WIN] = 1;
    win_open_anim(FM_WIN);
    fm_cat = 0;
    fm_path[0] = '\0';
    fm_sel = -1;
    fm_off = 0;
    fm_set_status("");
    focus_window(FM_WIN);
}

void gui_fm_toggle(void)
{
    if (!fb_active() || !ready)
        return;
    if (!win_vis[FM_WIN])
        fm_open_window();
    else if (fwin == FM_WIN)
        hide_win(FM_WIN, 0);
    else
        focus_window(FM_WIN);
}

void gui_np_toggle(void)
{
    if (!fb_active() || !ready)
        return;
    if (!win_vis[NP_WIN])
        np_open_window(0);
    else if (fwin == NP_WIN)
        hide_win(NP_WIN, 0);
    else
        focus_window(NP_WIN);
}

void gui_tm_toggle(void)
{
    if (!fb_active() || !ready)
        return;
    if (!win_vis[TM_WIN]) {
        win_vis[TM_WIN] = 1;
        win_open_anim(TM_WIN);
        focus_window(TM_WIN);
    } else if (fwin == TM_WIN)
        hide_win(TM_WIN, 0);
    else
        focus_window(TM_WIN);
}

void gui_br_toggle(void)
{
    if (!fb_active() || !ready)
        return;
    if (!win_vis[BR_WIN]) {
        win_vis[BR_WIN] = 1;
        win_open_anim(BR_WIN);
        focus_window(BR_WIN);
    } else if (fwin == BR_WIN)
        hide_win(BR_WIN, 0);
    else
        focus_window(BR_WIN);
}

/* tasta Windows/Super: deschide/inchide meniul Start */
void gui_menu_toggle(void)
{
    if (!fb_active() || !ready)
        return;
    if (menu_open)
        menu_close();
    else {
        menu_open = 1;
        menu_draw();
    }
}

/* F11: maximizeaza / restaureaza fereastra focusata */
void gui_maximize_focused(void)
{
    if (!fb_active() || !ready)
        return;
    if (fwin >= 0 && fwin < NWIN && win_vis[fwin])
        toggle_maximize(fwin);
}

void gui_set_toggle(void)
{
    if (!fb_active() || !ready)
        return;
    if (!win_vis[SET_WIN]) {
        win_vis[SET_WIN] = 1;
        win_open_anim(SET_WIN);
        focus_window(SET_WIN);
    } else if (fwin == SET_WIN)
        hide_win(SET_WIN, 0);
    else
        focus_window(SET_WIN);
}

/* redeseneaza continutul browserului cand firul de retea a terminat */
void gui_refresh_browser(void)
{
    if (!fb_active() || !ready)
        return;
    if (win_vis[BR_WIN] && browser_poll_dirty())
        browser_draw(wins[BR_WIN].cx, wins[BR_WIN].cy);
    /* Task Manager live: reimprospatam lista de procese des (nou proces
     * aparut / proces terminat / kill) daca fereastra e sus si vizibila */
    if (win_vis[TM_WIN] && fwin == TM_WIN)
        tm_content_draw();
}

/* --- Management terminale (deschise la cerere, nu la boot) --- */

int gui_terminal_free_slot(void)
{
    for (int c = 0; c < CON_COUNT; c++)
        if (!win_vis[c])
            return c;
    return -1;
}

int gui_poll_term_request(void)
{
    int r = term_req;
    term_req = -1;
    return r;
}

/* cere deschiderea unui terminal: slot -2 = primul liber, 0..N = anume */
void gui_request_terminal(int slot)
{
    if (!fb_active() || !ready)
        return;
    term_req = slot;
}

/* serializare: operatiile GUI din context de task (init) nu trebuie
 * intrerupte de IRQ-ul de mouse care deseneaza cursorul */
static inline uint64_t gui_lock(void)
{
    uint64_t fl;
    __asm__ volatile("pushfq; cli; pop %0" : "=r"(fl));
    return fl;
}
static inline void gui_unlock(uint64_t fl)
{
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "cc");
}

void gui_open_terminal(int con)
{
    if (con < 0 || con >= CON_COUNT)
        return;
    uint64_t fl = gui_lock();
    win_vis[con] = 1;
    focus_window(con);
    gui_unlock(fl);
}

void gui_close_terminal(int con)
{
    if (con < 0 || con >= CON_COUNT)
        return;
    uint64_t fl = gui_lock();
    win_vis[con] = 0;
    console_clear(con);
    int x, y, w, h;
    win_full_rect(con, &x, &y, &w, &h);
    repaint_rect(x, y, w, h, -1);
    if (fwin == con)
        focus_top_visible();
    term_btn_draw(0);
    gui_unlock(fl);
}

int gui_terminal_count(void)
{
    int n = 0;
    for (int c = 0; c < CON_COUNT; c++)
        if (win_vis[c])
            n++;
    return n;
}

int gui_terminal_is_open(int con)
{
    return con >= 0 && con < CON_COUNT && win_vis[con];
}

/* fereastra vizibila cea mai de sus care contine punctul (x,y) */
static int win_at(int x, int y)
{
    for (int i = NWIN - 1; i >= 0; i--) {
        int t = zord[i];
        if (!win_vis[t])
            continue;
        if (rect_hit(x, y, wins[t].cx - 2, wins[t].cy - 30,
                     cwv(t) + 4, chv(t) + 34))
            return t;
    }
    return -1;
}

/* id-urile de hover din interiorul FM (doar cand FM e deasupra punctului) */
static int fm_hover_at(int x, int y)
{
    if (!win_vis[FM_WIN] || win_at(x, y) != FM_WIN)
        return -1;
    g_curwin = FM_WIN;
    int cx = wins[FM_WIN].cx, cy = wins[FM_WIN].cy;

    for (int i = 0; i < 5; i++) {
        int bx, by, bw, bh;
        fm_btn_rect(i, &bx, &by, &bw, &bh);
        if (rect_hit(x, y, bx, by, bw, bh))
            return 20 + i;
    }
    for (int i = 0; i < 3; i++)
        if (rect_hit(x, y, cx + 8, cy + 72 + i * 34, FM_SIDE_W - 16, 28))
            return 26 + i;
    if (rect_hit(x, y, cx + 28, cy + 2, 48, 22))   /* breadcrumb "MyFS" */
        return 32;
    if (fm_n > FM_ROWS) {
        if (rect_hit(x, y, cx + WCONT_W - 30, cy + 66, 22, 22))
            return 30;
        if (rect_hit(x, y, cx + WCONT_W - 30, cy + WCONT_H - 26 - 24, 22, 22))
            return 31;
    }
    return -1;
}

static int np_key(char ch);

int gui_key_intercept(char ch)
{
    /* Browser focusat: bara de adresa + derulare + navigare */
    if (fb_active() && ready && fwin == BR_WIN && win_vis[BR_WIN]) {
        int r = browser_key(ch);
        browser_draw(wins[BR_WIN].cx, wins[BR_WIN].cy);
        return r;
    }

    /* Notepad focusat: primeste toate tastele (editare libera) */
    if (fb_active() && ready && fwin == NP_WIN && win_vis[NP_WIN])
        return np_key(ch);

    /* Setari focusat: 1/2/3 = rezolutie (in tabul Display), d/a = taburi */
    if (fb_active() && ready && fwin == SET_WIN && win_vis[SET_WIN]) {
        if (ch == 'd' || ch == 'a') {
            int nt = (ch == 'd') ? 0 : 1;
            if (set_tab != nt) { set_tab = nt; set_content_draw(); }
        } else if (ch >= '1' && ch <= '3' && set_tab == 0) {
            gui_set_resolution(set_res[ch - '1'].w, set_res[ch - '1'].h);
        }
        return 1;
    }

    if (!fb_active() || !ready || fwin != FM_WIN || !win_vis[FM_WIN])
        return 0;
    unsigned char c = (unsigned char)ch;

    if (cm_open) {                      /* meniul contextual: Esc inchide */
        if (c == 27)
            cm_close();
        return 1;
    }

    if (fm_input) {
        if (c == '\n')
            fm_commit_input();
        else if (c == 27) {
            fm_input = 0;
            fm_status_draw();
        } else if (c == '\b') {
            if (fm_len > 0)
                fm_len--;
            fm_status_draw();
        } else if (c >= 32 && c <= 126 && fm_len < 23) {
            fm_buf[fm_len++] = (char)c;
            fm_status_draw();
        }
        return 1;
    }

    if (c == 0x80) {                    /* sus */
        if (fm_sel > 0)
            fm_sel--;
        else
            fm_sel = 0;
        if (fm_sel < fm_off)
            fm_off = fm_sel;
        fm_content_draw();
    } else if (c == 0x81) {             /* jos */
        if (fm_sel < fm_n - 1)
            fm_sel++;
        if (fm_sel >= fm_off + FM_ROWS)
            fm_off = fm_sel - FM_ROWS + 1;
        fm_content_draw();
    } else if (c >= '1' && c <= '3') {  /* 1/2/3 = categoria (ca sidebar) */
        fm_action(26 + (c - '1'));
    } else if (c == '\n') {
        fm_open_selected();
    }
    return 1;   /* FM focusat: tastele nu ajung la terminale */
}

/* ------------------------------------------------------------------ */
/* Notepad — editor de text grafic (fereastra NP_WIN), ca in Windows.
 * Tastezi liber; cursorul se misca cu sagetile; Enter = linie noua;
 * Backspace sterge; toolbar cu Nou / Deschide / Salveaza. */

#define NP_COLS   76            /* caractere pe linie vizuala */
#define NP_ROWS   20            /* linii vizibile */
#define NP_TX     12            /* x-ul zonei de text (relativ la continut) */
#define NP_TY     52            /* y-ul zonei de text (sub toolbar) */
#define NP_MAX    16384

static char np_text[NP_MAX + 1];
static int  np_len;
static int  np_cur;             /* pozitia cursorului (offset in buffer) */
static int  np_sel = -1;        /* ancora selectiei (-1 = fara selectie) */
static int  np_top;             /* prima linie vizuala afisata (scroll) */
static int  np_mdrag;           /* selectie in curs cu mouse-ul */
static char np_name[24];
static int  np_dirty;           /* modificat de la ultima salvare? */
static int  np_input;           /* 0 nimic, 1 = "Deschide" nume, 2 = "Salveaza ca" */
static char np_ibuf[24];
static int  np_ilen;
static char np_stat[40];
static char np_titlebuf[40];
static int  np_vs[600];         /* inceputurile liniilor vizuale */
static int  np_vn;

/* clipboard global (partajat) */
static char clipboard[8192];
static int  clip_len;

/* [start, end) al selectiei curente (start<=end) */
static void np_sel_range(int *s, int *e)
{
    if (np_sel < 0 || np_sel == np_cur) {
        *s = *e = np_cur;
        return;
    }
    if (np_sel < np_cur) {
        *s = np_sel;
        *e = np_cur;
    } else {
        *s = np_cur;
        *e = np_sel;
    }
}

static void np_delete_range(int s, int e)
{
    if (e <= s)
        return;
    int n = e - s;
    for (int k = s; k < np_len - n; k++)
        np_text[k] = np_text[k + n];
    np_len -= n;
    np_cur = s;
    np_sel = -1;
    np_dirty = 1;
}

/* sterge selectia daca exista; intoarce 1 daca a sters ceva */
static int np_del_selection(void)
{
    int s, e;
    np_sel_range(&s, &e);
    if (e > s) {
        np_delete_range(s, e);
        return 1;
    }
    np_sel = -1;
    return 0;
}

static void np_copy(int cut)
{
    int s, e;
    np_sel_range(&s, &e);
    if (e <= s)
        return;
    int n = e - s;
    if (n > (int)sizeof(clipboard))
        n = sizeof(clipboard);
    for (int i = 0; i < n; i++)
        clipboard[i] = np_text[s + i];
    clip_len = n;
    if (cut)
        np_delete_range(s, e);
}

static void np_paste(void)
{
    np_del_selection();
    for (int i = 0; i < clip_len; i++) {
        if (np_len >= NP_MAX)
            break;
        for (int k = np_len; k > np_cur; k--)
            np_text[k] = np_text[k - 1];
        np_text[np_cur] = clipboard[i];
        np_len++;
        np_cur++;
    }
    np_sel = -1;
    np_dirty = 1;
}

static const char *np_title(void)
{
    int p = 0;
    const char *pre = "Notepad - ";
    while (*pre)
        np_titlebuf[p++] = *pre++;
    if (np_name[0])
        for (int i = 0; np_name[i] && p < 36; i++)
            np_titlebuf[p++] = np_name[i];
    else {
        const char *u = "fara nume";
        while (*u)
            np_titlebuf[p++] = *u++;
    }
    if (np_dirty)
        np_titlebuf[p++] = '*';
    np_titlebuf[p] = '\0';
    return np_titlebuf;
}

static void np_layout(void)
{
    np_vn = 0;
    np_vs[np_vn++] = 0;
    int col = 0;
    for (int i = 0; i < np_len && np_vn < 599; i++) {
        if (np_text[i] == '\n') {
            np_vs[np_vn++] = i + 1;
            col = 0;
        } else if (++col == NP_COLS) {
            np_vs[np_vn++] = i + 1;
            col = 0;
        }
    }
}

/* linia vizuala care contine offsetul `off` */
static int np_vrow_of(int off)
{
    int r = 0;
    for (int i = 0; i < np_vn; i++)
        if (np_vs[i] <= off)
            r = i;
        else
            break;
    return r;
}

static void np_scroll_to_cursor(void)
{
    int r = np_vrow_of(np_cur);
    if (r < np_top)
        np_top = r;
    if (r >= np_top + NP_ROWS)
        np_top = r - NP_ROWS + 1;
    if (np_top < 0)
        np_top = 0;
}

static void np_toolbar_btn(int i, int hov)
{
    static const char *lbl[3] = { "Nou", "Deschide", "Salveaza" };
    static const uint16_t *ic[3] = { ic_new, ic_open, ic_save };
    int cx = wins[NP_WIN].cx, cy = wins[NP_WIN].cy;
    int x = cx + 10 + i * 112, y = cy + 7;
    uint32_t bg = hov ? COL_CARD_HOV : COL_CARD;
    fb_fill_round2(x, y, 104, 30, 8, bg, 1, 0x22262E);
    icon16(x + 12, y + 7, ic[i], hov ? COL_ACCENT_HI : 0xBFC8D6);
    fb_text(x + 34, y + 7, lbl[i], COL_TEXT, bg);
}

static void np_content_draw(void)
{
    g_curwin = NP_WIN;
    int cx = wins[NP_WIN].cx, cy = wins[NP_WIN].cy;
    title_draw(NP_WIN);          /* titlul reflecta numele + starea "modificat" */
    np_layout();
    np_scroll_to_cursor();

    /* toolbar (bara de comenzi Fluent) */
    fb_fill(cx, cy, WCONT_W, 44, 0x22262E);
    fb_fill(cx, cy + 44, WCONT_W, 1, 0x14161B);
    for (int i = 0; i < 3; i++)
        np_toolbar_btn(i, hover_id == 40 + i);

    /* foaia de text (alba, ca Notepad) */
    fb_fill(cx, cy + 45, WCONT_W, WCONT_H - 45 - 24, 0xFBFCFE);

    int sel_s, sel_e;
    np_sel_range(&sel_s, &sel_e);

    /* desenam doar caracterele fiecarei linii vizuale */
    for (int row = 0; row < NP_ROWS && np_top + row < np_vn; row++) {
        int vr = np_top + row;
        int s = np_vs[vr];
        int e = (vr + 1 < np_vn) ? np_vs[vr + 1] : np_len;
        int tx = cx + NP_TX, ty = cy + NP_TY + row * 16;
        for (int i = s; i < e; i++) {
            char ch = np_text[i];
            if (ch == '\n') {
                if (i >= sel_s && i < sel_e)
                    fb_fill(tx, ty, 8, 16, 0xCDE7FF);   /* selectie peste EOL */
                break;
            }
            uint32_t bgc = (i >= sel_s && i < sel_e) ? 0xCDE7FF : 0xFBFCFE;
            fb_char(tx, ty, ch, 0x1A1A1A, bgc);
            tx += 8;
        }
    }

    /* Caretul: linia vizuala si coloana vin DIRECT din np_vrow_of, sursa
     * unica de adevar. Asta evita bugul in care cursorul de dupa un '\n'
     * aparea pe linia veche (la coloana = lungimea liniei). */
    int cur_vr = np_vrow_of(np_cur);
    if (cur_vr >= np_top && cur_vr < np_top + NP_ROWS) {
        int col = np_cur - np_vs[cur_vr];
        int curx = cx + NP_TX + col * 8;
        int cury = cy + NP_TY + (cur_vr - np_top) * 16;
        fb_fill(curx, cury, 2, 15, 0x2060D0);   /* caret vertical */
    }

    /* status bar */
    int sy = cy + WCONT_H - 24;
    fb_fill(cx, sy, WCONT_W, 24, 0x22262E);
    fb_fill(cx, sy, WCONT_W, 1, 0x14161B);
    if (np_input) {
        fb_text(cx + 8, sy + 3, np_input == 1 ? "Deschide: " : "Salveaza ca: ",
                COL_TEXT, 0x20242C);
        char tmp[26];
        int i;
        for (i = 0; i < np_ilen; i++)
            tmp[i] = np_ibuf[i];
        tmp[i++] = '_';
        tmp[i] = '\0';
        fb_text(cx + 8 + (np_input == 1 ? 80 : 104), sy + 3, tmp,
                0xFFE9A8, 0x20242C);
        fb_text(cx + WCONT_W - 186, sy + 3, "Enter=ok  Esc=anuleaza",
                COL_DIM, 0x20242C);
    } else {
        char line[40];
        int p = 0;
        const char *l1 = "linia ";
        while (*l1)
            line[p++] = *l1++;
        int ln = np_vrow_of(np_cur) + 1;
        char num[12];
        int t = 0;
        do {
            num[t++] = (char)('0' + ln % 10);
            ln /= 10;
        } while (ln);
        while (t--)
            line[p++] = num[t];
        line[p] = '\0';
        fb_text(cx + 8, sy + 3, line, COL_DIM, 0x20242C);
        if (np_stat[0])
            fb_text(cx + WCONT_W - (int)strlen(np_stat) * 8 - 10, sy + 3,
                    np_stat, 0x9FD49F, 0x20242C);
    }
}

static void np_set_stat(const char *s)
{
    int i = 0;
    for (; s[i] && i < 39; i++)
        np_stat[i] = s[i];
    np_stat[i] = '\0';
}

static void np_insert(char ch)
{
    if (np_len >= NP_MAX)
        return;
    for (int k = np_len; k > np_cur; k--)
        np_text[k] = np_text[k - 1];
    np_text[np_cur] = ch;
    np_len++;
    np_cur++;
    np_dirty = 1;
}

static void np_backspace(void)
{
    if (np_cur == 0)
        return;
    for (int k = np_cur - 1; k < np_len - 1; k++)
        np_text[k] = np_text[k + 1];
    np_len--;
    np_cur--;
    np_dirty = 1;
}

static void np_delete(void)
{
    if (np_cur >= np_len)
        return;
    for (int k = np_cur; k < np_len - 1; k++)
        np_text[k] = np_text[k + 1];
    np_len--;
    np_dirty = 1;
}

/* coloana vizuala a cursorului pe linia lui */
static int np_col_of(int off)
{
    return off - np_vs[np_vrow_of(off)];
}

static void np_move_vert(int dir)
{
    np_layout();
    int r = np_vrow_of(np_cur);
    int col = np_col_of(np_cur);
    int nr = r + dir;
    if (nr < 0 || nr >= np_vn)
        return;
    int s = np_vs[nr];
    int e = (nr + 1 < np_vn) ? np_vs[nr + 1] : np_len;
    int len = e - s;
    if (len > 0 && np_text[e - 1] == '\n')
        len--;                       /* nu trece de '\n' */
    np_cur = s + (col < len ? col : len);
}

static void np_load(const char *name)
{
    int i = 0;
    for (; name[i] && i < 23; i++)
        np_name[i] = name[i];
    np_name[i] = '\0';

    int64_t n = fs_read_into(name, np_text, NP_MAX);
    np_len = (n > 0) ? (int)n : 0;
    np_cur = 0;
    np_sel = -1;
    np_top = 0;
    np_dirty = 0;
    np_set_stat(n >= 0 ? "incarcat" : "fisier nou");
}

static void np_do_save(void)
{
    if (fs_is_protected(np_name)) {
        np_set_stat("fisier de sistem, protejat");
        return;
    }
    int len = np_len;
    if (len == 0) {
        np_text[0] = '\n';
        len = 1;
    }
    if (fs_save(np_name, np_text, (uint32_t)len) == 0) {
        np_dirty = 0;
        np_set_stat("salvat");
    } else {
        np_set_stat("eroare la salvare");
    }
}

/* Salveaza: daca nu are nume, cere unul ("Salveaza ca") */
static void np_save(void)
{
    if (np_name[0] == '\0') {
        np_input = 2;
        np_ilen = 0;
        return;
    }
    np_do_save();
}

/* deschidere din File Manager / taskbar */
static void np_open_window(const char *name)
{
    int wasvis = win_vis[NP_WIN];
    win_vis[NP_WIN] = 1;
    if (!wasvis)
        win_open_anim(NP_WIN);
    np_input = 0;
    np_stat[0] = '\0';
    if (name)
        np_load(name);
    focus_window(NP_WIN);
}

int gui_np_focused(void);
int gui_np_focused(void)
{
    return fb_active() && ready && fwin == NP_WIN && win_vis[NP_WIN];
}

/* tastatura pentru Notepad; intoarce 1 daca a consumat tasta */
static int np_key(char ch)
{
    unsigned char c = (unsigned char)ch;

    if (np_input) {
        if (c == '\n') {
            np_ibuf[np_ilen] = '\0';
            int mode = np_input;
            np_input = 0;
            if (np_ilen > 0) {
                if (mode == 1) {
                    np_load(np_ibuf);
                } else {
                    /* Salveaza ca: adopta numele si scrie */
                    int i = 0;
                    for (; np_ibuf[i] && i < 23; i++)
                        np_name[i] = np_ibuf[i];
                    np_name[i] = '\0';
                    np_do_save();
                }
            }
        } else if (c == 27) {
            np_input = 0;
        } else if (c == '\b') {
            if (np_ilen > 0)
                np_ilen--;
        } else if (c >= 32 && c <= 126 && np_ilen < 23) {
            np_ibuf[np_ilen++] = (char)c;
        }
        np_content_draw();
        return 1;
    }

    /* helper Home/End: intoarce inceputul/sfarsitul liniei vizuale a lui `off` */
#define NP_LINE_START(off) (np_layout(), np_vs[np_vrow_of(off)])

    if (c == 0x80) {                 /* sus */
        np_sel = -1;
        np_move_vert(-1);
    } else if (c == 0x81) {          /* jos */
        np_sel = -1;
        np_move_vert(1);
    } else if (c == 0x82) {          /* stanga */
        np_sel = -1;
        if (np_cur > 0)
            np_cur--;
    } else if (c == 0x83) {          /* dreapta */
        np_sel = -1;
        if (np_cur < np_len)
            np_cur++;
    } else if (c >= 0x90 && c <= 0x95) {   /* Shift+navigare = selectie */
        if (np_sel < 0)
            np_sel = np_cur;         /* ancoram selectia */
        if (c == 0x90)
            np_move_vert(-1);
        else if (c == 0x91)
            np_move_vert(1);
        else if (c == 0x92) {
            if (np_cur > 0)
                np_cur--;
        } else if (c == 0x93) {
            if (np_cur < np_len)
                np_cur++;
        } else if (c == 0x94) {
            np_cur = NP_LINE_START(np_cur);
        } else {
            np_layout();
            int r = np_vrow_of(np_cur);
            int e = (r + 1 < np_vn) ? np_vs[r + 1] : np_len;
            if (e > np_vs[r] && e <= np_len && np_text[e - 1] == '\n')
                e--;
            np_cur = e;
        }
    } else if (c == 0x84) {          /* Delete */
        if (!np_del_selection())
            np_delete();
    } else if (c == 0x85) {          /* Home */
        np_sel = -1;
        np_cur = NP_LINE_START(np_cur);
    } else if (c == 0x86) {          /* End */
        np_sel = -1;
        np_layout();
        {
            int r = np_vrow_of(np_cur);
            int e = (r + 1 < np_vn) ? np_vs[r + 1] : np_len;
            if (e > np_vs[r] && e <= np_len && np_text[e - 1] == '\n')
                e--;
            np_cur = e;
        }
    } else if (c == 0x01) {          /* Ctrl+A = selecteaza tot */
        np_sel = 0;
        np_cur = np_len;
    } else if (c == 0x03) {          /* Ctrl+C = copiaza */
        np_copy(0);
    } else if (c == 0x18) {          /* Ctrl+X = taie */
        np_copy(1);
    } else if (c == 0x16) {          /* Ctrl+V = lipeste */
        np_paste();
    } else if (c == 0x13) {          /* Ctrl+S = Salveaza */
        np_save();
    } else if (c == 0x0E) {          /* Ctrl+N = document nou */
        np_toolbar_action(0);
        return 1;
    } else if (c == 0x0F) {          /* Ctrl+O = Deschide */
        np_input = 1;
        np_ilen = 0;
    } else if (c == '\n') {
        np_del_selection();
        np_insert('\n');
    } else if (c == '\b') {
        if (!np_del_selection())
            np_backspace();
    } else if (c == '\t') {
        np_del_selection();
        np_insert(' ');
        np_insert(' ');
    } else if (c >= 32 && c <= 126) {
        np_del_selection();
        np_insert((char)c);
    } else {
        return 1;
    }
    np_content_draw();
    return 1;
#undef NP_LINE_START
}

static void np_toolbar_action(int i)
{
    if (i == 0) {                    /* Nou */
        np_len = 0;
        np_cur = 0;
        np_sel = -1;
        np_top = 0;
        np_name[0] = '\0';
        np_dirty = 0;
        np_set_stat("document nou");
        np_content_draw();
    } else if (i == 1) {             /* Deschide (dialog nume) */
        np_input = 1;
        np_ilen = 0;
        np_content_draw();
    } else if (i == 2) {             /* Salveaza */
        np_save();
        np_content_draw();
    }
}

/* muta cursorul la coordonatele ecran (mx,my) din zona de text */
static void np_cursor_at(int mx, int my)
{
    int cx = wins[NP_WIN].cx, cy = wins[NP_WIN].cy;
    int row = (my - (cy + NP_TY)) / 16;
    int col = (mx - (cx + NP_TX)) / 8;
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    int vr = np_top + row;
    if (vr >= np_vn)
        vr = np_vn - 1;
    if (vr < 0)
        vr = 0;
    int s = np_vs[vr];
    int e = (vr + 1 < np_vn) ? np_vs[vr + 1] : np_len;
    int len = e - s;
    if (len > 0 && np_text[e - 1] == '\n')
        len--;
    np_cur = s + (col < len ? col : len);
}

/* click in zona de text: pune cursorul + porneste o eventuala selectie */
static void np_content_click(int mx, int my)
{
    np_cursor_at(mx, my);
    np_sel = np_cur;        /* ancora; devine selectie daca se face drag */
    np_mdrag = 1;
    np_content_draw();
}

/* drag cu mouse-ul: extinde selectia de la ancora la pozitia curenta */
static void np_drag_to(int mx, int my)
{
    np_cursor_at(mx, my);
    np_content_draw();
}

/* ------------------------------------------------------------------ */
/* Task Manager (fereastra TM_WIN) — procese, CPU%, RAM, disc, locatie */

#define TM_ROWS 13

static int tm_sel = -1;

static const char *tm_state_name(int s)
{
    static const char *n[] = { "-", "gata", "ruleaza", "doarme", "moare" };
    return (s >= 0 && s <= 4) ? n[s] : "?";
}

static void tm_fmt(char *dst, uint32_t v)   /* numar zecimal */
{
    char t[12];
    int i = 0;
    do {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    int p = 0;
    while (i--)
        dst[p++] = t[i];
    dst[p] = '\0';
}

static void tm_content_draw(void)
{
    g_curwin = TM_WIN;
    int cx = wins[TM_WIN].cx, cy = wins[TM_WIN].cy;

    fb_fill(cx, cy, WCONT_W, WCONT_H, 0x1B1F26);

    /* antet cu totaluri de sistem */
    fb_fill(cx, cy, WCONT_W, 40, 0x22262E);
    int nproc = 0;
    uint32_t total_mem = 0, total_cpu = 0;
    struct task_info info;
    for (int i = 0; i < task_count_max(); i++)
        if (task_get_info(i, &info)) {
            nproc++;
            total_mem += info.mem_kb;
            total_cpu += info.cpu_pct;
        }
    char b[16], line[64];
    int p = 0;
    tm_fmt(b, (uint32_t)nproc);
    for (int i = 0; b[i]; i++) line[p++] = b[i];
    const char *s1 = " procese   CPU ";
    while (*s1) line[p++] = *s1++;
    if (total_cpu > 100) total_cpu = 100;
    tm_fmt(b, total_cpu);
    for (int i = 0; b[i]; i++) line[p++] = b[i];
    const char *s2 = "%   RAM procese ";
    while (*s2) line[p++] = *s2++;
    tm_fmt(b, total_mem);
    for (int i = 0; b[i]; i++) line[p++] = b[i];
    const char *s3 = " KiB";
    while (*s3) line[p++] = *s3++;
    line[p] = '\0';
    icon16(cx + 10, cy + 12, ic_chart, COL_ACCENT_HI);
    fb_text(cx + 34, cy + 12, line, COL_TEXT, 0x22262E);

    /* capul de tabel */
    int hy = cy + 44;
    fb_fill(cx, hy, WCONT_W, 20, 0x2A2F38);
    fb_text(cx + 12, hy + 2, "PID", COL_DIM, 0x2A2F38);
    fb_text(cx + 52, hy + 2, "Nume", COL_DIM, 0x2A2F38);
    fb_text(cx + 190, hy + 2, "Stare", COL_DIM, 0x2A2F38);
    fb_text(cx + 268, hy + 2, "CPU", COL_DIM, 0x2A2F38);
    fb_text(cx + 320, hy + 2, "RAM", COL_DIM, 0x2A2F38);
    fb_text(cx + 392, hy + 2, "Disc", COL_DIM, 0x2A2F38);
    fb_text(cx + 456, hy + 2, "Locatie", COL_DIM, 0x2A2F38);

    /* randurile */
    int ry = cy + 66;
    int row = 0;
    for (int i = 0; i < task_count_max() && row < TM_ROWS; i++) {
        if (!task_get_info(i, &info))
            continue;
        int y = ry + row * 22;
        int is_sel = (i == tm_sel);
        uint32_t bg = is_sel ? 0x2E5C9E : ((row & 1) ? 0x1F242C : 0x1B1F26);
        fb_fill_round2(cx + 6, y, WCONT_W - 12, 20, 6, bg, 1, 0x1B1F26);
        uint32_t fg = is_sel ? 0xFFFFFF : COL_TEXT;

        char n[12];
        tm_fmt(n, (uint32_t)i);
        fb_text(cx + 14, y + 2, n, fg, bg);
        fb_text(cx + 52, y + 2, info.name, fg, bg);
        fb_text(cx + 190, y + 2, tm_state_name(info.state),
                is_sel ? 0xD9E6F7 : COL_DIM, bg);
        tm_fmt(n, info.cpu_pct);
        int ln = (int)strlen(n);
        fb_text(cx + 300 - ln * 8, y + 2, n, fg, bg);
        fb_text(cx + 300, y + 2, "%", is_sel ? 0xD9E6F7 : COL_DIM, bg);
        tm_fmt(n, info.mem_kb);
        fb_text(cx + 320, y + 2, n, fg, bg);
        fb_text(cx + 320 + (int)strlen(n) * 8, y + 2, "K",
                is_sel ? 0xD9E6F7 : COL_DIM, bg);
        if (info.disk_kb) {
            tm_fmt(n, info.disk_kb);
            fb_text(cx + 392, y + 2, n, fg, bg);
            fb_text(cx + 392 + (int)strlen(n) * 8, y + 2, "K",
                    is_sel ? 0xD9E6F7 : COL_DIM, bg);
        } else {
            fb_text(cx + 392, y + 2, "-", is_sel ? 0xD9E6F7 : COL_DIM, bg);
        }
        fb_text(cx + 456, y + 2, info.file, is_sel ? 0xD9E6F7 : COL_DIM, bg);
        row++;
    }

    /* bara de jos cu butonul Kill */
    int by = cy + WCONT_H - 34;
    fb_fill(cx, by, WCONT_W, 34, 0x22262E);
    fb_fill(cx, by, WCONT_W, 1, 0x14161B);
    uint32_t kbg = (hover_id == 60) ? 0xB03A3A : 0x8A2E2E;
    fb_fill_round2(cx + WCONT_W - 150, by + 5, 140, 24, 10, kbg, 1, 0x22262E);
    fb_text(cx + WCONT_W - 120, by + 9, "Termina task", 0xFFE0E0, kbg);
}

/* randul de proces sub (mx,my) → id-ul task-ului, sau -1 */
static int tm_row_at(int mx, int my)
{
    g_curwin = TM_WIN;
    int cx = wins[TM_WIN].cx, cy = wins[TM_WIN].cy;
    int ry = cy + 66;
    if (my < ry || my >= ry + TM_ROWS * 22 ||
        mx < cx + 6 || mx >= cx + WCONT_W - 6)
        return -1;
    int visrow = (my - ry) / 22;
    int row = 0;
    struct task_info info;
    for (int i = 0; i < task_count_max(); i++) {
        if (!task_get_info(i, &info))
            continue;
        if (row == visrow)
            return i;
        row++;
    }
    return -1;
}

static void tm_content_click(int mx, int my)
{
    g_curwin = TM_WIN;
    int cx = wins[TM_WIN].cx, cy = wins[TM_WIN].cy;
    int by = cy + WCONT_H - 34;
    if (rect_hit(mx, my, cx + WCONT_W - 150, by + 5, 140, 24)) {
        if (tm_sel > 0 && task_kill_id(tm_sel) == 0)
            tm_sel = -1;
        tm_content_draw();
        return;
    }
    int id = tm_row_at(mx, my);
    if (id >= 0) {
        tm_sel = id;
        tm_content_draw();
    }
}

/* ------------------------------------------------------------------ */
/* Ceasul RTC (CMOS) */

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

/* actualizare periodica (o data pe secunda): ceas + Task Manager live */
void gui_refresh_taskmgr(void)
{
    if (fb_active() && ready && win_vis[TM_WIN])
        tm_content_draw();
}

void gui_clock(void)
{
    if (!fb_active() || !ready)
        return;

    uint8_t ss = bcd(cmos(0x00));
    uint8_t mm = bcd(cmos(0x02));
    uint8_t hh = bcd(cmos(0x04));

    char s[16];
    s[0] = (char)('0' + hh / 10); s[1] = (char)('0' + hh % 10); s[2] = ':';
    s[3] = (char)('0' + mm / 10); s[4] = (char)('0' + mm % 10); s[5] = ':';
    s[6] = (char)('0' + ss / 10); s[7] = (char)('0' + ss % 10);
    s[8] = ' '; s[9] = ' ';
    s[10] = 'F'; s[11] = (char)('1' + console_active()); s[12] = 0;

    fb_fill(W - 150, tby + 14, 144, 16, COL_TASKBAR);
    icon16(W - 150, tby + 14, ic_clock, COL_DIM);
    fb_text(W - 128, tby + 14, s, COL_DIM, COL_TASKBAR);
}

void gui_status_left(const char *s)
{
    if (!fb_active() || !ready)
        return;
    fb_fill(102, tby + 14, 10, 16, COL_TASKBAR);
    fb_text(102, tby + 14, s, 0x55627A, COL_TASKBAR);
}

void gui_status_right(const char *s)
{
    (void)s;
}

/* ------------------------------------------------------------------ */
/* Mouse: hover, click, focus, drag (context de IRQ) */

static int hit_taskbar(int x, int y)
{
    if (y < tby || y >= tby + TB_H)
        return -1;
    int sx = tb_start_x();
    if (x < sx || x >= sx + TB_N * TB_SLOT)
        return -1;
    int i = (x - sx) / TB_SLOT;
    if (i < 0 || i >= TB_N)
        return -1;
    return tb_id[i];               /* 10=Start,50=Terminal,FM/NP/TM/BR/SET_WIN */
}

/* butonul "Termina task" din Task Manager */
static int tm_hover_at(int x, int y)
{
    if (!win_vis[TM_WIN] || win_at(x, y) != TM_WIN)
        return -1;
    g_curwin = TM_WIN;
    int cx = wins[TM_WIN].cx, cy = wins[TM_WIN].cy;
    int by = cy + WCONT_H - 34;
    if (rect_hit(x, y, cx + WCONT_W - 150, by + 5, 140, 24))
        return 60;
    return -1;
}

/* id-uri de hover in interiorul Notepad-ului (doar cand e deasupra) */
static int np_hover_at(int x, int y)
{
    if (!win_vis[NP_WIN] || win_at(x, y) != NP_WIN)
        return -1;
    int cx = wins[NP_WIN].cx, cy = wins[NP_WIN].cy;
    for (int i = 0; i < 3; i++)
        if (rect_hit(x, y, cx + 10 + i * 112, cy + 7, 104, 30))
            return 40 + i;
    return -1;
}

static void clamp_win(int t, int *cx, int *cy)
{
    int cw = cwv(t), ch = chv(t);
    if (*cx < 4) *cx = 4;
    if (*cx > W - cw - 4) *cx = W - cw - 4;
    if (*cy < 34) *cy = 34;
    if (*cy > tby - ch - 10) *cy = tby - ch - 10;
}

/* ------------------------------------------------------------------ */
/* Setari (fereastra SET_WIN): sectiunile Display si Despre */

#define SET_SIDE_W 150
#define SET_BG     0x161A22

/* dreptunghiul unui buton de rezolutie (in panoul Display) */
static void set_btn_rect(int idx, int *x, int *y, int *w, int *h)
{
    int cx = wins[SET_WIN].cx, cy = wins[SET_WIN].cy;
    *x = cx + SET_SIDE_W + 30;
    *y = cy + 96 + idx * 50;
    *w = 300;
    *h = 42;
}

/* dreptunghiul unui tab din bara laterala (0 = Display, 1 = Despre) */
static void set_tab_rect(int idx, int *x, int *y, int *w, int *h)
{
    int cx = wins[SET_WIN].cx, cy = wins[SET_WIN].cy;
    *x = cx + 10;
    *y = cy + 60 + idx * 40;
    *w = SET_SIDE_W - 20;
    *h = 34;
}

static void set_btn_row(int idx, int hov)
{
    if (!win_vis[SET_WIN] || set_tab != 0)
        return;
    int bx, by, bw, bh;
    set_btn_rect(idx, &bx, &by, &bw, &bh);
    int cur   = (fb_width() == set_res[idx].w && fb_height() == set_res[idx].h);
    int avail = (set_res[idx].w <= fb_max_width() &&
                 set_res[idx].h <= fb_max_height());
    uint32_t bg = cur ? COL_ACCENT
                      : ((hov && avail) ? COL_CARD_HOV : COL_CARD);
    fb_fill_round2(bx, by, bw, bh, 8, bg, 1, SET_BG);
    uint32_t fg = cur ? 0xFFFFFF : (avail ? COL_TEXT : 0x5A6472);
    fb_text(bx + 16, by + (bh - 16) / 2, set_res[idx].label, fg, bg);
    if (cur)
        fb_text(bx + bw - 52, by + (bh - 16) / 2, "activ", 0xFFFFFF, bg);
    else if (!avail)
        fb_text(bx + bw - 96, by + (bh - 16) / 2, "indisponibil", 0x5A6472, bg);
}

static void set_tab_draw(int idx, int hov)
{
    if (!win_vis[SET_WIN])
        return;
    int bx, by, bw, bh;
    set_tab_rect(idx, &bx, &by, &bw, &bh);
    int active = (set_tab == idx);
    uint32_t bg = active ? COL_ACCENT : (hov ? COL_CARD_HOV : 0x12151C);
    fb_fill_round2(bx, by, bw, bh, 8, bg, 1, 0x12151C);
    icon16(bx + 8, by + 9, idx == 0 ? ic_chart : ic_gear,
           active ? 0xFFFFFF : COL_DIM);
    fb_text(bx + 34, by + 9, idx == 0 ? "Display" : "Despre",
            active ? 0xFFFFFF : COL_TEXT, bg);
}

/* panoul Display: alegerea rezolutiei */
static void set_panel_display(int cx, int cy)
{
    fb_text_scaled(cx + SET_SIDE_W + 30, cy + 24, "Display", COL_TEXT, 2);
    fb_fill(cx + SET_SIDE_W + 30, cy + 58, WCONT_W - SET_SIDE_W - 60, 1, 0x2C3542);
    fb_text(cx + SET_SIDE_W + 30, cy + 72, "Rezolutie ecran:", COL_DIM, SET_BG);
    for (int i = 0; i < 3; i++)
        set_btn_row(i, hover_id == 70 + i);
    fb_text(cx + SET_SIDE_W + 30, cy + WCONT_H - 34,
            "Rezolutia se aplica imediat (tastele 1/2/3).", 0x6B7A94, SET_BG);
}

/* panoul Despre: informatii sistem + dezvoltator */
static void set_panel_about(int cx, int cy)
{
    int px = cx + SET_SIDE_W + 30;
    fb_text_scaled(px, cy + 24, "Despre", COL_TEXT, 2);
    fb_fill(px, cy + 58, WCONT_W - SET_SIDE_W - 60, 1, 0x2C3542);

    fb_text_scaled(px, cy + 76, "DevOS", COL_ACCENT_HI, 3);
    fb_text(px, cy + 116, "Developer OS - sistem de operare x86-64", COL_TEXT, SET_BG);
    fb_text(px, cy + 138, "scris de la zero (bootloader, kernel, retea,", COL_DIM, SET_BG);
    fb_text(px, cy + 156, "TLS, motor JS, browser grafic).", COL_DIM, SET_BG);

    fb_fill(px, cy + 184, WCONT_W - SET_SIDE_W - 60, 1, 0x2C3542);
    fb_text(px, cy + 198, "Versiune:  v0.42", COL_DIM, SET_BG);
    fb_text(px, cy + 220, "Dezvoltat de:", COL_DIM, SET_BG);
    fb_text_scaled(px, cy + 240, "Gavrilencu Grigore", 0xF2C14E, 2);

    char num[24], line[48];
    int p;
    p = 0; const char *mm = "RAM liber:  ";
    while (*mm) line[p++] = *mm++;
    fmt_u(num, pmm_free_bytes() / (1024 * 1024));
    for (int i = 0; num[i]; i++) line[p++] = num[i];
    const char *mb = " MiB";
    for (int i = 0; mb[i]; i++) line[p++] = mb[i];
    line[p] = 0;
    fb_text(px, cy + 280, line, COL_DIM, SET_BG);

    p = 0; const char *up = "Uptime:     ";
    while (*up) line[p++] = *up++;
    fmt_u(num, pit_ticks() / 100);
    for (int i = 0; num[i]; i++) line[p++] = num[i];
    line[p++] = 's'; line[p] = 0;
    fb_text(px, cy + 300, line, COL_DIM, SET_BG);
}

static void set_content_draw(void)
{
    g_curwin = SET_WIN;
    int cx = wins[SET_WIN].cx, cy = wins[SET_WIN].cy;
    fb_fill(cx, cy, WCONT_W, WCONT_H, SET_BG);
    /* bara laterala */
    fb_fill(cx, cy, SET_SIDE_W, WCONT_H, 0x12151C);
    icon16(cx + 12, cy + 20, ic_gear, COL_ACCENT_HI);
    fb_text(cx + 36, cy + 20, "Setari", COL_TEXT, 0x12151C);
    for (int i = 0; i < 2; i++)
        set_tab_draw(i, hover_id == 74 + i);

    if (set_tab == 0)
        set_panel_display(cx, cy);
    else
        set_panel_about(cx, cy);
}

static int set_hover_at(int x, int y)
{
    if (!win_vis[SET_WIN] || win_at(x, y) != SET_WIN)
        return -1;
    for (int i = 0; i < 2; i++) {
        int bx, by, bw, bh;
        set_tab_rect(i, &bx, &by, &bw, &bh);
        if (rect_hit(x, y, bx, by, bw, bh))
            return 74 + i;
    }
    if (set_tab == 0)
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh;
            set_btn_rect(i, &bx, &by, &bw, &bh);
            if (rect_hit(x, y, bx, by, bw, bh))
                return 70 + i;
        }
    return -1;
}

static void gui_set_resolution(int w, int h)
{
    if (w == fb_width() && h == fb_height())
        return;
    if (!fb_set_mode(w, h))
        return;
    wall_prepared = 0;                 /* regenereaza fundalul la noua dimensiune */
    dims();                            /* actualizeaza W, H, tby */
    for (int i = 0; i < NWIN; i++) {
        if (wins[i].maxed) {           /* re-maximizeaza la noua rezolutie */
            wins[i].cx = 6;  wins[i].cy = 40;
            wins[i].cw = W - 12;  wins[i].ch = tby - 50;
        }
        clamp_win(i, &wins[i].cx, &wins[i].cy);
    }
    desktop_draw(0);                   /* wallpaper + taskbar */
    for (int i = 0; i < NWIN; i++)     /* ferestrele vizibile, de jos in sus */
        if (win_vis[zord[i]])
            win_draw(zord[i]);
    gui_clock();
}

static void set_content_click(int mx, int my)
{
    for (int i = 0; i < 2; i++) {       /* taburi */
        int bx, by, bw, bh;
        set_tab_rect(i, &bx, &by, &bw, &bh);
        if (rect_hit(mx, my, bx, by, bw, bh)) {
            if (set_tab != i) {
                set_tab = i;
                set_content_draw();
            }
            return;
        }
    }
    if (set_tab == 0)                   /* butoane rezolutie */
        for (int i = 0; i < 3; i++) {
            int bx, by, bw, bh;
            set_btn_rect(i, &bx, &by, &bw, &bh);
            if (rect_hit(mx, my, bx, by, bw, bh)) {
                gui_set_resolution(set_res[i].w, set_res[i].h);
                return;
            }
        }
}

void gui_pointer(int x, int y, int buttons)
{
    if (!fb_active() || !ready)
        return;

    cursor_hide();

    /* selectie in curs cu mouse-ul in Notepad? */
    if (np_mdrag) {
        if (!(buttons & 1))
            np_mdrag = 0;
        else
            np_drag_to(x, y);
        prev_btn = buttons;
        cursor_show(x, y);
        return;
    }

    /* fereastra in curs de tragere? */
    if (drag_term >= 0) {
        if (!(buttons & 1)) {
            drag_term = -1;
        } else {
            int nx = x - drag_dx, ny = y - drag_dy;
            clamp_win(drag_term, &nx, &ny);
            int t = drag_term;
            int ddx = nx - wins[t].cx;
            int ddy = ny - wins[t].cy;
            if (ddx > 1 || ddx < -1 || ddy > 1 || ddy < -1) {
                int ox, oy, ow, oh;
                win_full_rect(t, &ox, &oy, &ow, &oh);
                wins[t].cx = nx;
                wins[t].cy = ny;

                /* FARA flicker: fereastra se deseneaza INTAI la noua
                 * pozitie, apoi reparam doar fasiile ramase descoperite
                 * (vechiul dreptunghi minus cel nou). */
                win_draw(t);
                int bx = nx - 2, by = ny - 30;
                int sdx = bx - ox, sdy = by - oy;
                if (sdx > 0)
                    repaint_rect(ox, oy, sdx, oh, t);
                else if (sdx < 0)
                    repaint_rect(bx + ow, oy, -sdx, oh, t);
                if (sdy > 0)
                    repaint_rect(ox, oy, ow, sdy, t);
                else if (sdy < 0)
                    repaint_rect(ox, by + oh, ow, -sdy, t);
            }
        }
        prev_btn = buttons;
        cursor_show(x, y);
        return;
    }

    /* meniul contextual deschis: el primeste toate evenimentele */
    if (cm_open) {
        int it = cm_item_at(x, y);
        if (it != cm_hover) {
            cm_hover = it;
            cm_draw();
        }
        int lc = (buttons & 1) && !(prev_btn & 1);
        int rc = (buttons & 2) && !(prev_btn & 2);
        prev_btn = buttons;
        if (lc || rc) {
            if (it >= 0)
                cm_exec(it);
            else
                cm_close();
        }
        cursor_show(x, y);
        return;
    }

    int h = hit_taskbar(x, y);
    if (h < 0)
        h = menu_hover_at(x, y);
    if (h < 0)
        h = fm_hover_at(x, y);
    if (h < 0)
        h = np_hover_at(x, y);
    if (h < 0)
        h = tm_hover_at(x, y);
    if (h < 0)
        h = set_hover_at(x, y);
    if (h != hover_id) {
        int old = hover_id;
        hover_id = h;
        if (old == 10 || h == 10)
            start_button(hover_id == 10);
        if (old == 50 || h == 50)
            term_btn_draw(h == 50);
        if (old == FM_WIN || h == FM_WIN)
            fm_tb_draw(h == FM_WIN);
        if (old == NP_WIN || h == NP_WIN)
            np_tb_draw(h == NP_WIN);
        if (old == TM_WIN || h == TM_WIN)
            tm_btn_draw(h == TM_WIN);
        if (old == BR_WIN || h == BR_WIN)
            br_btn_draw(h == BR_WIN);
        if (old == SET_WIN || h == SET_WIN)
            set_tb_draw(h == SET_WIN);
        if (old >= 20 && old <= 24)
            fm_button(old - 20, 0);
        if (h >= 20 && h <= 24)
            fm_button(h - 20, 1);
        if (old >= 26 && old <= 28)
            fm_side_item(old - 26, 0);
        if (h >= 26 && h <= 28)
            fm_side_item(h - 26, 1);
        if (old == 30 || old == 31)
            fm_arrow(old - 30, 0);
        if (h == 30 || h == 31)
            fm_arrow(h - 30, 1);
        if (old >= 40 && old <= 42)
            np_toolbar_btn(old - 40, 0);
        if (h >= 40 && h <= 42)
            np_toolbar_btn(h - 40, 1);
        if ((old == 60 || h == 60) && win_vis[TM_WIN])
            tm_content_draw();     /* re-hover butonul Kill */
        if (old >= 70 && old <= 72)
            set_btn_row(old - 70, 0);
        if (h >= 70 && h <= 72)
            set_btn_row(h - 70, 1);
        if (old >= 74 && old <= 75)
            set_tab_draw(old - 74, 0);
        if (h >= 74 && h <= 75)
            set_tab_draw(h - 74, 1);
        if (menu_open) {
            if (old >= 80 && old <= 87)
                menu_item_draw(old - 80, 0);
            if (h >= 80 && h <= 87)
                menu_item_draw(h - 80, 1);
        }
    }

    int clicked  = (buttons & 1) && !(prev_btn & 1);
    int rclicked = (buttons & 2) && !(prev_btn & 2);
    prev_btn = buttons;

    /* click dreapta pe File Manager: meniul contextual */
    if (rclicked) {
        if (menu_open)
            menu_close();
        if (h == 50) {                 /* click dreapta pe butonul Terminal */
            cm_show(3, x + 2, y - 40);
            cursor_show(x, y);
            return;
        }
        if (win_vis[FM_WIN] && win_at(x, y) == FM_WIN) {
            if (fwin != FM_WIN)
                focus_window(FM_WIN);
            int kind = 2;
            int row = fm_row_at(x, y);
            if (row >= 0) {
                fm_sel = row;
                fm_content_draw();
                kind = 1;
            }
            cm_show(kind, x + 2, y + 2);
        }
        cursor_show(x, y);
        return;
    }

    if (clicked) {
        if (h == 50) {                 /* butonul Terminal */
            if (menu_open)
                menu_close();
            int cnt = gui_terminal_count();
            if (cnt == 0) {
                term_req = -2;         /* cere primul terminal (init il deschide) */
            } else {
                /* comuta la urmatorul terminal vizibil (cycle) */
                int start = (fwin >= 0 && fwin < CON_COUNT) ? fwin : -1;
                for (int k = 1; k <= CON_COUNT; k++) {
                    int c = (start + k) % CON_COUNT;
                    if (win_vis[c]) {
                        focus_window(c);
                        break;
                    }
                }
            }
        } else if (h == TM_WIN) {
            if (menu_open)
                menu_close();
            if (!win_vis[TM_WIN]) {
                win_vis[TM_WIN] = 1;
                win_open_anim(TM_WIN);
                focus_window(TM_WIN);
            } else if (fwin == TM_WIN)
                hide_win(TM_WIN, 0);
            else
                focus_window(TM_WIN);
        } else if (h == BR_WIN) {
            if (menu_open)
                menu_close();
            if (!win_vis[BR_WIN]) {
                win_vis[BR_WIN] = 1;
                win_open_anim(BR_WIN);
                focus_window(BR_WIN);
            } else if (fwin == BR_WIN)
                hide_win(BR_WIN, 0);
            else
                focus_window(BR_WIN);
        } else if (h == FM_WIN) {
            if (menu_open)
                menu_close();
            if (!win_vis[FM_WIN])
                fm_open_window();
            else if (fwin == FM_WIN)
                hide_win(FM_WIN, 0);  /* click pe butonul activ = minimizare */
            else
                focus_window(FM_WIN);
        } else if (h == NP_WIN) {
            if (menu_open)
                menu_close();
            if (!win_vis[NP_WIN])
                np_open_window(0);
            else if (fwin == NP_WIN)
                hide_win(NP_WIN, 0);
            else
                focus_window(NP_WIN);
        } else if (h == SET_WIN) {      /* iconul Setari din taskbar */
            if (menu_open)
                menu_close();
            gui_set_toggle();
        } else if (h >= 40 && h <= 42) {
            np_toolbar_action(h - 40);
        } else if (h >= 80 && h <= 87) {   /* element din meniul Start */
            menu_action(h - 80);
        } else if (h == 10) {
            if (menu_open)
                menu_close();
            else {
                menu_open = 1;
                menu_draw();
            }
        } else if ((h >= 20 && h <= 24) || (h >= 26 && h <= 28) ||
                   h == 30 || h == 31 || h == 32) {
            fm_action(h);
        } else {
            if (menu_open)
                menu_close();
            /* click pe o fereastra? (de sus in jos in z-order) */
            for (int i = NWIN - 1; i >= 0; i--) {
                int t = zord[i];
                if (!win_vis[t])
                    continue;
                int cx = wins[t].cx, cy = wins[t].cy;
                int frw = cwv(t) + 4, frh = chv(t) + 34;

                /* butoanele din titlu: rosu=inchide (+kill), galben=minimizeaza,
                 * verde=maximizeaza/restaureaza */
                if (rect_hit(x, y, cx + frw - 28, cy - 24, 16, 16)) {
                    close_win(t);
                    break;
                }
                if (rect_hit(x, y, cx + frw - 46, cy - 24, 16, 16)) {
                    hide_win(t, 0);
                    break;
                }
                if (rect_hit(x, y, cx + frw - 64, cy - 24, 16, 16)) {
                    toggle_maximize(t);
                    break;
                }

                if (rect_hit(x, y, cx - 2, cy - 30, frw, 28)) {
                    focus_window(t);        /* focus + ridicare */
                    if (!wins[t].maxed) {   /* ferestrele maximizate nu se trag */
                        drag_term = t;
                        drag_dx = x - wins[t].cx;
                        drag_dy = y - wins[t].cy;
                    }
                    break;
                }
                if (rect_hit(x, y, cx - 2, cy - 30, frw, frh)) {
                    if (t != fwin)
                        focus_window(t);
                    if (t == FM_WIN)
                        fm_content_click(x, y);
                    else if (t == NP_WIN &&
                             y >= cy + NP_TY && y < cy + chv(NP_WIN) - 22)
                        np_content_click(x, y);
                    else if (t == TM_WIN)
                        tm_content_click(x, y);
                    else if (t == BR_WIN) {
                        browser_click(cx, cy, x, y);
                        win_draw(BR_WIN);
                    }
                    else if (t == SET_WIN)
                        set_content_click(x, y);
                    break;
                }
            }
        }
    }

    cursor_show(x, y);
}
