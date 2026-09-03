/* Browser web MyOS. Client HTTP peste clientul TCP din kernel (tcp.c),
 * parser HTML minimal cu layout (word-wrap), linkuri clicabile, istoric,
 * derulare. Reteaua ruleaza pe un fir de kernel (task_create) ca sa poata
 * astepta politicos (task_sleep) fara sa blocheze intreruperile. */

#include "browser.h"
#include "fb.h"
#include "netstack.h"
#include "tls.h"
#include "pmm.h"
#include "string.h"
#include "task.h"

/* ---- geometrie (continut 640x400, ca celelalte ferestre) ---- */
#define BR_W 640
#define BR_H 400
#define BR_TOOL 40
#define BR_STAT 22
#define BR_PAGE_H (BR_H - BR_TOOL - BR_STAT)
#define BR_LINE 18
#define BR_MARGIN 12
#define BR_RIGHT (BR_W - BR_MARGIN - 14)   /* lasa loc de scrollbar */

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

/* buffere mari alocate din PMM (identity-mapped), nu din kheap */
#define HTML_CAP (160 * 1024)
static char *html;
static int   html_len;

struct run {
    int y;
    short x;
    short link;
    unsigned char bold;
    unsigned int color;
    char text[28];
};
#define RUN_CAP 4000
static struct run *runs;
static int run_n;

#define LINK_CAP 400
#define HREF_MAX 192
static char (*links)[HREF_MAX];
static int link_n;

static int scroll;
static int content_h;

/* istoric */
#define HIST_N 24
static char hist[HIST_N][300];
static int  hist_n, hist_cur;

static volatile int dirty;

/* stare de layout (folosita doar de firul de retea) */
static int  ly_x, ly_y;
static int  ly_bold;
static int  ly_link;
static unsigned int ly_color;

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

/* ================= layout HTML ================= */

static void ly_newline(void)
{
    ly_x = BR_MARGIN;
    ly_y += BR_LINE;
}

static void ly_blank(void)   /* linie goala doar daca nu suntem deja la inceput */
{
    if (ly_x > BR_MARGIN) ly_newline();
    ly_y += BR_LINE / 2;
}

