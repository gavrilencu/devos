/* Browser web MyOS. Client HTTP peste clientul TCP din kernel (tcp.c),
 * parser HTML minimal cu layout (word-wrap), linkuri clicabile, istoric,
 * derulare. Reteaua ruleaza pe un fir de kernel (task_create) ca sa poata
 * astepta politicos (task_sleep) fara sa blocheze intreruperile. */

#include "browser.h"
#include "fb.h"
#include "netstack.h"
#include "tls.h"
#include "js.h"
#include "png.h"
#include "pmm.h"
#include "string.h"
#include "task.h"

/* ---- geometrie (continut 640x400, ca celelalte ferestre) ---- */
static int br_w = 640;             /* dimensiunea continutului (variabila: maximizare) */
static int br_h = 400;
#define BR_TOOL 40
#define BR_STAT 22
#define BR_PAGE_H (br_h - BR_TOOL - BR_STAT)
#define BR_LINE 18
#define BR_MARGIN 12
#define BR_RIGHT (br_w - BR_MARGIN - 14)   /* lasa loc de scrollbar */

/* ---- culori ---- */
#define TOOL_BG  0x2A2E36
#define BTN_BG   0x3A3F49
#define ACCENT   0x1284E4
#define PAGE_BG  0xFFFFFF
#define PAGE_FG  0x202124
#define LINK_FG  0x1A56DB
#define HEAD_FG  0x0B3D91
#define STAT_FG  0xB9C2CF

enum { ST_IDLE, ST_LOADING, ST_DONE, ST_ERR };
static volatile int state = ST_IDLE;

static char cur_url[300];
static char addr[300];
static int  addr_len;
static int  addr_focus;
static int  addr_caret;          /* pozitia cursorului in bara (0..addr_len) */
static int  addr_view;           /* primul caracter vizibil (derulare orizontala) */
static char status[110];

static char req_url[300];
static volatile int req_flag;

/* trimitere POST (formular): setate inainte de navigare, citite de firul de retea */
static volatile int req_post;      /* 1 = urmatoarea cerere e POST */
static char req_post_body[4096];
static int  req_post_len;

/* borcan de cookie simplu: cookie-urile unei singure gazde (sesiune/login) */
static char cookie_host[80];
static char cookie_jar[1024];

/* buffere mari alocate din PMM (identity-mapped), nu din kheap */
#define HTML_CAP (512 * 1024)
static char *html;
static int   html_len;

/* un "run" = o bucata de text / imagine / control de formular */
struct run {
    int y;                 /* y absolut in continut (varful) */
    short x, w, h;         /* pozitie + dimensiuni in pixeli */
    short link;            /* index in tabela de linkuri, sau -1 */
    short field;           /* index in tabela de campuri formular, sau -1 */
    unsigned char scale;   /* marimea fontului: 1,2,3 (x 8x16) */
    unsigned char bold;
    unsigned char align;   /* 0 stanga, 1 centru, 2 dreapta */
    unsigned char pad;
    unsigned int color;
    unsigned int bg;       /* fundal (0 = transparent) */
    uint32_t *img;         /* daca != 0: run imagine (pixeli 0xRRGGBB) */
    short iw, ih;          /* dimensiunile naturale ale imaginii */
    char text[32];
};
#define RUN_CAP 6000
static struct run *runs;
static int run_n;

/* --- formulare --- */
enum { F_TEXT, F_PASSWORD, F_SUBMIT, F_BUTTON, F_HIDDEN, F_TEXTAREA, F_CHECKBOX };
#define FIELD_CAP 64
#define FVAL_CAP 512
struct field {
    unsigned char type;
    int form;              /* index in forms[], sau -1 */
    char name[64];
    char label[40];        /* pt. butoane: textul */
    int checked;
};
static struct field fields[FIELD_CAP];
static int field_n;
static char fval[FIELD_CAP][FVAL_CAP];   /* valorile, persistente intre relayout-uri */
static int focused_field;                /* -1 = niciunul */

#define FORM_CAP 16
struct wform { char action[256]; int method; };  /* method: 0 GET, 1 POST */
static struct wform forms[FORM_CAP];
static int form_n;
static int cur_form;                     /* in timpul layout-ului */

#define LINK_CAP 600
#define HREF_MAX 192
static char (*links)[HREF_MAX];
static int link_n;

static int scroll;
static int content_h;

/* imagini: buffere alocate din PMM */
#define IMG_ARENA   (6 * 1024 * 1024)   /* pixeli decodati (bump, per pagina) */
#define IMG_SRC_CAP (320 * 1024)        /* valoarea atributului src (data: URI) */
#define IMG_BIN_CAP (512 * 1024)        /* fisierul PNG (base64-decodat / descarcat) */
#define IMG_SCR_CAP (768 * 1024)        /* scratch pentru png_decode */
static uint32_t *img_arena;
static int img_used;                    /* in pixeli */
static char *img_src;
static uint8_t *img_bin;
static uint8_t *img_scr;
static int img_count;                   /* limita de imagini per pagina */

/* istoric */
#define HIST_N 24
static char hist[HIST_N][300];
static int  hist_n, hist_cur;

static volatile int dirty;

/* --- stil calculat (CSS) --- */
struct style {
    unsigned int color;
    unsigned int bg;       /* 0 = transparent */
    unsigned char scale;   /* 1..3 */
    unsigned char bold;
    unsigned char align;   /* 0 stanga, 1 centru, 2 dreapta */
    unsigned char hidden;  /* display:none */
    unsigned char pre;     /* whitespace pastrat (pre) */
    int indent;            /* margine stanga (blockquote/li) */
};

/* reguli CSS extrase din <style> (selector simplu: tag, .class, #id) */
struct cssrule {
    char key[40];
    char decl[200];        /* declaratiile brute "prop:val;..." */
};
#define CSS_CAP 300
static struct cssrule *css;
static int css_n;

/* --- stare de layout (folosita doar de firul de retea) --- */
#define STK 48
static struct style stk[STK];
static int stkn;
static int  ly_x, ly_y, ly_link, ly_lineh;
static int  list_depth;
static int  list_num[16];      /* contor pt. <ol> pe nivel */
static int  list_ol[16];

/* ================= utilitare de string ================= */

static int ci_eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

static int has_scheme(const char *s)
{
    for (int i = 0; s[i] && i < 12; i++) {
        if (s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/') return 1;
        if (s[i] == '/') return 0;
    }
    return 0;
}

/* imparte un URL in gazda / cale / port; *tls=1 daca era https */
static void url_split(const char *url, char *host, int hcap,
                      char *path, int pcap, int *port, int *tls)
{
    *port = 80; *tls = 0;
    const char *p = url;
    if (ci_eq(p, "http://", 7)) p += 7;
    else if (ci_eq(p, "https://", 8)) { p += 8; *port = 443; *tls = 1; }
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < hcap - 1) host[i++] = *p++;
    host[i] = '\0';
    if (*p == ':') {
        p++;
        int pr = 0;
        while (*p >= '0' && *p <= '9') pr = pr * 10 + (*p++ - '0');
        if (pr > 0 && pr < 65536) *port = pr;
    }
    i = 0;
    if (*p == '/') while (*p && i < pcap - 1) path[i++] = *p++;
    path[i] = '\0';
    if (!path[0]) { path[0] = '/'; path[1] = '\0'; }
}

/* rezolva `href` fata de `base` -> `out` (URL absolut http) */
static void resolve_url(const char *base, const char *href, char *out, int cap)
{
    if (has_scheme(href)) {
        int i = 0; while (href[i] && i < cap - 1) out[i] = href[i], i++;
        out[i] = '\0';
        return;
    }
    /* scheme + host din base */
    char sh[300]; int n = 0;
    const char *p = base;
    if (ci_eq(p, "http://", 7)) { for (int k = 0; k < 7; k++) sh[n++] = p[k]; p += 7; }
    else if (ci_eq(p, "https://", 8)) { for (int k = 0; k < 8; k++) sh[n++] = p[k]; p += 8; }
    else { const char *h = "http://"; while (*h) sh[n++] = *h++; }
    /* gazda */
    while (*p && *p != '/') sh[n++] = *p++;
    sh[n] = '\0';

    int i = 0;
    for (int k = 0; sh[k] && i < cap - 1; k++) out[i++] = sh[k];
    if (href[0] == '/') {
        for (int k = 0; href[k] && i < cap - 1; k++) out[i++] = href[k];
    } else {
        /* cale relativa: base pana la ultimul '/' + href */
        int last = -1;
        for (int k = 0; base[k]; k++) if (base[k] == '/' && k >= (int)(p - base)) last = k;
        (void)last;
        out[i++] = '/';
        for (int k = 0; href[k] && i < cap - 1; k++) out[i++] = href[k];
    }
    out[i] = '\0';
}

/* ================= layout HTML + CSS ================= */

static char stk_tag[STK][12];

static struct style *cs(void) { return &stk[stkn > 0 ? stkn - 1 : 0]; }

/* --- parsare culori CSS: #rgb, #rrggbb, rgb(...), nume --- */
static int hexd(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static unsigned int parse_color(const char *v, int *ok)
{
    *ok = 0;
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '#') {
        v++;
        int d[8], nd = 0;
        while (nd < 8) { int h = hexd(v[nd]); if (h < 0) break; d[nd] = h; nd++; }
        if (nd >= 6) { *ok = 1; return (unsigned)((d[0]<<20)|(d[1]<<16)|(d[2]<<12)|(d[3]<<8)|(d[4]<<4)|d[5]); }
        if (nd >= 3) { *ok = 1; return (unsigned)((d[0]<<20)|(d[0]<<16)|(d[1]<<12)|(d[1]<<8)|(d[2]<<4)|d[2]); }
        return 0;
    }
    if (ci_eq(v, "rgb", 3)) {
        while (*v && *v != '(') v++;
        if (*v) v++;
        int c[3] = { 0, 0, 0 }, ci = 0;
        while (*v && *v != ')' && ci < 3) {
            while (*v == ' ' || *v == ',') v++;
            int val = 0, got = 0;
            while (*v >= '0' && *v <= '9') { val = val * 10 + (*v - '0'); v++; got = 1; }
            if (got) c[ci++] = val > 255 ? 255 : val;
            while (*v && *v != ',' && *v != ')') v++;
        }
        *ok = 1;
        return (unsigned)((c[0]<<16)|(c[1]<<8)|c[2]);
    }
    static const struct { const char *n; unsigned int c; } nm[] = {
        {"black",0x000000},{"white",0xffffff},{"red",0xe01010},{"green",0x108010},
        {"blue",0x1a56db},{"gray",0x808080},{"grey",0x808080},{"silver",0xc0c0c0},
        {"maroon",0x800000},{"yellow",0xf0d000},{"orange",0xf08000},{"purple",0x800080},
        {"navy",0x102060},{"teal",0x008080},{"lime",0x40c040},{"aqua",0x00b0b0},
        {"fuchsia",0xd000d0},{"olive",0x808000},{"darkblue",0x102a6b},
        {"lightgray",0xe0e0e0},{"lightgrey",0xe0e0e0},{"whitesmoke",0xf5f5f5},
        {"transparent",0},
    };
    for (unsigned i = 0; i < sizeof(nm)/sizeof(nm[0]); i++) {
        int L = (int)strlen(nm[i].n);
        if (ci_eq(v, nm[i].n, L) && (v[L]==0||v[L]==' '||v[L]==';'||v[L]=='!')) {
            *ok = 1; return nm[i].c;
        }
    }
    return 0;
}

static int map_fontsize(const char *v)
{
    while (*v == ' ') v++;
    if (*v >= '0' && *v <= '9') {
        int n = 0;
        while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; }
        if (*v == '.') { v++; while (*v >= '0' && *v <= '9') v++; }
        if (ci_eq(v, "em", 2) || ci_eq(v, "rem", 3))
            return n >= 2 ? 3 : (n >= 1 ? 2 : 1);
        if (n >= 30) return 3;
        if (n >= 20) return 2;
        return 1;
    }
    if (ci_eq(v, "xx-large", 8) || ci_eq(v, "x-large", 7)) return 3;
    if (ci_eq(v, "large", 5) || ci_eq(v, "larger", 6)) return 2;
    return 1;
}

/* citeste un atribut (class/id/style/href/color) dintr-un tag */
static void read_attr(const char *s, const char *attr, char *out, int cap)
{
    out[0] = '\0';
    int al = (int)strlen(attr);
    for (int i = 0; s[i] && s[i] != '>'; i++) {
        if ((i == 0 || s[i-1] == ' ' || s[i-1] == '\t') &&
            ci_eq(s + i, attr, al) &&
            (s[i+al] == '=' || s[i+al] == ' ')) {
            int j = i + al;
            while (s[j] == ' ') j++;
            if (s[j] != '=') continue;
            j++;
            while (s[j] == ' ') j++;
            char q = 0;
            if (s[j] == '"' || s[j] == '\'') q = s[j++];
            int o = 0;
            while (s[j] && s[j] != '>' && o < cap - 1) {
                if (q && s[j] == q) break;
                if (!q && (s[j] == ' ' || s[j] == '\t')) break;
                out[o++] = s[j++];
            }
            out[o] = '\0';
            return;
        }
    }
}

/* aplica un sir de declaratii "prop:val;..." peste un stil */
static void apply_decls(struct style *st, const char *d)
{
    while (*d) {
        while (*d == ' ' || *d == ';' || *d == '\n' || *d == '\t' || *d == '\r') d++;
        if (!*d || *d == '}') break;
        char prop[28]; int pl = 0;
        while (*d && *d != ':' && *d != ';' && *d != '}' && pl < 27) {
            char c = *d; if (c >= 'A' && c <= 'Z') c += 32; prop[pl++] = c; d++;
        }
        prop[pl] = '\0';
        if (*d != ':') { while (*d && *d != ';' && *d != '}') d++; continue; }
        d++;
        char val[72]; int vl = 0;
        while (*d && *d != ';' && *d != '}' && vl < 71) val[vl++] = *d++;
        val[vl] = '\0';
        char *vv = val; while (*vv == ' ') vv++;
        int ok;
        if (strcmp(prop, "color") == 0) {
            unsigned c = parse_color(vv, &ok); if (ok) st->color = c;
        } else if (strcmp(prop, "background") == 0 || strcmp(prop, "background-color") == 0) {
            if (ci_eq(vv, "transparent", 11) || ci_eq(vv, "none", 4)) st->bg = 0;
            else { unsigned c = parse_color(vv, &ok); if (ok) st->bg = c ? c : 0; }
        } else if (strcmp(prop, "font-size") == 0) {
            st->scale = (unsigned char)map_fontsize(vv);
        } else if (strcmp(prop, "font-weight") == 0) {
            st->bold = (unsigned char)(ci_eq(vv, "bold", 4) || (vv[0] >= '6' && vv[0] <= '9'));
        } else if (strcmp(prop, "text-align") == 0) {
            st->align = (unsigned char)(ci_eq(vv, "center", 6) ? 1 :
                        (ci_eq(vv, "right", 5) ? 2 : 0));
        } else if (strcmp(prop, "display") == 0) {
            if (ci_eq(vv, "none", 4)) st->hidden = 1;
        }
    }
}

/* extrage regulile din continutul unui bloc <style> */
static void parse_style_block(const char *s, int n)
{
    int i = 0;
    while (i < n && css_n < CSS_CAP) {
        while (i < n && (s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r')) i++;
        if (i + 1 < n && s[i] == '/' && s[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i]=='*' && s[i+1]=='/')) i++;
            i += 2; continue;
        }
        int selstart = i;
        while (i < n && s[i] != '{') i++;
        if (i >= n) break;
        int selend = i; i++;
        int decstart = i;
        while (i < n && s[i] != '}') i++;
        int decend = i;
        if (i < n) i++;
        int p = selstart;
        while (p < selend) {
            int q = p;
            while (q < selend && s[q] != ',') q++;
            int a = p; while (a < q && (s[a]==' '||s[a]=='\n'||s[a]=='\t'||s[a]=='\r')) a++;
            int b = q; while (b > a && (s[b-1]==' '||s[b-1]=='\n'||s[b-1]=='\t'||s[b-1]=='\r')) b--;
            int start = a;
            for (int k = a; k < b; k++)
                if (s[k]==' '||s[k]=='>'||s[k]=='+'||s[k]=='~') start = k + 1;
            int simple = 1;
            for (int k = start; k < b; k++)
                if (s[k]==':'||s[k]=='['||s[k]=='*'||s[k]==' ') simple = 0;
            int keylen = b - start;
            if (simple && keylen > 0 && keylen < 39 && css_n < CSS_CAP) {
                struct cssrule *r = &css[css_n];
                int kl = 0;
                for (int k = start; k < b; k++) {
                    char c = s[k]; if (c >= 'A' && c <= 'Z') c += 32; r->key[kl++] = c;
                }
                r->key[kl] = '\0';
                int dl = 0;
                for (int k = decstart; k < decend && dl < (int)sizeof(r->decl) - 1; k++)
                    r->decl[dl++] = s[k];
                r->decl[dl] = '\0';
                css_n++;
            }
            p = q + 1;
        }
    }
}

static int class_has(const char *cls, const char *name)
{
    int nl = (int)strlen(name);
    for (int i = 0; cls[i]; ) {
        while (cls[i] == ' ') i++;
        int j = i; while (cls[j] && cls[j] != ' ') j++;
        if (j - i == nl && ci_eq(cls + i, name, nl)) return 1;
        i = j;
    }
    return 0;
}

/* culorile implicite ale unei etichete */
static void tag_defaults(struct style *st, const char *n)
{
    if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && n[2] == 0) {
        st->bold = 1;
        st->scale = (n[1] == '1') ? 3 : (n[1] <= '3' ? 2 : 1);
    } else if (strcmp(n, "b") == 0 || strcmp(n, "strong") == 0 ||
               strcmp(n, "th") == 0) {
        st->bold = 1;
    } else if (strcmp(n, "a") == 0) {
        st->color = LINK_FG;
    } else if (strcmp(n, "small") == 0) {
        st->scale = 1;
    } else if (strcmp(n, "big") == 0) {
        st->scale = 2;
    } else if (strcmp(n, "center") == 0) {
        st->align = 1;
    } else if (strcmp(n, "blockquote") == 0) {
        st->indent += 24; st->color = 0x555555;
    } else if (strcmp(n, "ul") == 0 || strcmp(n, "ol") == 0 ||
               strcmp(n, "dl") == 0) {
        st->indent += 22;
    }
}

/* calculeaza stilul pentru o eticheta: mostenit + implicit + CSS + inline */
static void compute_style(struct style *st, const char *name, const char *tagstart)
{
    tag_defaults(st, name);

    char cls[160], id[64];
    read_attr(tagstart, "class", cls, sizeof(cls));
    read_attr(tagstart, "id", id, sizeof(id));

    for (int r = 0; r < css_n; r++) {
        const char *sel = css[r].key;
        int match = 0;
        if (sel[0] == '.') { if (cls[0] && class_has(cls, sel + 1)) match = 1; }
        else if (sel[0] == '#') { if (id[0] && strcmp(id, sel + 1) == 0) match = 1; }
        else if (strcmp(sel, name) == 0) match = 1;
        if (match) apply_decls(st, css[r].decl);
    }

    char inl[200];
    read_attr(tagstart, "style", inl, sizeof(inl));
    if (inl[0]) apply_decls(st, inl);
}

static void ly_newline(void)
{
    ly_x = BR_MARGIN + cs()->indent;
    ly_y += ly_lineh;
    ly_lineh = 20;
}

static void emit_word(const char *w, int len)
{
    if (len <= 0) return;
    struct style *st = cs();
    if (st->hidden) return;
    if (len > 31) len = 31;
    int sc = st->scale ? st->scale : 1;
    int cw = 8 * sc;
    int wpx = len * cw;
    int line_start = BR_MARGIN + st->indent;
    if (ly_x > line_start && ly_x + wpx > BR_RIGHT) ly_newline();
    int h = 16 * sc;
    if (h + 4 > ly_lineh) ly_lineh = h + 4;
    if (run_n < RUN_CAP) {
        struct run *r = &runs[run_n++];
        r->x = (short)ly_x;
        r->y = ly_y;
        r->w = (short)wpx;
        r->h = (short)h;
        r->scale = (unsigned char)sc;
        r->bold = st->bold;
        r->align = st->align;
        r->link = (short)ly_link;
        r->field = -1;
        r->color = st->color;
        r->bg = st->bg;
        r->img = 0;
        int c = 0;
        for (; c < len && c < 31; c++) r->text[c] = w[c];
        r->text[c] = '\0';
    }
    ly_x += wpx + (sc > 1 ? 4 * sc : 6);
}

/* decodeaza o entitate care incepe la &; scrie in *out un octet, intoarce
 * cate caractere din intrare au fost consumate (inclusiv & si ;). 0 = nu-i entitate */
static int decode_entity(const char *s, char *out)
{
    if (s[0] != '&') return 0;
    static const struct { const char *name; char ch; } ent[] = {
        { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' },
        { "apos;", '\'' }, { "nbsp;", ' ' }, { "#39;", '\'' }, { "copy;", 'c' },
        { "mdash;", '-' }, { "ndash;", '-' }, { "hellip;", '.' }, { "raquo;", '>' },
        { "laquo;", '<' }, { "rsquo;", '\'' }, { "lsquo;", '\'' },
        { "ldquo;", '"' }, { "rdquo;", '"' },
    };
    for (unsigned i = 0; i < sizeof(ent) / sizeof(ent[0]); i++) {
        int n = (int)strlen(ent[i].name);
        if (ci_eq(s + 1, ent[i].name, n)) { *out = ent[i].ch; return n + 1; }
    }
    return 0;
}

/* citeste numele tag-ului (litere) din `s` (dupa '<' sau '</') in `name` */
static int read_tag_name(const char *s, char *name, int cap)
{
    int i = 0;
    while (s[i] && s[i] != '>' && s[i] != ' ' && s[i] != '/' &&
           s[i] != '\t' && s[i] != '\n' && i < cap - 1) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        name[i] = c;
        i++;
    }
    name[i] = '\0';
    return i;
}