static void emit_word(const char *w, int len)
{
    if (len <= 0) return;
    if (len > 70) len = 70;
    int wpx = len * 8;
    if (ly_x > BR_MARGIN && ly_x + wpx > BR_RIGHT) ly_newline();
    if (run_n < RUN_CAP) {
        struct run *r = &runs[run_n++];
        r->x = (short)ly_x;
        r->y = ly_y;
        r->bold = (unsigned char)ly_bold;
        r->link = (short)ly_link;
        r->color = ly_color;
        int c = 0;
        for (; c < len && c < 27; c++) r->text[c] = w[c];
        r->text[c] = '\0';
    }
    ly_x += wpx + 8;
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

/* extrage atributul href din interiorul unui tag (pana la '>') */
static void read_href(const char *s, char *out, int cap)
{
    out[0] = '\0';
    for (int i = 0; s[i] && s[i] != '>'; i++) {
        if ((s[i] == 'h' || s[i] == 'H') && ci_eq(s + i, "href", 4)) {
            int j = i + 4;
            while (s[j] == ' ' || s[j] == '=') j++;
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

static int is_block_tag(const char *n)
{
    static const char *b[] = { "p", "div", "br", "h1", "h2", "h3", "h4", "h5",
                               "h6", "li", "tr", "ul", "ol", "table", "form",
                               "header", "footer", "section", "article", "nav",
                               "hr", "blockquote", "pre", "figure", "main" };
    for (unsigned i = 0; i < sizeof(b) / sizeof(b[0]); i++)
        if (strcmp(n, b[i]) == 0) return 1;
    return 0;
}

static int is_heading(const char *n)
{
    return n[0] == 'h' && n[1] >= '1' && n[1] <= '3' && n[2] == '\0';
}

static char page_title[80];

static void layout_html(const char *h, int n)
{
    run_n = 0; link_n = 0; content_h = 0; page_title[0] = '\0';
    ly_x = BR_MARGIN; ly_y = BR_MARGIN;
    ly_bold = 0; ly_link = -1; ly_color = PAGE_FG;

    int i = 0;
    while (i < n) {
        char c = h[i];
        if (c == '<') {
            int close = (h[i + 1] == '/');
            const char *ts = h + i + (close ? 2 : 1);
            char name[16];
            read_tag_name(ts, name, sizeof(name));

            /* <script>/<style>: sarim tot pana la inchidere */
            if (!close && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) {
                const char *endtag = (name[0] == 's' && name[1] == 'c') ? "/script" : "/style";
                int j = i + 1;
                while (j < n) {
                    if (h[j] == '<' && ci_eq(h + j + 1, endtag, (int)strlen(endtag))) break;
                    j++;
                }
                i = j;
                /* sarim si tag-ul de inchidere */
                while (i < n && h[i] != '>') i++;
                i++;
                continue;
            }

            if (strcmp(name, "title") == 0) {
                if (!close) {
                    int j = i;
                    while (j < n && h[j] != '>') j++;
                    j++;
                    int t = 0;
                    while (j < n && h[j] != '<' && t < (int)sizeof(page_title) - 1)
                        page_title[t++] = h[j++];
                    page_title[t] = '\0';
                    i = j;
                    continue;
                }
            } else if (strcmp(name, "a") == 0) {
                if (!close) {
                    char href[HREF_MAX];
                    read_href(ts, href, sizeof(href));
                    if (href[0] && link_n < LINK_CAP) {
                        int k = 0;
                        for (; href[k] && k < HREF_MAX - 1; k++) links[link_n][k] = href[k];
                        links[link_n][k] = '\0';
                        ly_link = link_n++;
                        ly_color = LINK_FG;
                    }
                } else {
                    ly_link = -1;
                    ly_color = PAGE_FG;
                }
            } else if (is_heading(name)) {
                ly_blank();
                if (ly_x > BR_MARGIN) ly_newline();
                ly_bold = !close;
                ly_color = close ? PAGE_FG : HEAD_FG;
                if (close) ly_blank();
            } else if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) {
                ly_bold = !close;
            } else if (is_block_tag(name)) {
                if (ly_x > BR_MARGIN) ly_newline();
                if (strcmp(name, "p") == 0 || strcmp(name, "li") == 0)
                    ly_y += 2;      /* mic spatiu intre paragrafe */
            }

            /* avanseaza dupa '>' */
            while (i < n && h[i] != '>') i++;
            i++;
        } else if (c == '&') {
            char out;
            int used = decode_entity(h + i, &out);
            if (used) {
                char w[2] = { out, 0 };
                if (out == ' ') { ly_x += 8; }
                else emit_word(w, 1);
                i += used;
            } else {
                emit_word("&", 1);
                i++;
            }
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
        } else {
            /* un cuvant: pana la spatiu/tag/entitate */
            char word[80];
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
    content_h = ly_y + BR_LINE;
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
static int http_get(const char *url)
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
    char req[600];
    int p = 0;
    const char *g = "GET "; while (*g) req[p++] = *g++;
    for (const char *s = path; *s; s++) req[p++] = *s;
    const char *m = " HTTP/1.0\r\nHost: "; while (*m) req[p++] = *m++;
    for (const char *s = host; *s; s++) req[p++] = *s;
    const char *e = "\r\nUser-Agent: MyOS-Browser/1.0\r\n"
                    "Accept: text/html\r\nConnection: close\r\n\r\n";
    while (*e) req[p++] = *e++;
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

    int hops = 0;
    for (;;) {
        int code = http_get(url);
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

        layout_html(html + body, html_len - body);

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
    "<h1>MyOS Browser</h1>"
    "<p>Scrie o adresa in bara de sus si apasa Enter. Merge HTTP si HTTPS.</p>"
    "<p>Exemple (click pe link):</p>"
    "<p><a href=\"https://example.com/\">https://example.com</a> - test HTTPS/TLS</p>"
    "<p><a href=\"http://info.cern.ch/\">http://info.cern.ch</a> - primul site web din lume</p>"
    "<p><a href=\"https://www.google.com/\">https://www.google.com</a></p>"
    "<p><a href=\"http://httpforever.com/\">http://httpforever.com</a></p>"
    "<p>Comenzi: sagetile sus/jos = derulare, click pe bara = editezi adresa,"
    " Backspace (fara focus) = inapoi.</p>";

void browser_init(void)
{
    uint64_t hp = pmm_alloc_contig((HTML_CAP + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t rp = pmm_alloc_contig((RUN_CAP * sizeof(struct run) + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint64_t lp = pmm_alloc_contig((LINK_CAP * HREF_MAX + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    html = (char *)hp;
    runs = (struct run *)rp;
    links = (char (*)[HREF_MAX])lp;

    strcpy(addr, "");
    addr_len = 0;
    addr_focus = 0;
    strcpy(cur_url, "about:home");
    strcpy(hist[0], "about:home");
    hist_n = 1; hist_cur = 0;
    strcpy(status, "Gata.");

    if (html && runs && links) {
        layout_html(HOME, (int)strlen(HOME));
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
    fb_fill(cx, cy, BR_W, BR_TOOL, TOOL_BG);
    draw_btn(cx + 6, cy + 8, 26, "<", hist_cur > 0);
    draw_btn(cx + 34, cy + 8, 26, ">", hist_cur < hist_n - 1);
    draw_btn(cx + 62, cy + 8, 26, "R", 1);

    int ax = cx + 94;
    int aw = BR_W - 94 - 52;
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

    fb_fill_round2(cx + BR_W - 50, cy + 8, 44, 24, 8, ACCENT, 1, TOOL_BG);
    fb_text(cx + BR_W - 50 + 13, cy + 12, "Go", 0xFFFFFF, ACCENT);

    /* zona de pagina */
    int py = cy + BR_TOOL;
    fb_fill(cx, py, BR_W, BR_PAGE_H, PAGE_BG);

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
                "Probabil e construita cu JavaScript (neacceptat inca).",
                0x808388, PAGE_BG);
        fb_text(cx + BR_MARGIN, py + 68, status, 0xA0A4AC, PAGE_BG);
    } else {
        int top = py, bot = py + BR_PAGE_H;
        for (int i = 0; i < run_n; i++) {
            struct run *r = &runs[i];
            int sy = py + r->y - scroll;
            if (sy + 16 <= top || sy >= bot) continue;
            int sx = cx + r->x;
            fb_text(sx, sy, r->text, r->color, PAGE_BG);
            if (r->bold) fb_text(sx + 1, sy, r->text, r->color, PAGE_BG);
            if (r->link >= 0) {
                int uw = (int)strlen(r->text) * 8;
                fb_fill(sx, sy + 15, uw, 1, r->color);
            }
        }
        /* scrollbar */
        if (content_h > BR_PAGE_H) {
            int trackx = cx + BR_W - 10;
            fb_fill(trackx, py, 8, BR_PAGE_H, 0xE8EAED);
            int th = BR_PAGE_H * BR_PAGE_H / content_h;
            if (th < 24) th = 24;
            int maxs = content_h - BR_PAGE_H;
            int ty = py + (maxs ? (BR_PAGE_H - th) * scroll / maxs : 0);
            fb_fill_round2(trackx, ty, 8, th, 4, 0xB0B4BB, 1, 0xE8EAED);
        }
    }

    /* bara de stare */
    int sy = cy + BR_H - BR_STAT;
    fb_fill(cx, sy, BR_W, BR_STAT, TOOL_BG);
    fb_fill(cx, sy, BR_W, 1, 0x14161B);
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

void browser_click(int cx, int cy, int mx, int my)
{
    int rx = mx - cx, ry = my - cy;

    if (ry < BR_TOOL) {
        if (in_rect(rx, ry, 6, 8, 26, 24)) { browser_back(); return; }
        if (in_rect(rx, ry, 34, 8, 26, 24)) { browser_fwd(); return; }
        if (in_rect(rx, ry, 62, 8, 26, 24)) { browser_navigate(cur_url); return; }
        int ax = 94, aw = BR_W - 94 - 52;
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
        if (in_rect(rx, ry, BR_W - 50, 8, 44, 24)) {
            addr_focus = 0;
            browser_navigate(addr);
            return;
        }
        return;
    }
    if (ry >= BR_H - BR_STAT) return;

    /* click in pagina: link? */
    addr_focus = 0;
    if (state == ST_DONE) {
        int py = BR_TOOL;
        int content_y = ry - py + scroll;
        for (int i = 0; i < run_n; i++) {
            struct run *r = &runs[i];
            if (r->link < 0) continue;
            int rw = (int)strlen(r->text) * 8;
            if (rx >= r->x - 2 && rx < r->x + rw + 2 &&
                content_y >= r->y - 2 && content_y < r->y + 18) {
                char full[300];
                resolve_url(cur_url, links[r->link], full, sizeof(full));
                browser_navigate(full);
                return;
            }
        }
    }
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