/* eticheta fara continut (nu deschide un context de stil) */
static int is_void_tag(const char *n)
{
    static const char *v[] = { "br", "hr", "img", "meta", "link", "input",
                               "source", "track", "area", "base", "col",
                               "embed", "param", "wbr" };
    for (unsigned i = 0; i < sizeof(v)/sizeof(v[0]); i++)
        if (strcmp(n, v[i]) == 0) return 1;
    return 0;
}

/* eticheta de tip bloc (rupe randul) */
static int is_block_tag(const char *n)
{
    static const char *b[] = { "p","div","h1","h2","h3","h4","h5","h6","li",
                               "tr","ul","ol","dl","dd","dt","table","thead",
                               "tbody","form","header","footer","section",
                               "article","nav","aside","blockquote","pre",
                               "figure","figcaption","main","address",
                               "fieldset","hr","center","title" };
    for (unsigned i = 0; i < sizeof(b)/sizeof(b[0]); i++)
        if (strcmp(n, b[i]) == 0) return 1;
    return 0;
}

static char page_title[80];

/* ---- imagini ---- */
static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int base64_decode(const char *s, uint8_t *out, int cap)
{
    int bits = 0, acc = 0, n = 0;
    for (; *s; s++) {
        if (*s == '=' || *s == '"' || *s == '>') break;
        int v = b64v(*s);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n < cap) out[n++] = (uint8_t)(acc >> bits); }
    }
    return n;
}

/* descarca continutul binar al unui URL in `out` (max cap); -1 la eroare */
static int fetch_url_binary(const char *url, uint8_t *out, int cap)
{
    task_sleep(60);      /* lasa conexiunea anterioara sa se inchida */
    char host[160], path[320]; int port, tls;
    url_split(url, host, sizeof(host), path, sizeof(path), &port, &tls);
    if (!host[0]) return -1;
    uint32_t ip = ip_parse_k(host);
    if (!ip) {
        dns_query(host);
        for (;;) { uint32_t r = dns_result(); if (r == (uint32_t)-1) { task_sleep(30); continue; } ip = r; break; }
    }
    if (!ip) return -1;
    int hc, g = 0;
    if (tls) { hc = tls_connect(ip, (uint16_t)port, host); if (hc < 0) return -1; }
    else {
        hc = tcp_connect(ip, (uint16_t)port);
        if (hc < 0) return -1;
        int st; while ((st = tcp_status(hc)) == 1) { if (++g > 500) { tcp_cclose(hc); return -1; } task_sleep(20); }
        if (st != 2) { tcp_cclose(hc); return -1; }
    }
    char req[600]; int q = 0;
    const char *gg = "GET "; while (*gg) req[q++] = *gg++;
    for (const char *s = path; *s; s++) req[q++] = *s;
    const char *m = " HTTP/1.0\r\nHost: "; while (*m) req[q++] = *m++;
    for (const char *s = host; *s; s++) req[q++] = *s;
    const char *e = "\r\nUser-Agent: DevOS-Browser/1.0\r\nConnection: close\r\n\r\n";
    while (*e) req[q++] = *e++;
    if (tls) tls_send(hc, req, q); else tcp_csend(hc, req, q);

    int tot = 0, idle = 0;
    for (;;) {
        int room = IMG_SCR_CAP - 1 - tot; if (room <= 0) break;
        int ch = room > 1400 ? 1400 : room;
        int r = tls ? tls_recv(hc, img_scr + tot, ch) : tcp_crecv(hc, img_scr + tot, ch);
        if (r > 0) { tot += r; idle = 0; continue; }
        if (tls) { if (r < 0) break; }
        else if (tcp_status(hc) == 0) { int r2 = tcp_crecv(hc, img_scr + tot, room); if (r2 > 0) { tot += r2; continue; } break; }
        if (++idle > 400) break;
        task_sleep(25);
    }
    if (tls) tls_close(hc); else tcp_cclose(hc);
    int body = 0;
    for (int k = 0; k + 3 < tot; k++)
        if (img_scr[k]=='\r'&&img_scr[k+1]=='\n'&&img_scr[k+2]=='\r'&&img_scr[k+3]=='\n') { body = k+4; break; }
    int blen = tot - body; if (blen > cap) blen = cap; if (blen < 0) blen = 0;
    memcpy(out, img_scr + body, blen);
    return blen;
}

/* decodeaza o imagine (data: URI PNG sau URL http/https catre PNG) */
static uint32_t *decode_image(const char *src, int *w, int *h)
{
    if (!img_arena || img_count >= 12) return 0;
    int binlen;
    if (ci_eq(src, "data:", 5)) {
        const char *comma = 0;
        for (const char *p = src; *p; p++) if (*p == ',') { comma = p + 1; break; }
        if (!comma) return 0;
        int isb64 = 0;
        for (const char *p = src; p < comma; p++) if (ci_eq(p, "base64", 6)) { isb64 = 1; break; }
        if (!isb64) return 0;
        binlen = base64_decode(comma, img_bin, IMG_BIN_CAP);
    } else {
        binlen = fetch_url_binary(src, img_bin, IMG_BIN_CAP);
    }
    if (binlen <= 8) return 0;
    uint32_t *px = img_arena + img_used;
    int avail = (IMG_ARENA / 4) - img_used;
    int iw, ih;
    if (png_decode(img_bin, binlen, px, avail, &iw, &ih, img_scr, IMG_SCR_CAP) != 0)
        return 0;
    img_used += iw * ih;
    img_count++;
    *w = iw; *h = ih;
    return px;
}

static void emit_image(uint32_t *px, int iw, int ih)
{
    struct style *st = cs();
    if (ly_x > BR_MARGIN + st->indent) ly_newline();
    int maxw = BR_RIGHT - (BR_MARGIN + st->indent);
    int dw = iw, dh = ih;
    if (dw > maxw && iw > 0) { dh = (int)((long)ih * maxw / iw); dw = maxw; }
    if (dh > 640 && dh > 0) { dw = (int)((long)dw * 640 / dh); dh = 640; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (run_n < RUN_CAP) {
        struct run *r = &runs[run_n++];
        r->x = (short)(BR_MARGIN + st->indent);
        r->y = ly_y; r->w = (short)dw; r->h = (short)dh;
        r->img = px; r->iw = (short)iw; r->ih = (short)ih;
        r->link = (short)ly_link; r->field = -1; r->scale = 1; r->bold = 0;
        r->align = st->align; r->color = 0; r->bg = 0; r->text[0] = 0;
    }
    ly_y += dh + 6;
    ly_x = BR_MARGIN + st->indent;
    ly_lineh = 20;
}

static int fields_init;    /* 1 = prima asezare a paginii (seteaza valorile initiale) */

static void emit_field(int type, const char *name, const char *initval,
                       const char *label, int checked)
{
    if (field_n >= FIELD_CAP) return;
    int idx = field_n++;
    struct field *f = &fields[idx];
    f->type = (unsigned char)type; f->form = cur_form; f->checked = checked;
    int i = 0; for (; name[i] && i < 63; i++) f->name[i] = name[i]; f->name[i] = 0;
    i = 0; for (; label && label[i] && i < 39; i++) f->label[i] = label[i]; f->label[i] = 0;
    if (fields_init) {   /* prima asezare: pune valoarea din atributul value= */
        int k = 0; for (; initval && initval[k] && k < FVAL_CAP - 1; k++) fval[idx][k] = initval[k];
        fval[idx][k] = 0;
    }
    if (type == F_HIDDEN) return;    /* invizibil, dar in tabela */

    struct style *st = cs();
    int wdt, hgt = 22;
    if (type == F_SUBMIT || type == F_BUTTON) {
        int L = (int)strlen(f->label[0] ? f->label : (type == F_SUBMIT ? "Trimite" : "Buton"));
        wdt = L * 8 + 20;
    } else if (type == F_TEXTAREA) { wdt = 320; hgt = 66; }
    else if (type == F_CHECKBOX) { wdt = 18; hgt = 18; }
    else wdt = 190;                  /* text / password */

    if (ly_x > BR_MARGIN + st->indent && ly_x + wdt > BR_RIGHT) ly_newline();
    if (hgt + 4 > ly_lineh) ly_lineh = hgt + 4;
    if (run_n < RUN_CAP) {
        struct run *r = &runs[run_n++];
        r->x = (short)ly_x; r->y = ly_y; r->w = (short)wdt; r->h = (short)hgt;
        r->field = (short)idx; r->link = -1; r->img = 0;
        r->scale = 1; r->bold = 0; r->align = 0; r->color = 0; r->bg = 0; r->text[0] = 0;
    }
    ly_x += wdt + 6;
}

/* aliniere pe linii: deplaseaza rulele cu align != 0 (post-procesare) */
static void align_pass(void)
{
    int i = 0;
    while (i < run_n) {
        int j = i;
        int y = runs[i].y;
        int right = 0, left = runs[i].x;
        while (j < run_n && runs[j].y == y) {
            int r = runs[j].x + runs[j].w;
            if (r > right) right = r;
            if (runs[j].x < left) left = runs[j].x;
            j++;
        }
        int al = runs[i].align;
        if (al == 1) {                        /* centru */
            int shift = (BR_MARGIN + BR_RIGHT - (right - left)) / 2 - left;
            if (shift > 0) for (int k = i; k < j; k++) runs[k].x += shift;
        } else if (al == 2) {                 /* dreapta */
            int shift = BR_RIGHT - right;
            if (shift > 0) for (int k = i; k < j; k++) runs[k].x += shift;
        }
        i = j;
    }
}

static void layout_html(const char *h, int n)
{
    run_n = 0; link_n = 0; css_n = 0; content_h = 0; page_title[0] = '\0';
    field_n = 0; form_n = 0; cur_form = -1;
    ly_x = BR_MARGIN; ly_y = BR_MARGIN; ly_link = -1; ly_lineh = 20;
    list_depth = 0;

    /* stilul de baza */
    stkn = 1;
    stk[0].color = PAGE_FG; stk[0].bg = 0; stk[0].scale = 1;
    stk[0].bold = 0; stk[0].align = 0; stk[0].hidden = 0; stk[0].pre = 0;
    stk[0].indent = 0;
    stk_tag[0][0] = '\0';

    /* prima trecere: extragem toate blocurile <style> pentru CSS */
    for (int k = 0; k + 6 < n; k++) {
        if (h[k] == '<' && ci_eq(h + k + 1, "style", 5) &&
            (h[k+6] == '>' || h[k+6] == ' ')) {
            int j = k + 6;
            while (j < n && h[j] != '>') j++;
            j++;
            int s0 = j;
            while (j + 7 < n && !(h[j] == '<' && ci_eq(h + j + 1, "/style", 6))) j++;
            parse_style_block(h + s0, j - s0);
            k = j;
        }
    }

    int i = 0;
    while (i < n) {
        char c = h[i];
        if (c == '<') {
            int close = (h[i + 1] == '/');
            const char *ts = h + i + (close ? 2 : 1);
            char name[16];
            read_tag_name(ts, name, sizeof(name));

            if (!close && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) {
                const char *endtag = (name[0] == 's' && name[1] == 'c') ? "/script" : "/style";
                int j = i + 1;
                while (j < n) {
                    if (h[j] == '<' && ci_eq(h + j + 1, endtag, (int)strlen(endtag))) break;
                    j++;
                }
                i = j;
                while (i < n && h[i] != '>') i++;
                i++;
                continue;
            }
            if (strcmp(name, "title") == 0 && !close) {
                int j = i; while (j < n && h[j] != '>') j++; j++;
                int t = 0;
                while (j < n && h[j] != '<' && t < (int)sizeof(page_title) - 1)
                    page_title[t++] = h[j++];
                page_title[t] = '\0';
                i = j; continue;
            }
            /* comentarii HTML */
            if (!close && h[i+1] == '!' && h[i+2] == '-' && h[i+3] == '-') {
                int j = i + 4;
                while (j + 2 < n && !(h[j]=='-'&&h[j+1]=='-'&&h[j+2]=='>')) j++;
                i = j + 3; continue;
            }

            int block = is_block_tag(name);
            int selfclose = 0;
            { int j = i; while (j < n && h[j] != '>') { if (h[j]=='/'&&h[j+1]=='>') selfclose=1; j++; } }

            if (block && (strcmp(name,"br")!=0)) {
                if (ly_x > BR_MARGIN + cs()->indent) ly_newline();
            }

            if (!close) {
                /* deschidere: calculam si punem stilul pe stiva */
                if (!is_void_tag(name) && !selfclose && stkn < STK) {
                    struct style ns = *cs();
                    compute_style(&ns, name, ts);
                    stk[stkn] = ns;
                    int tl = 0; for (; name[tl] && tl < 11; tl++) stk_tag[stkn][tl] = name[tl];
                    stk_tag[stkn][tl] = '\0';
                    stkn++;
                } else {
                    /* void/self-close: aplicam efectele imediate (ex. img alt) */
                }
                if (strcmp(name, "a") == 0) {
                    char href[HREF_MAX];
                    read_attr(ts, "href", href, sizeof(href));
                    if (href[0] && link_n < LINK_CAP) {
                        int k = 0;
                        for (; href[k] && k < HREF_MAX - 1; k++) links[link_n][k] = href[k];
                        links[link_n][k] = '\0';
                        ly_link = link_n++;
                    }
                }
                if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
                    if (list_depth < 15) {
                        list_depth++;
                        list_ol[list_depth] = (name[0] == 'o');
                        list_num[list_depth] = 0;
                    }
                }
                if (strcmp(name, "li") == 0) {
                    if (ly_x > BR_MARGIN + cs()->indent) ly_newline();
                    char b[8];
                    if (list_depth > 0 && list_ol[list_depth]) {
                        int num = ++list_num[list_depth];
                        int p = 0; char t[6]; int ti = 0;
                        do { t[ti++] = (char)('0'+num%10); num/=10; } while (num);
                        while (ti--) b[p++] = t[ti];
                        b[p++] = '.'; b[p] = 0;
                    } else { b[0] = '-'; b[1] = 0; }
                    emit_word(b, (int)strlen(b));
                }
                if (strcmp(name, "hr") == 0) {
                    if (ly_x > BR_MARGIN) ly_newline();
                    ly_y += 4;
                    if (run_n < RUN_CAP) {
                        struct run *r = &runs[run_n++];
                        r->x = BR_MARGIN; r->y = ly_y; r->w = BR_RIGHT - BR_MARGIN;
                        r->h = 1; r->scale = 1; r->bold = 0; r->align = 0;
                        r->link = -1; r->field = -1; r->color = 0xCCCCCC; r->bg = 0xCCCCCC;
                        r->img = 0; r->text[0] = '\0';
                    }
                    ly_y += 8; ly_x = BR_MARGIN + cs()->indent;
                }
                if (strcmp(name, "img") == 0 && img_src) {
                    read_attr(ts, "src", img_src, IMG_SRC_CAP);
                    int iw = 0, ih = 0;
                    uint32_t *px = img_src[0] ? decode_image(img_src, &iw, &ih) : 0;
                    if (px) emit_image(px, iw, ih);
                    else {
                        char alt[80]; read_attr(ts, "alt", alt, sizeof(alt));
                        if (alt[0]) {
                            emit_word("[img:", 5);
                            emit_word(alt, (int)strlen(alt));
                            emit_word("]", 1);
                        }
                    }
                }
                if (strcmp(name, "form") == 0) {
                    if (ly_x > BR_MARGIN) ly_newline();
                    if (form_n < FORM_CAP) {
                        cur_form = form_n++;
                        read_attr(ts, "action", forms[cur_form].action, sizeof(forms[cur_form].action));
                        char meth[8]; read_attr(ts, "method", meth, sizeof(meth));
                        forms[cur_form].method = (meth[0]=='p'||meth[0]=='P') ? 1 : 0;
                    }
                }
                if (strcmp(name, "input") == 0) {
                    char itype[16], iname[64], ival[FVAL_CAP];
                    read_attr(ts, "type", itype, sizeof(itype));
                    read_attr(ts, "name", iname, sizeof(iname));
                    read_attr(ts, "value", ival, sizeof(ival));
                    int ft = F_TEXT;
                    if (ci_eq(itype,"password",8)) ft = F_PASSWORD;
                    else if (ci_eq(itype,"submit",6)) ft = F_SUBMIT;
                    else if (ci_eq(itype,"button",6)) ft = F_BUTTON;
                    else if (ci_eq(itype,"hidden",6)) ft = F_HIDDEN;
                    else if (ci_eq(itype,"checkbox",8)) ft = F_CHECKBOX;
                    else if (ci_eq(itype,"radio",5)) ft = F_CHECKBOX;
                    int chk = 0;
                    for (const char *q = ts; *q && *q != '>'; q++)
                        if ((q[0]==' ') && ci_eq(q+1,"checked",7)) { chk = 1; break; }
                    const char *lbl = ft==F_SUBMIT ? (ival[0]?ival:"Trimite") : (ft==F_BUTTON?(ival[0]?ival:"Buton"):0);
                    emit_field(ft, iname, ival, lbl, chk);
                }
                if (strcmp(name, "textarea") == 0) {
                    char iname[64]; read_attr(ts, "name", iname, sizeof(iname));
                    /* valoarea = textul dintre <textarea> si </textarea> */
                    int j = i; while (j < n && h[j] != '>') j++; j++;
                    int s0 = j;
                    while (j + 10 < n && !(h[j]=='<' && ci_eq(h+j+1,"/textarea",9))) j++;
                    char ta[FVAL_CAP]; int tl = j - s0; if (tl > FVAL_CAP-1) tl = FVAL_CAP-1;
                    memcpy(ta, h + s0, tl); ta[tl] = 0;
                    emit_field(F_TEXTAREA, iname, ta, 0, 0);
                    i = j; while (i < n && h[i] != '>') i++;
                    /* lasam parserul sa consume </textarea> normal mai jos */
                }
                if (strcmp(name, "button") == 0) {
                    /* textul butonului = pana la </button> */
                    int j = i; while (j < n && h[j] != '>') j++; j++;
                    int s0 = j;
                    while (j + 8 < n && !(h[j]=='<' && ci_eq(h+j+1,"/button",7))) j++;
                    char lbl[40]; int tl = j - s0; if (tl > 39) tl = 39;
                    memcpy(lbl, h + s0, tl); lbl[tl] = 0;
                    char bt[16]; read_attr(ts, "type", bt, sizeof(bt));
                    int ft = ci_eq(bt,"button",6) ? F_BUTTON : F_SUBMIT;
                    char iname[64]; read_attr(ts, "name", iname, sizeof(iname));
                    emit_field(ft, iname, "", lbl[0]?lbl:"Trimite", 0);
                    i = j; while (i < n && h[i] != '>') i++;
                }
                if (strcmp(name, "p") == 0 || (name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&name[2]==0))
                    ly_y += 4;   /* spatiu inainte de paragraf/titlu */
            } else {
                /* inchidere: scoatem de pe stiva pana la eticheta */
                if (strcmp(name, "a") == 0) ly_link = -1;
                if (strcmp(name, "form") == 0) cur_form = -1;
                if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
                    if (list_depth > 0) list_depth--;
                }
                for (int s = stkn - 1; s >= 1; s--) {
                    if (strcmp(stk_tag[s], name) == 0) { stkn = s; break; }
                }
                if (block && strcmp(name,"br")!=0) {
                    if (ly_x > BR_MARGIN + cs()->indent) ly_newline();
                    if (strcmp(name,"p")==0 || (name[0]=='h'&&name[1]>='1'&&name[1]<='6'))
                        ly_y += 6;
                }
            }
            if (strcmp(name, "br") == 0) {
                if (ly_x > BR_MARGIN + cs()->indent) ly_newline();
                else ly_y += ly_lineh;
            }

            while (i < n && h[i] != '>') i++;
            i++;
        } else if (c == '&') {
            char out;
            int used = decode_entity(h + i, &out);
            if (used) {
                if (out == ' ') ly_x += 8;
                else { char w[2] = { out, 0 }; emit_word(w, 1); }
                i += used;
            } else { emit_word("&", 1); i++; }
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
        } else {
            char word[64];
            int wl = 0;
            while (i < n && wl < (int)sizeof(word) - 1) {
                char d = h[i];
                if (d == '<' || d == '&' || d == ' ' || d == '\t' ||
                    d == '\n' || d == '\r') break;
                word[wl++] = d;
                i++;
            }
            emit_word(word, wl);
        }
        if (run_n >= RUN_CAP) break;
    }
    content_h = ly_y + ly_lineh;
    align_pass();
}

/* ================= DOM + executie JavaScript ================= */

#define DOM_MAX 6000
struct dnode {
    char tag[20];
    int kind;              /* 0 element, 1 text, 2 raw-html, 3 style */
    char *text; int tlen;  /* text/raw/style continut, sau inner-override */
    int has_inner;
    char *attrs; int alen; /* atributele brute (in bufferul html) */
    char *ov_style; int ov_style_len;
    char *ov_class; int ov_class_len;
    char *extra; int elen; /* alte atribute din setAttribute */
    char id[48];
    int parent, child, sib, last;
};
static struct dnode *dnodes;
static int dnode_n;
static int dom_body_h;

#define DOM_SCR_CAP (512 * 1024)
static char *dom_scratch;
static int dom_scr_used;

#define JS_ARENA (8 * 1024 * 1024)
static char *js_arena;

#define SER_CAP (640 * 1024)
static char *ser_all;

struct scriptrec { const char *src; int len; };
static struct scriptrec scripts[128];
static int nscripts;

static char *dsalloc(int n)
{
    n = (n + 3) & ~3;
    if (dom_scr_used + n > DOM_SCR_CAP) return dom_scratch;  /* zona sigura */
    char *p = dom_scratch + dom_scr_used;
    dom_scr_used += n;
    return p;
}

static int dom_new(int kind)
{
    if (dnode_n >= DOM_MAX) return dnode_n - 1;
    int i = dnode_n++;
    struct dnode *d = &dnodes[i];
    d->tag[0] = 0; d->kind = kind; d->text = 0; d->tlen = 0; d->has_inner = 0;
    d->attrs = 0; d->alen = 0; d->ov_style = 0; d->ov_style_len = 0;
    d->ov_class = 0; d->ov_class_len = 0; d->extra = 0; d->elen = 0;
    d->id[0] = 0; d->parent = -1; d->child = -1; d->sib = -1; d->last = -1;
    return i;
}
static void dom_add_child(int p, int c)
{
    if (p < 0 || c < 0) return;
    dnodes[c].parent = p; dnodes[c].sib = -1;
    if (dnodes[p].last < 0) dnodes[p].child = c;
    else dnodes[dnodes[p].last].sib = c;
    dnodes[p].last = c;
}

static void dom_build(const char *h, int n)
{
    dnode_n = 0; nscripts = 0; dom_scr_used = 0; dom_body_h = 0;
    int root = dom_new(0);
    strcpy(dnodes[root].tag, "#root");
    int stack[128]; int sp = 0; stack[sp++] = root;
    int cur = root;
    int i = 0;
    while (i < n && dnode_n < DOM_MAX - 2) {
        if (h[i] == '<') {
            if (h[i+1] == '!') {
                if (h[i+2]=='-'&&h[i+3]=='-') { i+=4; while (i+2<n && !(h[i]=='-'&&h[i+1]=='-'&&h[i+2]=='>')) i++; i+=3; }
                else { while (i<n && h[i]!='>') i++; i++; }
                continue;
            }
            if (h[i+1] == '/') {
                char name[20]; read_tag_name(h+i+2, name, sizeof(name));
                for (int s = sp-1; s >= 1; s--)
                    if (strcmp(dnodes[stack[s]].tag, name) == 0) { sp = s; cur = stack[s-1]; break; }
                while (i<n && h[i]!='>') i++;
                i++;
                continue;
            }
            char name[20]; read_tag_name(h+i+1, name, sizeof(name));
            int nl = (int)strlen(name);
            int ta = i + 1 + nl;
            int gt = i; while (gt<n && h[gt]!='>') gt++;
            int selfclose = gt>i && h[gt-1]=='/';
            if (strcmp(name,"script")==0) {
                int j = gt+1, s0 = j;
                while (j<n && !(h[j]=='<' && ci_eq(h+j+1,"/script",7))) j++;
                if (nscripts<128) { scripts[nscripts].src=h+s0; scripts[nscripts].len=j-s0; nscripts++; }
                i=j; while (i<n && h[i]!='>') i++; i++; continue;
            }
            if (strcmp(name,"style")==0) {
                int j = gt+1, s0 = j;
                while (j<n && !(h[j]=='<' && ci_eq(h+j+1,"/style",6))) j++;
                int node = dom_new(3); strcpy(dnodes[node].tag,"style");
                dnodes[node].text=(char*)(h+s0); dnodes[node].tlen=j-s0; dnodes[node].has_inner=1;
                dom_add_child(cur,node);
                i=j; while (i<n && h[i]!='>') i++; i++; continue;
            }
            int node = dom_new(0);
            for (int k=0;k<nl&&k<19;k++) dnodes[node].tag[k]=name[k];
            dnodes[node].tag[nl<19?nl:19]=0;
            dnodes[node].attrs=(char*)(h+ta);
            dnodes[node].alen=(selfclose?gt-1:gt)-ta;
            read_attr(h+ta, "id", dnodes[node].id, sizeof(dnodes[node].id));
            dom_add_child(cur,node);
            if (strcmp(name,"body")==0) dom_body_h=node;
            if (!selfclose && !is_void_tag(name) && sp<128) { stack[sp++]=node; cur=node; }
            i=gt+1;
        } else {
            int j=i; while (j<n && h[j]!='<') j++;
            int node = dom_new(1);
            dnodes[node].text=(char*)(h+i); dnodes[node].tlen=j-i;
            dom_add_child(cur,node);
            i=j;
        }
    }
    if (dom_body_h==0) dom_body_h=root;
}

/* serializare arbore -> HTML */
static char *ser_buf; static int ser_pos, ser_cap;
static void ser_emit(const char *s, int n) { for (int i=0;i<n&&ser_pos<ser_cap-1;i++) ser_buf[ser_pos++]=s[i]; }
static void ser_node(int idx)
{
    if (idx < 0) return;
    struct dnode *d = &dnodes[idx];
    if (d->kind == 1 || d->kind == 2) { ser_emit(d->text, d->tlen); return; }
    if (d->kind == 3) { ser_emit("<style>",7); ser_emit(d->text,d->tlen); ser_emit("</style>",8); return; }
    if (idx != 0) {
        ser_emit("<",1); ser_emit(d->tag,(int)strlen(d->tag));
        if (d->ov_class) { ser_emit(" class=\"",8); ser_emit(d->ov_class,d->ov_class_len); ser_emit("\"",1); }
        if (d->ov_style) { ser_emit(" style=\"",8); ser_emit(d->ov_style,d->ov_style_len); ser_emit("\"",1); }
        if (d->extra) ser_emit(d->extra,d->elen);
        if (d->attrs && d->alen) { ser_emit(" ",1); ser_emit(d->attrs,d->alen); }
        ser_emit(">",1);
    }
    if (d->has_inner) ser_emit(d->text,d->tlen);
    else for (int c=d->child;c>=0;c=dnodes[c].sib) ser_node(c);
    if (idx != 0 && !is_void_tag(d->tag)) { ser_emit("</",2); ser_emit(d->tag,(int)strlen(d->tag)); ser_emit(">",1); }
}

/* ---- callback-uri DOM pentru motorul JS ---- */
static int br_getid(void *ud, const char *id)
{
    (void)ud;
    for (int i=0;i<dnode_n;i++)
        if (dnodes[i].kind==0 && dnodes[i].id[0] && strcmp(dnodes[i].id,id)==0) return i;
    return -1;
}
static int br_create(void *ud, const char *tag)
{
    (void)ud;
    int node = dom_new(0);
    int k=0; for (;tag[k]&&k<19;k++) dnodes[node].tag[k]=tag[k]; dnodes[node].tag[k]=0;
    return node;
}
static void br_append(void *ud, int p, int c) { (void)ud; if (p>=0&&p<dnode_n&&c>=0&&c<dnode_n) dom_add_child(p,c); }
static void br_setinner(void *ud, int h, const char *s, int n)
{
    (void)ud; if (h<0||h>=dnode_n) return;
    char *p = dsalloc(n+1); memcpy(p,s,n); p[n]=0;
    dnodes[h].text=p; dnodes[h].tlen=n; dnodes[h].has_inner=1;
}
static void br_settext(void *ud, int h, const char *s, int n)
{
    (void)ud; if (h<0||h>=dnode_n) return;
    /* escapam < > & ca sa fie text literal */
    char *p = dsalloc(n*5+1); int o=0;
    for (int i=0;i<n;i++) {
        if (s[i]=='<'){memcpy(p+o,"&lt;",4);o+=4;}
        else if (s[i]=='>'){memcpy(p+o,"&gt;",4);o+=4;}
        else if (s[i]=='&'){memcpy(p+o,"&amp;",5);o+=5;}
        else p[o++]=s[i];
    }
    p[o]=0; dnodes[h].text=p; dnodes[h].tlen=o; dnodes[h].has_inner=1;
}
static int br_getinner(void *ud, int h, char *buf, int cap)
{
    (void)ud; if (h<0||h>=dnode_n) return 0;
    if (dnodes[h].has_inner) { int n=dnodes[h].tlen; if(n>cap-1)n=cap-1; memcpy(buf,dnodes[h].text,n); return n; }
    char *sb=ser_buf; int sp=ser_pos, sc=ser_cap;
    ser_buf=buf; ser_pos=0; ser_cap=cap;
    for (int c=dnodes[h].child;c>=0;c=dnodes[c].sib) ser_node(c);
    int r=ser_pos;
    ser_buf=sb; ser_pos=sp; ser_cap=sc;
    return r;
}
static void br_setattr(void *ud, int h, const char *k, const char *v)
{
    (void)ud; if (h<0||h>=dnode_n) return;
    int vl=(int)strlen(v);
    if (strcmp(k,"style")==0) { char*p=dsalloc(vl+1); memcpy(p,v,vl); p[vl]=0; dnodes[h].ov_style=p; dnodes[h].ov_style_len=vl; return; }
    if (strcmp(k,"class")==0) { char*p=dsalloc(vl+1); memcpy(p,v,vl); p[vl]=0; dnodes[h].ov_class=p; dnodes[h].ov_class_len=vl; return; }
    if (strcmp(k,"id")==0) { int m=vl; if(m>47)m=47; memcpy(dnodes[h].id,v,m); dnodes[h].id[m]=0; return; }
    /* alt atribut: adaugam in extra */
    if (!dnodes[h].extra) { dnodes[h].extra=dsalloc(256); dnodes[h].elen=0; }
    int kl=(int)strlen(k);
    if (dnodes[h].elen + kl + vl + 5 < 256) {
        char *e=dnodes[h].extra; int o=dnodes[h].elen;
        e[o++]=' '; memcpy(e+o,k,kl); o+=kl; e[o++]='='; e[o++]='"';
        memcpy(e+o,v,vl); o+=vl; e[o++]='"'; dnodes[h].elen=o;
    }
}
static void br_write(void *ud, const char *s, int n)
{
    (void)ud;
    char *p = dsalloc(n+1); memcpy(p,s,n); p[n]=0;
    int node = dom_new(2); dnodes[node].text=p; dnodes[node].tlen=n;
    dom_add_child(dom_body_h, node);
}
static void br_jslog(void *ud, const char *s, int n) { (void)ud;(void)s;(void)n; }

static struct js_dom_ops g_ops = {
    0, br_write, br_getid, br_create, br_append,
    br_setinner, br_getinner, br_settext, br_setattr, br_jslog, 0
};

static void run_scripts(void)
{
    if (nscripts == 0 || !js_arena) return;
    g_ops.body = dom_body_h;
    JS *J = js_create(js_arena, JS_ARENA);
    js_set_dom(J, &g_ops);
    js_reset(J);                   /* re-instaleaza globalele CU dom setat */
    for (int i = 0; i < nscripts; i++)
        js_eval(J, scripts[i].src, scripts[i].len);
}

/* construieste DOM, ruleaza scripturile, apoi aseaza pagina finala */
static int ser_len;    /* lungimea HTML-ului serializat (pentru relayout) */

static void render_page(const char *h, int n)
{
    img_used = 0; img_count = 0;
    /* pagina noua: golim valorile campurilor si focusul */
    for (int i = 0; i < FIELD_CAP; i++) fval[i][0] = 0;
    focused_field = -1;
    fields_init = 1;
    if (!dnodes || !ser_all) { ser_len = 0; layout_html(h, n); return; }
    dom_build(h, n);
    run_scripts();
    ser_buf = ser_all; ser_pos = 0; ser_cap = SER_CAP;
    ser_node(0);
    ser_all[ser_pos] = 0;
    ser_len = ser_pos;
    layout_html(ser_all, ser_pos);
}

/* reaseaza pagina din HTML-ul serializat cache-uit, PASTRAND valorile
 * campurilor (nu re-ruleaza JavaScript). Folosit dupa editarea unui camp. */
static void relayout(void)
{
    if (ser_len <= 0) return;
    fields_init = 0;
    layout_html(ser_all, ser_len);
    dirty = 1;
}

/* schimba dimensiunea zonei de continut (maximizare/restaurare/rezolutie)
 * si reface aranjarea paginii ca textul sa se re-incadreze pe noua latime. */
void browser_set_size(int w, int h)
{
    if (w < 320) w = 320;
    if (h < 200) h = 200;
    if (w == br_w && h == br_h)
        return;
    br_w = w;
    br_h = h;
    if (ser_len > 0)
        layout_html(ser_all, ser_len);   /* re-incadreaza pe latimea noua */
    int max = content_h - BR_PAGE_H;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
    dirty = 1;
}

/* ================= retea: descarcare HTTP ================= */

/* gaseste un antet (case-insensitive) in sectiunea de antete; copiaza valoarea */
static int find_header(const char *resp, int len, const char *name, char *out, int cap)
{
    int nl = (int)strlen(name);
    int i = 0;
    /* limita: pana la corpul \r\n\r\n */
    while (i < len - 3) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' &&
            resp[i + 2] == '\r' && resp[i + 3] == '\n') break;
        /* inceput de linie? */
        if (i == 0 || (resp[i - 1] == '\n')) {
            if (ci_eq(resp + i, name, nl)) {
                int j = i + nl;
                while (resp[j] == ' ' || resp[j] == ':') j++;
                int o = 0;
                while (j < len && resp[j] != '\r' && resp[j] != '\n' && o < cap - 1)
                    out[o++] = resp[j++];
                out[o] = '\0';
                return 1;
            }
        }
        i++;
    }
    return 0;
}

static int parse_status(const char *resp, int len)
{
    /* "HTTP/1.x CODE ..." */
    int i = 0;
    while (i < len && resp[i] != ' ') i++;
    while (i < len && resp[i] == ' ') i++;
    int code = 0;
    while (i < len && resp[i] >= '0' && resp[i] <= '9') code = code * 10 + (resp[i++] - '0');
    return code;
}

/* descarca un URL in `html`/`html_len`; intoarce codul HTTP sau -1 la eroare
 * de retea. Nu urmareste redirectari (o face apelantul). */
/* extrage cookie-urile (Set-Cookie) din raspuns si le pastreaza pt. gazda */
static void store_cookies(const char *resp, int len, const char *host)
{
    int hend = len;
    for (int i = 0; i + 3 < len; i++)
        if (resp[i]=='\r'&&resp[i+1]=='\n'&&resp[i+2]=='\r'&&resp[i+3]=='\n') { hend = i; break; }
    int found = 0;
    for (int i = 0; i < hend; i++) {
        if ((i == 0 || resp[i-1] == '\n') && ci_eq(resp + i, "set-cookie:", 11)) {
            int j = i + 11; while (resp[j] == ' ') j++;
            char pair[256]; int p = 0;
            while (j < hend && resp[j]!=';' && resp[j]!='\r' && resp[j]!='\n' && p < 255)
                pair[p++] = resp[j++];
            pair[p] = 0;
            if (p > 0) {
                if (!found) {
                    int k = 0; for (; host[k] && k < 79; k++) cookie_host[k] = host[k];
                    cookie_host[k] = 0; cookie_jar[0] = 0; found = 1;
                }
                int jl = (int)strlen(cookie_jar);
                if (jl > 0 && jl < 1000) { cookie_jar[jl++] = ';'; cookie_jar[jl++] = ' '; cookie_jar[jl] = 0; }
                jl = (int)strlen(cookie_jar);
                for (int kk = 0; pair[kk] && jl < 1022; kk++) cookie_jar[jl++] = pair[kk];
                cookie_jar[jl] = 0;
            }
        }
    }
}

static int http_get(const char *url, int post, const char *body, int blen)
{
    char host[160], path[320];
    int port, tls;
    url_split(url, host, sizeof(host), path, sizeof(path), &port, &tls);
    if (!host[0]) return -1;

    /* rezolvare */
    strcpy(status, "Rezolv ");
    strcat(status, host);
    strcat(status, " ...");
    dirty = 1;

    uint32_t ip = ip_parse_k(host);
    if (!ip) {
        dns_query(host);
        for (;;) {
            uint32_t r = dns_result();
            if (r == (uint32_t)-1) { task_sleep(30); continue; }
            ip = r;
            break;
        }
    }
    if (!ip) { strcpy(status, "Nu pot rezolva gazda."); return -1; }

    strcpy(status, tls ? "Conectare TLS ..." : "Conectare ...");
    dirty = 1;

    int hc;
    if (tls) {
        hc = tls_connect(ip, (uint16_t)port, host);
        if (hc < 0) { strcpy(status, "Handshake TLS esuat (sau site TLS 1.3)."); return -1; }
    } else {
        hc = tcp_connect(ip, (uint16_t)port);
        if (hc < 0) { strcpy(status, "Fara conexiuni libere."); return -1; }
        int st;
        while ((st = tcp_status(hc)) == 1) task_sleep(25);
        if (st != 2) { strcpy(status, "Conectare esuata."); return -1; }
    }

    /* cerere */
    char req[900];
    int p = 0;
    const char *g = post ? "POST " : "GET ";
    while (*g) req[p++] = *g++;
    for (const char *s = path; *s; s++) req[p++] = *s;
    const char *m = " HTTP/1.0\r\nHost: "; while (*m) req[p++] = *m++;
    for (const char *s = host; *s; s++) req[p++] = *s;
    const char *ua = "\r\nUser-Agent: DevOS-Browser/1.0\r\nAccept: text/html\r\n";
    while (*ua) req[p++] = *ua++;
    /* cookie pentru gazda (sesiune) */
    if (cookie_jar[0] && ci_eq(cookie_host, host, (int)strlen(cookie_host)+1)) {
        const char *ck = "Cookie: "; while (*ck) req[p++] = *ck++;
        for (const char *s = cookie_jar; *s && p < 800; s++) req[p++] = *s;
        req[p++] = '\r'; req[p++] = '\n';
    }
    if (post) {
        const char *ct = "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
        while (*ct) req[p++] = *ct++;
        char nb[12]; int ni = 0, bl = blen; if (bl==0) nb[ni++]='0'; else { char t[12]; int ti=0; while(bl){t[ti++]=(char)('0'+bl%10);bl/=10;} while(ti)nb[ni++]=t[--ti]; } nb[ni]=0;
        for (int k = 0; k < ni; k++) req[p++] = nb[k];
        req[p++]='\r'; req[p++]='\n';
    }
    const char *e = "Connection: close\r\n\r\n";
    while (*e) req[p++] = *e++;
    if (post && body) for (int k = 0; k < blen && p < (int)sizeof(req); k++) req[p++] = body[k];
    if (tls) tls_send(hc, req, p);
    else     tcp_csend(hc, req, p);

    strcpy(status, "Se incarca ...");
    dirty = 1;

    html_len = 0;
    int idle = 0;
    for (;;) {
        int cap = HTML_CAP - 1 - html_len;
        if (cap <= 0) break;
        int chunk = cap > 1400 ? 1400 : cap;
        int r;
        if (tls) r = tls_recv(hc, html + html_len, chunk);
        else     r = tcp_crecv(hc, html + html_len, chunk);
        if (r > 0) { html_len += r; idle = 0; continue; }
        if (tls) {
            if (r < 0) break;            /* TLS inchis */
        } else {
            if (tcp_status(hc) == 0) {
                int r2 = tcp_crecv(hc, html + html_len, cap);
                if (r2 > 0) { html_len += r2; continue; }
                break;
            }
        }
        if (++idle > 300) break;
        task_sleep(25);
    }
    if (tls) tls_close(hc);
    else     tcp_cclose(hc);
    html[html_len] = '\0';
    store_cookies(html, html_len, host);
    return parse_status(html, html_len);
}

static void do_fetch(const char *url0)
{
    char url[300];
    strncpy(url, url0, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    /* daca nu are schema, adaugam http:// */
    if (!has_scheme(url) && !ci_eq(url, "about:", 6)) {
        char tmp[300];
        strcpy(tmp, "http://");
        strncat(tmp, url, sizeof(tmp) - 8);
        strncpy(url, tmp, sizeof(url) - 1);
    }

    state = ST_LOADING;
    scroll = 0;
    dirty = 1;

    int use_post = req_post;         /* doar prima cerere e POST */
    req_post = 0;

    int hops = 0;
    for (;;) {
        int code = http_get(url, use_post, req_post_body, req_post_len);
        use_post = 0;                /* dupa redirect: GET */
        if (code < 0) { state = ST_ERR; dirty = 1; return; }

        if ((code == 301 || code == 302 || code == 303 ||
             code == 307 || code == 308) && hops < 5) {
            char loc[300];
            if (find_header(html, html_len, "location:", loc, sizeof(loc))) {
                char nxt[300];
                resolve_url(url, loc, nxt, sizeof(nxt));
                strncpy(url, nxt, sizeof(url) - 1);
                url[sizeof(url) - 1] = '\0';
                strncpy(cur_url, url, sizeof(cur_url) - 1);
                if (!addr_focus) { strncpy(addr, url, sizeof(addr) - 1); addr_len = (int)strlen(addr); addr_caret = addr_len; }
                hops++;
                continue;
            }
        }

        /* corpul incepe dupa \r\n\r\n */
        int body = 0;
        for (int k = 0; k + 3 < html_len; k++)
            if (html[k] == '\r' && html[k + 1] == '\n' &&
                html[k + 2] == '\r' && html[k + 3] == '\n') { body = k + 4; break; }

        render_page(html + body, html_len - body);

        strncpy(cur_url, url, sizeof(cur_url) - 1);
        cur_url[sizeof(cur_url) - 1] = '\0';
        if (!addr_focus) { strncpy(addr, cur_url, sizeof(addr) - 1); addr_len = (int)strlen(addr); addr_caret = addr_len; }

        /* status: cod + octeti */
        char num[16]; int nn = html_len; int t = 0; char tb[12]; int ti = 0;
        do { tb[ti++] = (char)('0' + nn % 10); nn /= 10; } while (nn);
        while (ti--)
            num[t++] = tb[ti];
        num[t] = '\0';
        strcpy(status, "Gata - ");
        strcat(status, num);
        strcat(status, " octeti");
        if (code >= 400) { strcat(status, " (eroare "); }
        state = ST_DONE;
        dirty = 1;
        return;
    }
}

static void browser_task(void)
{
    for (;;) {
        if (req_flag) {
            req_flag = 0;
            do_fetch(req_url);
        }
        task_sleep(40);
    }
}

/* ================= API public ================= */

static void push_history(const char *url)
{
    if (hist_cur < hist_n - 1)
        hist_n = hist_cur + 1;          /* taiem ramura "inainte" */
    if (hist_n >= HIST_N) {
        for (int i = 0; i < HIST_N - 1; i++) strcpy(hist[i], hist[i + 1]);
        hist_n = HIST_N - 1;
        if (hist_cur > 0) hist_cur--;
    }
    strncpy(hist[hist_n], url, 299);
    hist[hist_n][299] = '\0';
    hist_cur = hist_n;
    hist_n++;
}

static void load_url(const char *url)
{
    strncpy(cur_url, url, sizeof(cur_url) - 1);
    cur_url[sizeof(cur_url) - 1] = '\0';
    strncpy(addr, url, sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = '\0';
    addr_len = (int)strlen(addr);
    addr_caret = addr_len;
    addr_view = 0;
    addr_focus = 0;
    strncpy(req_url, url, sizeof(req_url) - 1);
    req_url[sizeof(req_url) - 1] = '\0';
    req_flag = 1;
    state = ST_LOADING;
    scroll = 0;
    dirty = 1;
}

void browser_navigate(const char *url)
{
    if (!url || !url[0]) return;
    push_history(url);
    load_url(url);
}

static void browser_back(void)
{
    if (hist_cur > 0) { hist_cur--; load_url(hist[hist_cur]); }
}
static void browser_fwd(void)
{
    if (hist_cur < hist_n - 1) { hist_cur++; load_url(hist[hist_cur]); }
}

static const char *HOME =
    "<h1>DevOS Browser</h1>"
    "<p>Scrie o adresa in bara de sus si apasa Enter. Merge HTTP, HTTPS si JavaScript.</p>"
    "<p id=\"jsdemo\">(JavaScript nu a rulat)</p>"
    "<script>"
    "document.getElementById('jsdemo').innerHTML = "
    "'JavaScript ruleaza in DevOS! 6*7=' + (6*7) + ', sqrt(169)=' + Math.sqrt(169);"
    "</script>"
    "<p>Imagine PNG (decodata de DevOS dintr-un data: URI):</p>"
    "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAFAAAAAwCAYAAACG5f33AAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAACRSURBVGhD7dChAQIxEADBK+d1yqHOL4Ii0Gg6CB5H1o4Ys3Lnvdbm3PwG/mNgZGBkYGRgZGBkYGRgZGBkYGRgNOtem3MGRgZGBkYGRgZGBkYGRgZGBkYGRgZGsx6vzTkDIwMjAyMDIwMjAyMDIwMjAyMDIwOj+TyvzTkDIwMjAyMDIwMjAyMDIwMjAyMDIwOjL29uMJ0mi6joAAAAAElFTkSuQmCC\">"
    "<p>Exemple (click pe link):</p>"
    "<p><a href=\"https://example.com/\">https://example.com</a> - test HTTPS/TLS</p>"
    "<p><a href=\"http://info.cern.ch/\">http://info.cern.ch</a> - primul site web din lume</p>"
    "<p><a href=\"https://www.google.com/\">https://www.google.com</a></p>"
    "<p>Comenzi: sagetile sus/jos = derulare, click pe bara = editezi adresa,"
    " Backspace (fara focus) = inapoi.</p>";

void browser_init(void)
{
    uint64_t hp = pmm_alloc_contig((HTML_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t rp = pmm_alloc_contig((RUN_CAP * sizeof(struct run) + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t lp = pmm_alloc_contig((LINK_CAP * HREF_MAX + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t cp = pmm_alloc_contig((CSS_CAP * sizeof(struct cssrule) + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t dp = pmm_alloc_contig((DOM_MAX * sizeof(struct dnode) + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t sp2 = pmm_alloc_contig((DOM_SCR_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t jp = pmm_alloc_contig((JS_ARENA + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t serp = pmm_alloc_contig((SER_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t iap = pmm_alloc_contig((IMG_ARENA + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t isp = pmm_alloc_contig((IMG_SRC_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t ibp = pmm_alloc_contig((IMG_BIN_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t iscp = pmm_alloc_contig((IMG_SCR_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    html = (char *)hp;
    runs = (struct run *)rp;
    links = (char (*)[HREF_MAX])lp;
    css = (struct cssrule *)cp;
    dnodes = (struct dnode *)dp;
    dom_scratch = (char *)sp2;
    js_arena = (char *)jp;
    ser_all = (char *)serp;
    img_arena = (uint32_t *)iap;
    img_src = (char *)isp;
    img_bin = (uint8_t *)ibp;
    img_scr = (uint8_t *)iscp;

    strcpy(addr, "");
    addr_len = 0;
    addr_focus = 0;
    strcpy(cur_url, "about:home");
    strcpy(hist[0], "about:home");
    hist_n = 1; hist_cur = 0;
    strcpy(status, "Gata.");

    if (html && runs && links && css) {
        render_page(HOME, (int)strlen(HOME));
        state = ST_DONE;
    } else {
        strcpy(status, "Memorie insuficienta pentru browser.");
        state = ST_ERR;
    }
    task_create("browser", browser_task);
}

int browser_is_loading(void) { return state == ST_LOADING; }

int browser_poll_dirty(void)
{
    int d = dirty;
    dirty = 0;
    return d;
}

void browser_scroll(int lines)
{
    int max = content_h - BR_PAGE_H;
    if (max < 0) max = 0;
    scroll += lines * BR_LINE;
    if (scroll < 0) scroll = 0;
    if (scroll > max) scroll = max;
    dirty = 1;
}

/* ---- desenare ---- */

static void draw_btn(int x, int y, int w, const char *label, int enabled)
{
    fb_fill_round2(x, y, w, 24, 8, enabled ? BTN_BG : 0x30343C, 1, TOOL_BG);
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    fb_text(tx, y + 4, label, enabled ? 0xE6EAF0 : 0x6A7180, enabled ? BTN_BG : 0x30343C);
}

void browser_draw(int cx, int cy)
{
    /* bara de unelte */
    fb_fill(cx, cy, br_w, BR_TOOL, TOOL_BG);
    draw_btn(cx + 6, cy + 8, 26, "<", hist_cur > 0);
    draw_btn(cx + 34, cy + 8, 26, ">", hist_cur < hist_n - 1);
    draw_btn(cx + 62, cy + 8, 26, "R", 1);

    int ax = cx + 94;
    int aw = br_w - 94 - 52;
    fb_fill_round2(ax, cy + 8, aw, 24, 8, addr_focus ? 0xFFFFFF : 0x3A3F49, 1, TOOL_BG);
    int tx = ax + 8;
    int maxc = (aw - 16) / 8;
    if (maxc < 1) maxc = 1;
    /* derulare orizontala: tinem cursorul (addr_caret) mereu vizibil */
    if (addr_caret < addr_view) addr_view = addr_caret;
    if (addr_caret > addr_view + maxc) addr_view = addr_caret - maxc;
    if (addr_view > addr_len) addr_view = addr_len;
    if (addr_view < 0) addr_view = 0;
    uint32_t afg = addr_focus ? 0x202124 : 0xD7DCE4;
    uint32_t abg = addr_focus ? 0xFFFFFF : 0x3A3F49;
    char vis[82];
    int vn = 0;
    for (int i = addr_view; i < addr_len && vn < maxc && vn < 81; i++)
        vis[vn++] = addr[i];
    vis[vn] = '\0';
    if (vn == 0 && !addr_focus)
        fb_text(tx, cy + 12, "Cauta sau scrie o adresa", 0x8A93A0, abg);
    else
        fb_text(tx, cy + 12, vis, afg, abg);
    if (addr_focus) {
        int cpos = tx + (addr_caret - addr_view) * 8;
        fb_fill(cpos, cy + 11, 2, 18, 0x1284E4);
    }

    fb_fill_round2(cx + br_w - 50, cy + 8, 44, 24, 8, ACCENT, 1, TOOL_BG);
    fb_text(cx + br_w - 50 + 13, cy + 12, "Go", 0xFFFFFF, ACCENT);

    /* zona de pagina */
    int py = cy + BR_TOOL;
    fb_fill(cx, py, br_w, BR_PAGE_H, PAGE_BG);

    if (state == ST_LOADING) {
        fb_text(cx + BR_MARGIN, py + 20, "Se incarca...", 0x606368, PAGE_BG);
        fb_text(cx + BR_MARGIN, py + 44, addr, 0x808388, PAGE_BG);
    } else if (state == ST_ERR) {
        fb_text(cx + BR_MARGIN, py + 20, "Nu s-a putut incarca pagina.", 0xC0392B, PAGE_BG);
        fb_text(cx + BR_MARGIN, py + 44, status, 0x808388, PAGE_BG);
    } else if (run_n == 0) {
        fb_text(cx + BR_MARGIN, py + 20,
                "Pagina nu contine text vizibil.", 0x606368, PAGE_BG);
        fb_text(cx + BR_MARGIN, py + 44,
                "Poate folosi framework-uri JS mari sau functii web neacceptate.",
                0x808388, PAGE_BG);
        fb_text(cx + BR_MARGIN, py + 68, status, 0xA0A4AC, PAGE_BG);
    } else {
        int top = py, bot = py + BR_PAGE_H;
        for (int i = 0; i < run_n; i++) {
            struct run *r = &runs[i];
            int sy = py + r->y - scroll;
            if (sy + r->h <= top || sy >= bot) continue;
            int sx = cx + r->x;
            if (r->img) {                        /* imagine: blit cu scalare */
                for (int dy = 0; dy < r->h; dy++) {
                    int yy = sy + dy;
                    if (yy < top || yy >= bot) continue;
                    int srcy = r->ih > 0 ? dy * r->ih / r->h : 0;
                    const uint32_t *srow = r->img + (long)srcy * r->iw;
                    for (int dx = 0; dx < r->w; dx++) {
                        int srcx = r->iw > 0 ? dx * r->iw / r->w : 0;
                        fb_putpixel(sx + dx, yy, srow[srcx]);
                    }
                }
                if (r->link >= 0) fb_fill(sx, sy + r->h - 1, r->w, 1, LINK_FG);
                continue;
            }
            if (r->field >= 0) {                 /* control de formular */
                struct field *f = &fields[r->field];
                if (f->type == F_SUBMIT || f->type == F_BUTTON) {
                    uint32_t bbg = ACCENT;
                    fb_fill_round2(sx, sy, r->w, r->h, 6, bbg, 1, PAGE_BG);
                    const char *lb = f->label[0] ? f->label : "Trimite";
                    fb_text(sx + (r->w - (int)strlen(lb)*8)/2, sy + 3, lb, 0xFFFFFF, bbg);
                } else if (f->type == F_CHECKBOX) {
                    fb_fill_round2(sx, sy, r->w, r->h, 3, 0xFFFFFF, 1, PAGE_BG);
                    fb_fill(sx, sy, r->w, 1, 0x808890); fb_fill(sx, sy, 1, r->h, 0x808890);
                    fb_fill(sx, sy+r->h-1, r->w, 1, 0x808890); fb_fill(sx+r->w-1, sy, 1, r->h, 0x808890);
                    if (f->checked) fb_text(sx + 4, sy + 1, "x", 0x1284E4, 0xFFFFFF);
                } else {                         /* text / password / textarea */
                    fb_fill(sx, sy, r->w, r->h, 0xFFFFFF);
                    uint32_t brd = (focused_field == r->field) ? ACCENT : 0x9098A0;
                    fb_fill(sx, sy, r->w, 1, brd); fb_fill(sx, sy+r->h-1, r->w, 1, brd);
                    fb_fill(sx, sy, 1, r->h, brd); fb_fill(sx+r->w-1, sy, 1, r->h, brd);
                    char *v = fval[r->field];
                    int vl = (int)strlen(v);
                    int maxch = (r->w - 8) / 8;
                    int start = 0;
                    if (f->type != F_TEXTAREA && vl > maxch) start = vl - maxch;
                    char shown[80]; int sc2 = 0;
                    for (int k = start; v[k] && sc2 < maxch && sc2 < 79; k++)
                        shown[sc2++] = (f->type == F_PASSWORD) ? '*' : v[k];
                    shown[sc2] = 0;
                    fb_text(sx + 4, sy + 3, shown, 0x202124, 0xFFFFFF);
                    if (focused_field == r->field) {
                        int cxp = sx + 4 + sc2 * 8;
                        fb_fill(cxp, sy + 2, 2, r->h - 4, ACCENT);
                    }
                }
                continue;
            }
            if (r->bg) fb_fill(sx, sy, r->w, r->h, r->bg);
            if (r->text[0] == '\0') continue;    /* ex. linia <hr> */
            uint32_t tbg = r->bg ? r->bg : PAGE_BG;
            if (r->scale > 1) {
                fb_text_scaled(sx, sy, r->text, r->color, r->scale);
                if (r->bold) fb_text_scaled(sx + 1, sy, r->text, r->color, r->scale);
            } else {
                fb_text(sx, sy, r->text, r->color, tbg);
                if (r->bold) fb_text(sx + 1, sy, r->text, r->color, tbg);
            }
            if (r->link >= 0)
                fb_fill(sx, sy + r->h - 1, r->w, 1, r->color);
        }
        /* scrollbar */
        if (content_h > BR_PAGE_H) {
            int trackx = cx + br_w - 10;
            fb_fill(trackx, py, 8, BR_PAGE_H, 0xE8EAED);
            int th = BR_PAGE_H * BR_PAGE_H / content_h;
            if (th < 24) th = 24;
            int maxs = content_h - BR_PAGE_H;
            int ty = py + (maxs ? (BR_PAGE_H - th) * scroll / maxs : 0);
            fb_fill_round2(trackx, ty, 8, th, 4, 0xB0B4BB, 1, 0xE8EAED);
        }
    }

    /* bara de stare */
    int sy = cy + br_h - BR_STAT;
    fb_fill(cx, sy, br_w, BR_STAT, TOOL_BG);
    fb_fill(cx, sy, br_w, 1, 0x14161B);
    fb_text(cx + 8, sy + 3, status, STAT_FG, TOOL_BG);
}

/* titlul paginii (gol daca pagina n-are <title>) */
const char *browser_title(void)
{
    return page_title;
}

/* ---- interactiune ---- */

static int in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int urlencode(const char *s, char *out, int cap)
{
    int o = 0;
    static const char *hex = "0123456789ABCDEF";
    for (; *s && o < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')
            out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    return o;
}

/* trimite un formular: construieste query-ul din campuri, apoi GET sau POST */
static void do_submit(int formidx)
{
    if (formidx < 0 || formidx >= form_n) return;
    struct wform *fm = &forms[formidx];
    static char query[4096];
    int q = 0;
    for (int i = 0; i < field_n; i++) {
        struct field *f = &fields[i];
        if (f->form != formidx) continue;
        if (f->type == F_SUBMIT || f->type == F_BUTTON) continue;
        if (f->type == F_CHECKBOX && !f->checked) continue;
        if (!f->name[0]) continue;
        if (q) query[q++] = '&';
        q += urlencode(f->name, query + q, (int)sizeof(query) - q);
        query[q++] = '=';
        const char *val = (f->type == F_CHECKBOX) ? (fval[i][0] ? fval[i] : "on") : fval[i];
        q += urlencode(val, query + q, (int)sizeof(query) - q);
    }
    query[q] = 0;

    char action[300];
    if (fm->action[0]) resolve_url(cur_url, fm->action, action, sizeof(action));
    else { strncpy(action, cur_url, sizeof(action) - 1); action[sizeof(action) - 1] = 0; }

    if (fm->method == 1) {               /* POST */
        int n = q; if (n > (int)sizeof(req_post_body) - 1) n = sizeof(req_post_body) - 1;
        memcpy(req_post_body, query, n); req_post_body[n] = 0; req_post_len = n;
        req_post = 1;
        browser_navigate(action);
    } else {                             /* GET: action?query */
        static char full[4096];
        int fl = 0;
        for (const char *p = action; *p && fl < 4000; p++) if (*p != '?') full[fl++] = *p; else break;
        full[fl++] = '?';
        for (int k = 0; k < q && fl < 4095; k++) full[fl++] = query[k];
        full[fl] = 0;
        browser_navigate(full);
    }
}

void browser_click(int cx, int cy, int mx, int my)
{
    int rx = mx - cx, ry = my - cy;

    if (ry < BR_TOOL) {
        if (in_rect(rx, ry, 6, 8, 26, 24)) { browser_back(); return; }
        if (in_rect(rx, ry, 34, 8, 26, 24)) { browser_fwd(); return; }
        if (in_rect(rx, ry, 62, 8, 26, 24)) { browser_navigate(cur_url); return; }
        int ax = 94, aw = br_w - 94 - 52;
        if (in_rect(rx, ry, ax, 8, aw, 24)) {
            addr_focus = 1;
            int tx = ax + 8;
            int col = (rx - tx + 4) / 8;      /* caracterul de sub click */
            if (col < 0) col = 0;
            addr_caret = addr_view + col;
            if (addr_caret > addr_len) addr_caret = addr_len;
            if (addr_caret < 0) addr_caret = 0;
            dirty = 1;
            return;
        }
        if (in_rect(rx, ry, br_w - 50, 8, 44, 24)) {
            addr_focus = 0;
            browser_navigate(addr);
            return;
        }
        return;
    }
    if (ry >= br_h - BR_STAT) return;

    /* click in pagina: camp de formular sau link? */
    addr_focus = 0;
    if (state == ST_DONE) {
        int py = BR_TOOL;
        int content_y = ry - py + scroll;
        for (int i = 0; i < run_n; i++) {
            struct run *r = &runs[i];
            if (r->field < 0) continue;
            if (rx >= r->x && rx < r->x + r->w &&
                content_y >= r->y && content_y < r->y + r->h) {
                struct field *f = &fields[r->field];
                if (f->type == F_SUBMIT || f->type == F_BUTTON) {
                    focused_field = -1;
                    if (f->type == F_SUBMIT) do_submit(f->form);
                } else if (f->type == F_CHECKBOX) {
                    f->checked = !f->checked;
                    focused_field = -1;
                    relayout();
                } else {
                    focused_field = r->field;
                    relayout();
                }
                return;
            }
        }
        for (int i = 0; i < run_n; i++) {
            struct run *r = &runs[i];
            if (r->link < 0) continue;
            if (rx >= r->x - 2 && rx < r->x + r->w + 2 &&
                content_y >= r->y - 2 && content_y < r->y + r->h + 2) {
                char full[300];
                resolve_url(cur_url, links[r->link], full, sizeof(full));
                browser_navigate(full);
                return;
            }
        }
    }
    focused_field = -1;
    dirty = 1;
}

int browser_key(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (addr_focus) {
        if (c == '\n') {                       /* Enter: navigheaza */
            addr_focus = 0;
            browser_navigate(addr);
        } else if (c == 27) {                  /* Esc: renunta */
            addr_focus = 0;
            strncpy(addr, cur_url, sizeof(addr) - 1);
            addr[sizeof(addr) - 1] = '\0';
            addr_len = (int)strlen(addr);
            addr_caret = addr_len;
        } else if (c == '\b') {                /* Backspace: sterge inainte de cursor */
            if (addr_caret > 0) {
                for (int i = addr_caret - 1; i < addr_len; i++)
                    addr[i] = addr[i + 1];
                addr_len--;
                addr_caret--;
            }
        } else if (c == 0x84) {                /* Delete: sterge la cursor */
            if (addr_caret < addr_len) {
                for (int i = addr_caret; i < addr_len; i++)
                    addr[i] = addr[i + 1];
                addr_len--;
            }
        } else if (c == 0x82 || c == 0x92) {   /* stanga */
            if (addr_caret > 0) addr_caret--;
        } else if (c == 0x83 || c == 0x93) {   /* dreapta */
            if (addr_caret < addr_len) addr_caret++;
        } else if (c == 0x85 || c == 0x94) {   /* Home */
            addr_caret = 0;
        } else if (c == 0x86 || c == 0x95) {   /* End */
            addr_caret = addr_len;
        } else if (c >= 32 && c < 127 && addr_len < (int)sizeof(addr) - 1) {
            /* insereaza la cursor (nu la sfarsit) */
            for (int i = addr_len; i > addr_caret; i--)
                addr[i] = addr[i - 1];
            addr[addr_caret] = (char)c;
            addr_len++;
            addr_caret++;
            addr[addr_len] = '\0';
        }
        dirty = 1;
        return 1;
    }
    /* Tab: focuseaza urmatorul camp editabil (are prioritate fata de editare) */
    if (c == '\t' && field_n > 0) {
        int start = focused_field;
        for (int k = 1; k <= field_n; k++) {
            int idx = (start + k) % field_n;
            int t = fields[idx].type;
            if (t == F_TEXT || t == F_PASSWORD || t == F_TEXTAREA) {
                focused_field = idx; relayout(); return 1;
            }
        }
        return 1;
    }
    /* camp de formular focusat: editam valoarea lui */
    if (focused_field >= 0 && focused_field < field_n) {
        char *v = fval[focused_field];
        int vl = (int)strlen(v);
        int ta = (fields[focused_field].type == F_TEXTAREA);
        if (c == '\b') { if (vl > 0) v[vl - 1] = 0; relayout(); return 1; }
        if (c == '\n') {
            if (ta) { if (vl < FVAL_CAP - 1) { v[vl] = '\n'; v[vl+1] = 0; } relayout(); }
            else { int fm = fields[focused_field].form; focused_field = -1; do_submit(fm); }
            return 1;
        }
        if (c == 27) { focused_field = -1; relayout(); return 1; }
        if (c == 0x81) { browser_scroll(1); return 1; }
        if (c == 0x80) { browser_scroll(-1); return 1; }
        if (c >= 32 && c < 127 && vl < FVAL_CAP - 1) {
            v[vl] = (char)c; v[vl + 1] = 0; relayout();
        }
        return 1;
    }
    /* fara focus pe bara: derulare / navigare */
    if (c == 0x80) { browser_scroll(-1); return 1; }       /* sus */
    if (c == 0x81) { browser_scroll(1); return 1; }        /* jos */
    if (c == ' ') { browser_scroll(BR_PAGE_H / BR_LINE - 1); return 1; }
    if (c == '\b') { browser_back(); return 1; }
    if (c == '\n' || (c >= 32 && c < 127)) {
        /* a tasta fara sa fi dat click = adresa noua (porneste de la zero).
         * pentru editare in loc: click pe bara (pune cursorul unde vrei). */
        addr_focus = 1;
        addr_len = 0;
        addr_caret = 0;
        addr_view = 0;
        if (c >= 32 && c < 127) {
            addr[addr_len++] = (char)c;
            addr[addr_len] = '\0';
            addr_caret = addr_len;
        }
        dirty = 1;
        return 1;
    }
    return 1;
}
