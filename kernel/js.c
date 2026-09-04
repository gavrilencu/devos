/* Motor JavaScript minimal pentru MyOS. Tree-walking, arena bump.
 * Subset: var/let/const, functii (inclusiv arrow simple), if/else, for,
 * for-in, while, do-while, return/break/continue, obiecte, array-uri,
 * string-uri, operatori uzuali, ternar, si o biblioteca de baza (console,
 * Math, String/Array, parseInt, document/DOM). NU e V8. */

#include "js.h"
#include "string.h"

/* ---------- valori si obiecte ---------- */
typedef struct Obj Obj;
typedef struct { uint8_t t; union { double n; int b; Obj *o; } u; } Val;
enum { T_UNDEF, T_NULL, T_BOOL, T_NUM, T_OBJ };
enum { O_STR, O_OBJECT, O_ARRAY, O_FUNC, O_NATIVE, O_DOM };

typedef struct Node Node;
typedef struct Env Env;

struct Prop { char *key; Val val; };

struct Obj {
    uint8_t kind;
    char *s; int slen;                 /* O_STR */
    struct Prop *props; int np, pcap;  /* proprietati (orice obiect) */
    Val *el; int nel, elcap;           /* O_ARRAY */
    Node *fn; Env *env;                /* O_FUNC */
    Val (*nat)(JS *, Obj *, Val, Val *, int); /* O_NATIVE (self, this, args) */
    int dom;                           /* O_DOM: handle; sau tip metoda nativa */
};

struct Env { Env *parent; struct Prop *v; int nv, vcap; };

/* ---------- AST ---------- */
enum {
    N_NUM, N_STR, N_BOOL, N_NULL, N_UNDEF, N_IDENT, N_THIS,
    N_ARR, N_OBJ, N_BIN, N_UN, N_POST, N_ASSIGN, N_LOGIC, N_COND,
    N_CALL, N_MEMBER, N_INDEX, N_FUNC, N_VAR, N_IF, N_FOR, N_FORIN,
    N_WHILE, N_DO, N_BLOCK, N_RET, N_BREAK, N_CONT, N_EXPR, N_PROG, N_NEW
};
struct Node {
    uint8_t k;
    double num; char *str; int op;
    Node *a, *b, *c, *d;
    Node **list; int nlist;
    char **keys;
};

/* ---------- token-uri ---------- */
enum {
    K_EOF, K_NUM, K_STR, K_ID,
    K_LP, K_RP, K_LB, K_RB, K_LC, K_RC,
    K_SEMI, K_COMMA, K_DOT, K_COLON, K_QUEST,
    K_ASSIGN, K_PLUS, K_MINUS, K_STAR, K_SLASH, K_PCT,
    K_EQ, K_NE, K_SEQ, K_SNE, K_LT, K_GT, K_LE, K_GE,
    K_AND, K_OR, K_NOT, K_INC, K_DEC,
    K_PLUSEQ, K_MINUSEQ, K_STAREQ, K_SLASHEQ, K_ARROW,
    K_VAR, K_FUNCTION, K_RETURN, K_IF, K_ELSE, K_FOR, K_WHILE, K_DO,
    K_BREAK, K_CONTINUE, K_TRUE, K_FALSE, K_NULL, K_UNDEF, K_NEW,
    K_TYPEOF, K_THIS, K_IN, K_OF
};

struct JS {
    char *arena; uint32_t asz, aused;
    char err[160]; int haserr;
    Env *global;
    struct js_dom_ops *dom;
    Val retv;
    int comp;                  /* completare: 0 normal,1 return,2 break,3 continue */
    long steps;
    int depth;
    char panic[256];
    /* lexer */
    const char *src; int slen, pos;
    int tk; double tnum; char *tstr; int tlen;
    int prev_end;              /* pt. ASI / arrow lookahead */
};

#define STEP_LIMIT  20000000
#define DEPTH_LIMIT 300

/* ---------- arena ---------- */
static void *aalloc(JS *J, int n)
{
    n = (n + 7) & ~7;
    if (J->aused + n > J->asz) {
        if (!J->haserr) { J->haserr = 1; strcpy(J->err, "memorie JS epuizata"); }
        return J->panic;
    }
    void *p = J->arena + J->aused;
    J->aused += n;
    return p;
}
static char *adup(JS *J, const char *s, int n)
{
    char *p = (char *)aalloc(J, n + 1);
    memcpy(p, s, n); p[n] = 0;
    return p;
}

/* ---------- constructori de valori ---------- */
static Val vundef(void) { Val v; v.t = T_UNDEF; return v; }
static Val vnull(void) { Val v; v.t = T_NULL; return v; }
static Val vbool(int b) { Val v; v.t = T_BOOL; v.u.b = b ? 1 : 0; return v; }
static Val vnum(double n) { Val v; v.t = T_NUM; v.u.n = n; return v; }
static Val vobj(Obj *o) { Val v; v.t = T_OBJ; v.u.o = o; return v; }

static Obj *newobj(JS *J, int kind)
{
    Obj *o = (Obj *)aalloc(J, sizeof(Obj));
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    return o;
}
static Val vstr(JS *J, const char *s, int n)
{
    Obj *o = newobj(J, O_STR);
    o->s = adup(J, s, n); o->slen = n;
    return vobj(o);
}
static Val vcstr(JS *J, const char *s)
{
    int n = 0; while (s[n]) n++;
    return vstr(J, s, n);
}

static int is_str(Val v) { return v.t == T_OBJ && v.u.o->kind == O_STR; }
static int is_arr(Val v) { return v.t == T_OBJ && v.u.o->kind == O_ARRAY; }
static int is_fn(Val v) { return v.t == T_OBJ && (v.u.o->kind == O_FUNC || v.u.o->kind == O_NATIVE); }

/* ---------- proprietati ---------- */
static Val *find_prop(Obj *o, const char *key)
{
    for (int i = 0; i < o->np; i++)
        if (strcmp(o->props[i].key, key) == 0) return &o->props[i].val;
    return 0;
}
static void set_own(JS *J, Obj *o, const char *key, Val val)
{
    Val *p = find_prop(o, key);
    if (p) { *p = val; return; }
    if (o->np >= o->pcap) {
        int nc = o->pcap ? o->pcap * 2 : 4;
        struct Prop *np = (struct Prop *)aalloc(J, nc * sizeof(struct Prop));
        for (int i = 0; i < o->np; i++) np[i] = o->props[i];
        o->props = np; o->pcap = nc;
    }
    o->props[o->np].key = adup(J, key, (int)strlen(key));
    o->props[o->np].val = val;
    o->np++;
}

/* ---------- environment ---------- */
static Env *newenv(JS *J, Env *parent)
{
    Env *e = (Env *)aalloc(J, sizeof(Env));
    e->parent = parent; e->v = 0; e->nv = 0; e->vcap = 0;
    return e;
}
static Val *env_find(Env *e, const char *k)
{
    for (; e; e = e->parent)
        for (int i = 0; i < e->nv; i++)
            if (strcmp(e->v[i].key, k) == 0) return &e->v[i].val;
    return 0;
}
static void env_def(JS *J, Env *e, const char *k, Val val)
{
    for (int i = 0; i < e->nv; i++)
        if (strcmp(e->v[i].key, k) == 0) { e->v[i].val = val; return; }
    if (e->nv >= e->vcap) {
        int nc = e->vcap ? e->vcap * 2 : 8;
        struct Prop *nv = (struct Prop *)aalloc(J, nc * sizeof(struct Prop));
        for (int i = 0; i < e->nv; i++) nv[i] = e->v[i];
        e->v = nv; e->vcap = nc;
    }
    e->v[e->nv].key = adup(J, k, (int)strlen(k));
    e->v[e->nv].val = val;
    e->nv++;
}

/* ---------- lexer ---------- */
static int kw(const char *s, int n)
{
    struct { const char *w; int t; } t[] = {
        {"var",K_VAR},{"let",K_VAR},{"const",K_VAR},{"function",K_FUNCTION},
        {"return",K_RETURN},{"if",K_IF},{"else",K_ELSE},{"for",K_FOR},
        {"while",K_WHILE},{"do",K_DO},{"break",K_BREAK},{"continue",K_CONTINUE},
        {"true",K_TRUE},{"false",K_FALSE},{"null",K_NULL},{"undefined",K_UNDEF},
        {"new",K_NEW},{"typeof",K_TYPEOF},{"this",K_THIS},{"in",K_IN},{"of",K_OF},
    };
    for (unsigned i = 0; i < sizeof(t)/sizeof(t[0]); i++) {
        const char *w = t[i].w; int L = 0; while (w[L]) L++;
        if (L == n && memcmp(w, s, n) == 0) return t[i].t;
    }
    return 0;
}

static void lex(JS *J)
{
    const char *s = J->src; int n = J->slen;
    int i = J->pos;
    J->prev_end = J->pos;
    for (;;) {
        while (i < n && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
        if (i + 1 < n && s[i]=='/' && s[i+1]=='/') { i += 2; while (i<n && s[i]!='\n') i++; continue; }
        if (i + 1 < n && s[i]=='/' && s[i+1]=='*') { i += 2; while (i+1<n && !(s[i]=='*'&&s[i+1]=='/')) i++; i += 2; continue; }
        break;
    }
    J->prev_end = i;
    if (i >= n) { J->tk = K_EOF; J->pos = i; return; }
    char c = s[i];
    /* numar */
    if ((c >= '0' && c <= '9') || (c == '.' && i+1<n && s[i+1]>='0' && s[i+1]<='9')) {
        int st = i; double val = 0;
        if (c=='0' && i+1<n && (s[i+1]=='x'||s[i+1]=='X')) {
            i += 2; while (i<n) { int h; char d=s[i]; if(d>='0'&&d<='9')h=d-'0'; else if((d|32)>='a'&&(d|32)<='f')h=(d|32)-'a'+10; else break; val=val*16+h; i++; }
        } else {
            while (i<n && s[i]>='0'&&s[i]<='9') { val=val*10+(s[i]-'0'); i++; }
            if (i<n && s[i]=='.') { i++; double f=0.1; while(i<n&&s[i]>='0'&&s[i]<='9'){val+=(s[i]-'0')*f;f*=0.1;i++;} }
            if (i<n && (s[i]=='e'||s[i]=='E')) { i++; int sg=1; if(s[i]=='+')i++; else if(s[i]=='-'){sg=-1;i++;} int ex=0; while(i<n&&s[i]>='0'&&s[i]<='9'){ex=ex*10+(s[i]-'0');i++;} double p=1; for(int k=0;k<ex;k++)p*=10; if(sg<0)val/=p; else val*=p; }
        }
        (void)st; J->tnum = val; J->tk = K_NUM; J->pos = i; return;
    }
    /* string */
    if (c=='"' || c=='\'' || c=='`') {
        char q = c; i++;
        char buf[1024]; int bl = 0;
        while (i<n && s[i]!=q && bl<1023) {
            char d = s[i++];
            if (d=='\\' && i<n) {
                char e = s[i++];
                if (e=='n') d='\n'; else if(e=='t')d='\t'; else if(e=='r')d='\r';
                else if(e=='\\')d='\\'; else if(e=='"')d='"'; else if(e=='\'')d='\'';
                else if(e=='`')d='`'; else if(e=='/')d='/'; else if(e=='0')d=0;
                else if(e=='u'){ /* \uXXXX -> ignoram, punem '?' */ for(int k=0;k<4&&i<n;k++)i++; d='?'; }
                else d=e;
            }
            buf[bl++] = d;
        }
        if (i<n) i++;
        J->tstr = adup(J, buf, bl); J->tlen = bl; J->tk = K_STR; J->pos = i; return;
    }
    /* identificator / cuvant cheie */
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$') {
        int st = i;
        while (i<n && ((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='$')) i++;
        int L = i - st;
        int k = kw(s+st, L);
        if (k) { J->tk = k; J->pos = i; J->tstr = adup(J, s+st, L); J->tlen = L; return; }
        J->tstr = adup(J, s+st, L); J->tlen = L; J->tk = K_ID; J->pos = i; return;
    }
    /* operatori */
    #define M2(a,b,t) if(c==a && i+1<n && s[i+1]==b){ J->tk=t; J->pos=i+2; return; }
    #define M3(a,b,cc,t) if(c==a && i+2<n && s[i+1]==b && s[i+2]==cc){ J->tk=t; J->pos=i+3; return; }
    M3('=','=','=',K_SEQ); M3('!','=','=',K_SNE);
    M2('=','=',K_EQ); M2('!','=',K_NE); M2('<','=',K_LE); M2('>','=',K_GE);
    M2('&','&',K_AND); M2('|','|',K_OR); M2('+','+',K_INC); M2('-','-',K_DEC);
    M2('+','=',K_PLUSEQ); M2('-','=',K_MINUSEQ); M2('*','=',K_STAREQ); M2('/','=',K_SLASHEQ);
    M2('=','>',K_ARROW);
    int t = K_EOF;
    switch (c) {
        case '(':t=K_LP;break; case ')':t=K_RP;break; case '[':t=K_LB;break;
        case ']':t=K_RB;break; case '{':t=K_LC;break; case '}':t=K_RC;break;
        case ';':t=K_SEMI;break; case ',':t=K_COMMA;break; case '.':t=K_DOT;break;
        case ':':t=K_COLON;break; case '?':t=K_QUEST;break; case '=':t=K_ASSIGN;break;
        case '+':t=K_PLUS;break; case '-':t=K_MINUS;break; case '*':t=K_STAR;break;
        case '/':t=K_SLASH;break; case '%':t=K_PCT;break; case '<':t=K_LT;break;
        case '>':t=K_GT;break; case '!':t=K_NOT;break;
        case '&':t=K_AND;break; case '|':t=K_OR;break;   /* bit -> logic (aprox) */
        default: t=K_EOF; break;
    }
    J->tk = t; J->pos = i + 1;
}

/* helper: token curent memorat; folosim un mic buffer de un token in avans */
static void nexttok(JS *J) { lex(J); }

/* ---------- parser ---------- */
static Node *nnode(JS *J, int k)
{
    Node *nd = (Node *)aalloc(J, sizeof(Node));
    memset(nd, 0, sizeof(*nd));
    nd->k = k;
    return nd;
}
static void perr(JS *J, const char *m)
{
    if (!J->haserr) { J->haserr = 1; strcpy(J->err, m); }
}

/* tokenul curent e in J->tk; consumam cu advance() */
static void advance(JS *J) { nexttok(J); }
static int accept(JS *J, int t) { if (J->tk == t) { advance(J); return 1; } return 0; }
static void expect(JS *J, int t) { if (!accept(J, t)) perr(J, "simbol asteptat lipsa"); }

static Node *parse_expr(JS *J);
static Node *parse_assign(JS *J);
static Node *parse_stmt(JS *J);
static Node *parse_block(JS *J);

static Node *parse_params_arrow(JS *J, Node **params, int *np);

static Node *parse_primary(JS *J)
{
    Node *nd;
    switch (J->tk) {
    case K_NUM: nd = nnode(J, N_NUM); nd->num = J->tnum; advance(J); return nd;
    case K_STR: nd = nnode(J, N_STR); nd->str = J->tstr; advance(J); return nd;
    case K_TRUE: nd = nnode(J, N_BOOL); nd->num = 1; advance(J); return nd;
    case K_FALSE: nd = nnode(J, N_BOOL); nd->num = 0; advance(J); return nd;
    case K_NULL: nd = nnode(J, N_NULL); advance(J); return nd;
    case K_UNDEF: nd = nnode(J, N_UNDEF); advance(J); return nd;
    case K_THIS: nd = nnode(J, N_THIS); advance(J); return nd;
    case K_ID: {
        char *name = J->tstr;
        advance(J);
        if (J->tk == K_ARROW) {            /* x => expr */
            advance(J);
            nd = nnode(J, N_FUNC);
            nd->keys = (char **)aalloc(J, sizeof(char*));
            nd->keys[0] = name; nd->nlist = 1;
            if (J->tk == K_LC) nd->a = parse_block(J);
            else { Node *r = nnode(J, N_RET); r->a = parse_assign(J); nd->a = r; }
            return nd;
        }
        nd = nnode(J, N_IDENT); nd->str = name; return nd;
    }
    case K_LP: {
        /* fie grupare, fie parametri de arrow: (a,b)=>... */
        int save = J->pos, stk = J->tk;
        char *sstr = J->tstr; double snum = J->tnum;
        /* incercam sa citim o lista de identificatori urmata de )=> */
        advance(J);
        char *pnames[16]; int pn = 0; int arrow = 1;
        if (J->tk == K_RP) { advance(J); }
        else {
            while (1) {
                if (J->tk != K_ID) { arrow = 0; break; }
                if (pn < 16) pnames[pn++] = J->tstr;
                advance(J);
                if (J->tk == K_COMMA) { advance(J); continue; }
                if (J->tk == K_RP) { advance(J); break; }
                arrow = 0; break;
            }
        }
        if (arrow && J->tk == K_ARROW) {
            advance(J);
            nd = nnode(J, N_FUNC);
            nd->keys = (char **)aalloc(J, pn * sizeof(char*) + 8);
            for (int i = 0; i < pn; i++) nd->keys[i] = pnames[i];
            nd->nlist = pn;
            if (J->tk == K_LC) nd->a = parse_block(J);
            else { Node *r = nnode(J, N_RET); r->a = parse_assign(J); nd->a = r; }
            return nd;
        }
        /* nu era arrow: reluam ca grupare */
        J->pos = save; J->tk = stk; J->tstr = sstr; J->tnum = snum;
        advance(J);
        Node *e = parse_expr(J);
        expect(J, K_RP);
        return e;
    }
    case K_LB: {                            /* array literal */
        advance(J);
        nd = nnode(J, N_ARR);
        Node *items[256]; int cnt = 0;
        while (J->tk != K_RB && J->tk != K_EOF) {
            if (cnt < 256) items[cnt++] = parse_assign(J);
            else parse_assign(J);
            if (!accept(J, K_COMMA)) break;
        }
        expect(J, K_RB);
        nd->list = (Node **)aalloc(J, (cnt?cnt:1) * sizeof(Node*));
        for (int i = 0; i < cnt; i++) nd->list[i] = items[i];
        nd->nlist = cnt;
        return nd;
    }
    case K_LC: {                            /* object literal */
        advance(J);
        nd = nnode(J, N_OBJ);
        char *keys[128]; Node *vals[128]; int cnt = 0;
        while (J->tk != K_RC && J->tk != K_EOF) {
            char *key = 0;
            if (J->tk == K_STR || J->tk == K_ID || (J->tk >= K_VAR && J->tk <= K_OF))
                { key = J->tstr; advance(J); }
            else if (J->tk == K_NUM) { static char nb[24]; int v=(int)J->tnum; int p=0; char tmp[16]; int ti=0; if(v==0)tmp[ti++]='0'; while(v){tmp[ti++]=(char)('0'+v%10);v/=10;} while(ti)nb[p++]=tmp[--ti]; nb[p]=0; key=adup(J,nb,p); advance(J); }
            else break;
            expect(J, K_COLON);
            if (cnt < 128) { keys[cnt] = key; vals[cnt] = parse_assign(J); cnt++; }
            else parse_assign(J);
            if (!accept(J, K_COMMA)) break;
        }
        expect(J, K_RC);
        nd->keys = (char **)aalloc(J, (cnt?cnt:1) * sizeof(char*));
        nd->list = (Node **)aalloc(J, (cnt?cnt:1) * sizeof(Node*));
        for (int i = 0; i < cnt; i++) { nd->keys[i] = keys[i]; nd->list[i] = vals[i]; }
        nd->nlist = cnt;
        return nd;
    }
    case K_FUNCTION: {
        advance(J);
        if (J->tk == K_ID) advance(J);      /* nume optional (ignorat) */
        nd = nnode(J, N_FUNC);
        expect(J, K_LP);
        char *pnames[16]; int pn = 0;
        while (J->tk != K_RP && J->tk != K_EOF) {
            if (J->tk == K_ID && pn < 16) pnames[pn++] = J->tstr;
            advance(J);
            if (!accept(J, K_COMMA)) break;
        }
        expect(J, K_RP);
        nd->keys = (char **)aalloc(J, (pn?pn:1) * sizeof(char*));
        for (int i = 0; i < pn; i++) nd->keys[i] = pnames[i];
        nd->nlist = pn;
        nd->a = parse_block(J);
        return nd;
    }
    case K_NEW: {
        advance(J);
        nd = nnode(J, N_NEW);
        nd->a = parse_primary(J);           /* constructor (simplificat) */
        return nd;
    }
    default:
        perr(J, "expresie invalida");
        advance(J);
        return nnode(J, N_UNDEF);
    }
}

static Node *parse_postfix(JS *J)
{
    Node *e = parse_primary(J);
    for (;;) {
        if (J->tk == K_DOT) {
            advance(J);
            Node *m = nnode(J, N_MEMBER);
            m->a = e; m->str = J->tstr;
            advance(J);
            e = m;
        } else if (J->tk == K_LB) {
            advance(J);
            Node *m = nnode(J, N_INDEX);
            m->a = e; m->b = parse_expr(J);
            expect(J, K_RB);
            e = m;
        } else if (J->tk == K_LP) {
            advance(J);
            Node *c = nnode(J, N_CALL);
            c->a = e;
            Node *args[32]; int na = 0;
            while (J->tk != K_RP && J->tk != K_EOF) {
                if (na < 32) args[na++] = parse_assign(J);
                else parse_assign(J);
                if (!accept(J, K_COMMA)) break;
            }
            expect(J, K_RP);
            c->list = (Node **)aalloc(J, (na?na:1) * sizeof(Node*));
            for (int i = 0; i < na; i++) c->list[i] = args[i];
            c->nlist = na;
            e = c;
        } else if (J->tk == K_INC || J->tk == K_DEC) {
            Node *p = nnode(J, N_POST); p->op = J->tk; p->a = e; advance(J); e = p;
        } else break;
    }
    return e;
}

static Node *parse_unary(JS *J)
{
    if (J->tk == K_NOT || J->tk == K_MINUS || J->tk == K_PLUS ||
        J->tk == K_TYPEOF || J->tk == K_INC || J->tk == K_DEC) {
        Node *u = nnode(J, N_UN); u->op = J->tk; advance(J); u->a = parse_unary(J);
        return u;
    }
    return parse_postfix(J);
}

static int binprec(int t)
{
    switch (t) {
    case K_STAR: case K_SLASH: case K_PCT: return 7;
    case K_PLUS: case K_MINUS: return 6;
    case K_LT: case K_GT: case K_LE: case K_GE: case K_IN: return 5;
    case K_EQ: case K_NE: case K_SEQ: case K_SNE: return 4;
    case K_AND: return 3;
    case K_OR: return 2;
    default: return 0;
    }
}

static Node *parse_bin(JS *J, int minp)
{
    Node *left = parse_unary(J);
    for (;;) {
        int t = J->tk, p = binprec(t);
        if (p == 0 || p < minp) break;
        advance(J);
        Node *right = parse_bin(J, p + 1);
        Node *b = nnode(J, (t == K_AND || t == K_OR) ? N_LOGIC : N_BIN);
        b->op = t; b->a = left; b->b = right;
        left = b;
    }
    return left;
}

static Node *parse_cond(JS *J)
{
    Node *c = parse_bin(J, 1);
    if (J->tk == K_QUEST) {
        advance(J);
        Node *t = nnode(J, N_COND);
        t->a = c; t->b = parse_assign(J);
        expect(J, K_COLON);
        t->c = parse_assign(J);
        return t;
    }
    return c;
}

static Node *parse_assign(JS *J)
{
    Node *l = parse_cond(J);
    if (J->tk == K_ASSIGN || J->tk == K_PLUSEQ || J->tk == K_MINUSEQ ||
        J->tk == K_STAREQ || J->tk == K_SLASHEQ) {
        int op = J->tk; advance(J);
        Node *a = nnode(J, N_ASSIGN); a->op = op; a->a = l; a->b = parse_assign(J);
        return a;
    }
    return l;
}

static Node *parse_expr(JS *J)
{
    Node *e = parse_assign(J);
    /* operatorul virgula: pastram doar ultima (rar folosit in expr statement) */
    while (J->tk == K_COMMA) { advance(J); e = parse_assign(J); }
    return e;
}

static Node *parse_block(JS *J)
{
    Node *b = nnode(J, N_BLOCK);
    b->list = (Node **)aalloc(J, 512 * sizeof(Node*));   /* din arena, nu stiva */
    expect(J, K_LC);
    while (J->tk != K_RC && J->tk != K_EOF && !J->haserr) {
        Node *s = parse_stmt(J);
        if (b->nlist < 512) b->list[b->nlist++] = s;
    }
    expect(J, K_RC);
    return b;
}

static Node *parse_var(JS *J)
{
    advance(J);                              /* var/let/const */
    Node *v = nnode(J, N_VAR);
    char *names[16]; Node *inits[16]; int cnt = 0;
    while (J->tk == K_ID) {
        char *name = J->tstr; advance(J);
        Node *init = 0;
        if (accept(J, K_ASSIGN)) init = parse_assign(J);
        if (cnt < 16) { names[cnt] = name; inits[cnt] = init; cnt++; }
        if (!accept(J, K_COMMA)) break;
    }
    v->keys = (char **)aalloc(J, (cnt?cnt:1) * sizeof(char*));
    v->list = (Node **)aalloc(J, (cnt?cnt:1) * sizeof(Node*));
    for (int i = 0; i < cnt; i++) { v->keys[i] = names[i]; v->list[i] = inits[i]; }
    v->nlist = cnt;
    accept(J, K_SEMI);
    return v;
}

static Node *parse_stmt(JS *J)
{
    switch (J->tk) {
    case K_LC: return parse_block(J);
    case K_VAR: return parse_var(J);
    case K_SEMI: advance(J); return nnode(J, N_BLOCK);
    case K_IF: {
        advance(J); expect(J, K_LP);
        Node *nd = nnode(J, N_IF); nd->a = parse_expr(J);
        expect(J, K_RP);
        nd->b = parse_stmt(J);
        if (accept(J, K_ELSE)) nd->c = parse_stmt(J);
        return nd;
    }
    case K_WHILE: {
        advance(J); expect(J, K_LP);
        Node *nd = nnode(J, N_WHILE); nd->a = parse_expr(J);
        expect(J, K_RP); nd->b = parse_stmt(J);
        return nd;
    }
    case K_DO: {
        advance(J);
        Node *nd = nnode(J, N_DO); nd->b = parse_stmt(J);
        expect(J, K_WHILE); expect(J, K_LP); nd->a = parse_expr(J);
        expect(J, K_RP); accept(J, K_SEMI);
        return nd;
    }
    case K_FOR: {
        advance(J); expect(J, K_LP);
        /* detectam for-in: (var x in obj) sau (x in obj) */
        Node *nd;
        int save = J->pos, stk = J->tk; char *ss = J->tstr; double sn = J->tnum;
        int isvar = (J->tk == K_VAR);
        if (isvar) advance(J);
        if (J->tk == K_ID) {
            char *iv = J->tstr; advance(J);
            if (J->tk == K_IN || J->tk == K_OF) {
                advance(J);
                nd = nnode(J, N_FORIN);
                nd->str = iv;
                nd->a = parse_expr(J);
                expect(J, K_RP);
                nd->b = parse_stmt(J);
                return nd;
            }
        }
        /* nu e for-in: reluam ca for clasic */
        J->pos = save; J->tk = stk; J->tstr = ss; J->tnum = sn;
        nd = nnode(J, N_FOR);
        if (J->tk == K_VAR) nd->a = parse_var(J);
        else if (J->tk != K_SEMI) { Node *e = nnode(J, N_EXPR); e->a = parse_expr(J); nd->a = e; accept(J, K_SEMI); }
        else advance(J);
        if (J->tk != K_SEMI) nd->b = parse_expr(J);
        expect(J, K_SEMI);
        if (J->tk != K_RP) nd->c = parse_expr(J);
        expect(J, K_RP);
        nd->d = parse_stmt(J);
        return nd;
    }
    case K_RETURN: {
        advance(J);
        Node *nd = nnode(J, N_RET);
        if (J->tk != K_SEMI && J->tk != K_RC && J->tk != K_EOF) nd->a = parse_expr(J);
        accept(J, K_SEMI);
        return nd;
    }
    case K_BREAK: advance(J); accept(J, K_SEMI); return nnode(J, N_BREAK);
    case K_CONTINUE: advance(J); accept(J, K_SEMI); return nnode(J, N_CONT);
    case K_FUNCTION: {
        /* declaratie de functie: function nume(...) {...} */
        int save = J->pos;
        advance(J);
        if (J->tk == K_ID) {
            char *name = J->tstr;
            /* reconstruim ca var nume = function... */
            J->pos = save; J->tk = K_FUNCTION;   /* nu chiar; parse_primary se ocupa */
            /* mai simplu: parsam expresia functie si o legam de nume */
            advance(J);          /* function */
            advance(J);          /* nume */
            Node *fn = nnode(J, N_FUNC);
            expect(J, K_LP);
            char *pnames[16]; int pn = 0;
            while (J->tk != K_RP && J->tk != K_EOF) {
                if (J->tk == K_ID && pn < 16) pnames[pn++] = J->tstr;
                advance(J);
                if (!accept(J, K_COMMA)) break;
            }
            expect(J, K_RP);
            fn->keys = (char **)aalloc(J, (pn?pn:1)*sizeof(char*));
            for (int i=0;i<pn;i++) fn->keys[i]=pnames[i];
            fn->nlist = pn;
            fn->a = parse_block(J);
            Node *v = nnode(J, N_VAR);
            v->keys = (char **)aalloc(J, sizeof(char*));
            v->list = (Node **)aalloc(J, sizeof(Node*));
            v->keys[0] = name; v->list[0] = fn; v->nlist = 1;
            return v;
        }
        J->pos = save; J->tk = K_FUNCTION;
        Node *e = nnode(J, N_EXPR); e->a = parse_expr(J); accept(J, K_SEMI); return e;
    }
    default: {
        Node *e = nnode(J, N_EXPR); e->a = parse_expr(J); accept(J, K_SEMI); return e;
    }
    }
}

/* ---------- to-string / to-number / truthiness ---------- */
static void num_to_str(double d, char *out)
{
    if (d != d) { strcpy(out, "NaN"); return; }
    int neg = 0;
    if (d < 0) { neg = 1; d = -d; }
    /* intreg? */
    long long i = (long long)d;
    if ((double)i == d && d < 1e15) {
        char tmp[32]; int t = 0;
        if (i == 0) tmp[t++] = '0';
        long long v = i; while (v) { tmp[t++] = (char)('0'+v%10); v/=10; }
        int p = 0; if (neg) out[p++]='-';
        while (t) out[p++] = tmp[--t];
        out[p] = 0; return;
    }
    /* zecimal cu ~6 cifre */
    int p = 0; if (neg) out[p++]='-';
    long long ip = (long long)d;
    char tmp[32]; int t = 0; long long v = ip;
    if (v==0) tmp[t++]='0'; while (v){tmp[t++]=(char)('0'+v%10);v/=10;}
    while (t) out[p++] = tmp[--t];
    out[p++] = '.';
    double frac = d - (double)ip;
    for (int k = 0; k < 6; k++) { frac *= 10; int dg=(int)frac; out[p++]=(char)('0'+dg); frac-=dg; }
    while (p>0 && out[p-1]=='0') p--;
    if (out[p-1]=='.') p--;
    out[p]=0;
}

static Val to_str(JS *J, Val v);
static double to_num(Val v)
{
    switch (v.t) {
    case T_NUM: return v.u.n;
    case T_BOOL: return v.u.b;
    case T_NULL: return 0;
    case T_UNDEF: return 0.0/0.0;
    case T_OBJ:
        if (v.u.o->kind == O_STR) {
            const char *s = v.u.o->s; while(*s==' ')s++;
            if (*s==0) return 0;
            int neg=0; if(*s=='-'){neg=1;s++;} else if(*s=='+')s++;
            double d=0; int any=0;
            while(*s>='0'&&*s<='9'){d=d*10+(*s-'0');s++;any=1;}
            if(*s=='.'){s++;double f=0.1;while(*s>='0'&&*s<='9'){d+=(*s-'0')*f;f*=0.1;s++;any=1;}}
            if(!any) return 0.0/0.0;
            return neg?-d:d;
        }
        return 0.0/0.0;
    }
    return 0.0/0.0;
}
static int truthy(Val v)
{
    switch (v.t) {
    case T_UNDEF: case T_NULL: return 0;
    case T_BOOL: return v.u.b;
    case T_NUM: return v.u.n != 0 && v.u.n == v.u.n;
    case T_OBJ: if (v.u.o->kind==O_STR) return v.u.o->slen>0; return 1;
    }
    return 0;
}

static Val eval(JS *J, Node *nd, Env *e);

static Val to_str(JS *J, Val v)
{
    char b[40];
    switch (v.t) {
    case T_UNDEF: return vcstr(J, "undefined");
    case T_NULL: return vcstr(J, "null");
    case T_BOOL: return vcstr(J, v.u.b ? "true" : "false");
    case T_NUM: num_to_str(v.u.n, b); return vcstr(J, b);
    case T_OBJ:
        if (v.u.o->kind == O_STR) return v;
        if (v.u.o->kind == O_ARRAY) {
            /* join cu virgula */
            char buf[1024]; int p = 0;
            for (int i = 0; i < v.u.o->nel && p < 1000; i++) {
                if (i) buf[p++] = ',';
                Val s = to_str(J, v.u.o->el[i]);
                for (int k = 0; k < s.u.o->slen && p < 1000; k++) buf[p++] = s.u.o->s[k];
            }
            buf[p]=0; return vstr(J, buf, p);
        }
        if (is_fn(v)) return vcstr(J, "function");
        return vcstr(J, "[object Object]");
    }
    return vcstr(J, "");
}

/* concatenare de string-uri C */
static Val str_cat(JS *J, Val a, Val b)
{
    Val sa = to_str(J, a), sb = to_str(J, b);
    int n = sa.u.o->slen + sb.u.o->slen;
    char *p = (char *)aalloc(J, n + 1);
    memcpy(p, sa.u.o->s, sa.u.o->slen);
    memcpy(p + sa.u.o->slen, sb.u.o->s, sb.u.o->slen);
    p[n] = 0;
    Obj *o = newobj(J, O_STR); o->s = p; o->slen = n;
    return vobj(o);
}

static int val_eq(JS *J, Val a, Val b, int strict)
{
    if (a.t == b.t) {
        if (a.t == T_OBJ) {
            if (is_str(a) && is_str(b))
                return a.u.o->slen==b.u.o->slen && memcmp(a.u.o->s,b.u.o->s,a.u.o->slen)==0;
            return a.u.o == b.u.o;
        }
        if (a.t == T_NUM) return a.u.n == b.u.n;
        if (a.t == T_BOOL) return a.u.b == b.u.b;
        return 1; /* undef==undef, null==null */
    }
    if (strict) return 0;
    if ((a.t==T_NULL&&b.t==T_UNDEF)||(a.t==T_UNDEF&&b.t==T_NULL)) return 1;
    /* comparatie laxa numerica/string */
    if (is_str(a) && b.t==T_NUM) return to_num(a)==b.u.n;
    if (a.t==T_NUM && is_str(b)) return a.u.n==to_num(b);
    if (a.t==T_BOOL||b.t==T_BOOL) return to_num(a)==to_num(b);
    (void)J;
    return 0;
}

/* apel de functie */
static Val call_fn(JS *J, Val fnv, Val thisv, Val *args, int nargs)
{
    if (J->haserr) return vundef();
    if (!is_fn(fnv)) { perr(J, "apel pe non-functie"); return vundef(); }
    Obj *f = fnv.u.o;
    if (f->kind == O_NATIVE) return f->nat(J, f, thisv, args, nargs);
    if (J->depth >= DEPTH_LIMIT) { perr(J, "recursie prea adanca"); return vundef(); }
    J->depth++;
    Env *fe = newenv(J, f->env);
    /* argumente */
    Node *fn = f->fn;
    for (int i = 0; i < fn->nlist; i++)
        env_def(J, fe, fn->keys[i], i < nargs ? args[i] : vundef());
    env_def(J, fe, "this", thisv);
    /* arguments array (simplu) */
    int save = J->comp; Val savret = J->retv;
    J->comp = 0; J->retv = vundef();
    eval(J, fn->a, fe);
    Val r = (J->comp == 1) ? J->retv : vundef();
    J->comp = save; J->retv = savret;
    J->depth--;
    return r;
}

/* ---------- proprietati / metode built-in ---------- */
typedef Val (*NatFn)(JS *, Obj *, Val, Val *, int);
static Val make_native(JS *J, NatFn fn)
{
    Obj *o = newobj(J, O_NATIVE); o->nat = fn; return vobj(o);
}

/* metode string/array: un singur native de dispatch, numele in self->s */
static Val nat_str_dispatch(JS *J, Obj *self, Val thisv, Val *a, int na);
static Val nat_arr_dispatch(JS *J, Obj *self, Val thisv, Val *a, int na);

static Val bound_method(JS *J, Val thisv, const char *name, int is_arr_m)
{
    (void)thisv;
    Obj *o = newobj(J, O_NATIVE);
    o->nat = is_arr_m ? nat_arr_dispatch : nat_str_dispatch;
    o->s = adup(J, name, (int)strlen(name));
    return vobj(o);
}

static Val to_str_key(JS *J, Val k, char *buf, int cap)
{
    Val s = to_str(J, k);
    int n = s.u.o->slen; if (n > cap-1) n = cap-1;
    memcpy(buf, s.u.o->s, n); buf[n]=0;
    return s;
}

static Val dom_prop_get(JS *J, Obj *o, const char *key);
static void dom_prop_set(JS *J, Obj *o, const char *key, Val val);
static void style_flush(JS *J, Obj *styleobj);

static Val get_prop(JS *J, Val obj, const char *key)
{
    if (obj.t == T_OBJ) {
        Obj *o = obj.u.o;
        if (o->kind == O_STR) {
            if (strcmp(key, "length") == 0) return vnum(o->slen);
            return bound_method(J, obj, key, 0);
        }
        if (o->kind == O_ARRAY) {
            if (strcmp(key, "length") == 0) return vnum(o->nel);
            /* index numeric ca proprietate? */
            int allnum = key[0]!=0; for (const char*p=key;*p;p++) if(*p<'0'||*p>'9'){allnum=0;break;}
            if (allnum) { int idx = 0; for(const char*p=key;*p;p++)idx=idx*10+(*p-'0'); if(idx>=0&&idx<o->nel) return o->el[idx]; return vundef(); }
            Val *bp = find_prop(o, key);
            if (bp) return *bp;
            return bound_method(J, obj, key, 1);
        }
        if (o->kind == O_DOM) return dom_prop_get(J, o, key);
        Val *p = find_prop(o, key);
        if (p) return *p;
        return vundef();
    }
    if (obj.t == T_NUM || obj.t == T_BOOL) {
        /* toString etc pe numar - minim */
        if (strcmp(key,"toString")==0) return bound_method(J, obj, "toString", 0);
    }
    return vundef();
}

static void set_prop(JS *J, Val obj, const char *key, Val val)
{
    if (obj.t != T_OBJ) return;
    Obj *o = obj.u.o;
    if (o->kind == O_ARRAY) {
        int allnum = key[0]!=0; for (const char*p=key;*p;p++) if(*p<'0'||*p>'9'){allnum=0;break;}
        if (allnum) {
            int idx=0; for(const char*p=key;*p;p++)idx=idx*10+(*p-'0');
            if (idx>=0 && idx<4096) {
                while (o->nel <= idx) {
                    if (o->nel >= o->elcap) { int nc=o->elcap?o->elcap*2:8; Val*ne=(Val*)aalloc(J,nc*sizeof(Val)); for(int i=0;i<o->nel;i++)ne[i]=o->el[i]; o->el=ne;o->elcap=nc; }
                    o->el[o->nel++] = vundef();
                }
                o->el[idx] = val;
            }
            return;
        }
        if (strcmp(key,"length")==0) { int L=(int)to_num(val); if(L>=0&&L<=o->nel) o->nel=L; return; }
        set_own(J, o, key, val);
        return;
    }
    if (o->kind == O_DOM) { dom_prop_set(J, o, key, val); return; }
    set_own(J, o, key, val);
    if (o->kind == O_OBJECT && find_prop(o, "__isstyle")) style_flush(J, o);
}

/* ---------- evaluator ---------- */
static Val eval_member_this(JS *J, Node *nd, Env *e, Val *thisout);

static Val eval(JS *J, Node *nd, Env *e)
{
    if (J->haserr || !nd) return vundef();
    if (++J->steps > STEP_LIMIT) { perr(J, "prea multe operatii (bucla infinita?)"); return vundef(); }

    switch (nd->k) {
    case N_NUM: return vnum(nd->num);
    case N_STR: return vstr(J, nd->str, (int)strlen(nd->str));
    case N_BOOL: return vbool((int)nd->num);
    case N_NULL: return vnull();
    case N_UNDEF: return vundef();
    case N_THIS: { Val *p = env_find(e, "this"); return p ? *p : vundef(); }
    case N_IDENT: {
        Val *p = env_find(e, nd->str);
        if (p) return *p;
        return vundef();
    }
    case N_ARR: {
        Obj *o = newobj(J, O_ARRAY);
        o->el = (Val *)aalloc(J, (nd->nlist?nd->nlist:1)*sizeof(Val));
        o->elcap = nd->nlist; o->nel = nd->nlist;
        for (int i = 0; i < nd->nlist; i++) o->el[i] = eval(J, nd->list[i], e);
        return vobj(o);
    }
    case N_OBJ: {
        Obj *o = newobj(J, O_OBJECT);
        for (int i = 0; i < nd->nlist; i++)
            set_own(J, o, nd->keys[i], eval(J, nd->list[i], e));
        return vobj(o);
    }
    case N_FUNC: {
        Obj *o = newobj(J, O_FUNC); o->fn = nd; o->env = e; return vobj(o);
    }
    case N_UN: {
        if (nd->op == K_TYPEOF) {
            Val v = eval(J, nd->a, e);
            const char *t = "undefined";
            if (v.t==T_NUM)t="number"; else if(v.t==T_BOOL)t="boolean";
            else if(v.t==T_NULL)t="object"; else if(is_str(v))t="string";
            else if(is_fn(v))t="function"; else if(v.t==T_OBJ)t="object";
            return vcstr(J, t);
        }
        if (nd->op == K_INC || nd->op == K_DEC) {   /* prefix */
            Val v = eval(J, nd->a, e);
            double d = to_num(v) + (nd->op==K_INC?1:-1);
            /* asignam inapoi */
            Node asg; memset(&asg,0,sizeof(asg)); /* nu folosim; setam direct */
            /* setam prin cai: identificator / member / index */
            if (nd->a->k == N_IDENT) { Val *p=env_find(e,nd->a->str); if(p)*p=vnum(d); else env_def(J,J->global,nd->a->str,vnum(d)); }
            else if (nd->a->k == N_MEMBER) { Val ob=eval(J,nd->a->a,e); set_prop(J,ob,nd->a->str,vnum(d)); }
            else if (nd->a->k == N_INDEX) { Val ob=eval(J,nd->a->a,e); char kb[24]; to_str_key(J, eval(J,nd->a->b,e), kb, sizeof(kb)); set_prop(J,ob,kb,vnum(d)); }
            return vnum(d);
        }
        Val v = eval(J, nd->a, e);
        if (nd->op == K_NOT) return vbool(!truthy(v));
        if (nd->op == K_MINUS) return vnum(-to_num(v));
        if (nd->op == K_PLUS) return vnum(to_num(v));
        return vundef();
    }
    case N_POST: {
        Val v = eval(J, nd->a, e);
        double old = to_num(v);
        double d = old + (nd->op==K_INC?1:-1);
        if (nd->a->k == N_IDENT) { Val *p=env_find(e,nd->a->str); if(p)*p=vnum(d); else env_def(J,J->global,nd->a->str,vnum(d)); }
        else if (nd->a->k == N_MEMBER) { Val ob=eval(J,nd->a->a,e); set_prop(J,ob,nd->a->str,vnum(d)); }
        else if (nd->a->k == N_INDEX) { Val ob=eval(J,nd->a->a,e); char kb[24]; to_str_key(J,eval(J,nd->a->b,e),kb,sizeof(kb)); set_prop(J,ob,kb,vnum(d)); }
        return vnum(old);
    }
    case N_BIN: {
        Val a = eval(J, nd->a, e), b = eval(J, nd->b, e);
        switch (nd->op) {
        case K_PLUS:
            if (is_str(a) || is_str(b)) return str_cat(J, a, b);
            return vnum(to_num(a) + to_num(b));
        case K_MINUS: return vnum(to_num(a) - to_num(b));
        case K_STAR: return vnum(to_num(a) * to_num(b));
        case K_SLASH: return vnum(to_num(a) / to_num(b));
        case K_PCT: { double x=to_num(a),y=to_num(b); long long q=(long long)(x/y); return vnum(x-(double)q*y); }
        case K_LT: if(is_str(a)&&is_str(b))return vbool(strcmp(a.u.o->s,b.u.o->s)<0); return vbool(to_num(a)<to_num(b));
        case K_GT: if(is_str(a)&&is_str(b))return vbool(strcmp(a.u.o->s,b.u.o->s)>0); return vbool(to_num(a)>to_num(b));
        case K_LE: if(is_str(a)&&is_str(b))return vbool(strcmp(a.u.o->s,b.u.o->s)<=0); return vbool(to_num(a)<=to_num(b));
        case K_GE: if(is_str(a)&&is_str(b))return vbool(strcmp(a.u.o->s,b.u.o->s)>=0); return vbool(to_num(a)>=to_num(b));
        case K_EQ: return vbool(val_eq(J,a,b,0));
        case K_NE: return vbool(!val_eq(J,a,b,0));
        case K_SEQ: return vbool(val_eq(J,a,b,1));
        case K_SNE: return vbool(!val_eq(J,a,b,1));
        case K_IN: if(b.t==T_OBJ){ char kb[64]; to_str_key(J,a,kb,sizeof(kb)); return vbool(find_prop(b.u.o,kb)!=0);} return vbool(0);
        }
        return vundef();
    }
    case N_LOGIC: {
        Val a = eval(J, nd->a, e);
        if (nd->op == K_AND) return truthy(a) ? eval(J, nd->b, e) : a;
        return truthy(a) ? a : eval(J, nd->b, e);
    }
    case N_COND:
        return truthy(eval(J, nd->a, e)) ? eval(J, nd->b, e) : eval(J, nd->c, e);
    case N_ASSIGN: {
        Val rhs = eval(J, nd->b, e);
        if (nd->op != K_ASSIGN) {
            Val cur = eval(J, nd->a, e);
            if (nd->op==K_PLUSEQ) rhs = is_str(cur)||is_str(rhs) ? str_cat(J,cur,rhs) : vnum(to_num(cur)+to_num(rhs));
            else if (nd->op==K_MINUSEQ) rhs = vnum(to_num(cur)-to_num(rhs));
            else if (nd->op==K_STAREQ) rhs = vnum(to_num(cur)*to_num(rhs));
            else if (nd->op==K_SLASHEQ) rhs = vnum(to_num(cur)/to_num(rhs));
        }
        Node *t = nd->a;
        if (t->k == N_IDENT) {
            Val *p = env_find(e, t->str);
            if (p) *p = rhs; else env_def(J, J->global, t->str, rhs);
        } else if (t->k == N_MEMBER) {
            Val ob = eval(J, t->a, e); set_prop(J, ob, t->str, rhs);
        } else if (t->k == N_INDEX) {
            Val ob = eval(J, t->a, e); char kb[64]; to_str_key(J, eval(J,t->b,e), kb, sizeof(kb)); set_prop(J, ob, kb, rhs);
        }
        return rhs;
    }
    case N_MEMBER: {
        Val ob = eval(J, nd->a, e);
        return get_prop(J, ob, nd->str);
    }
    case N_INDEX: {
        Val ob = eval(J, nd->a, e);
        Val k = eval(J, nd->b, e);
        if (is_str(ob) && k.t==T_NUM) { int i=(int)k.u.n; if(i>=0&&i<ob.u.o->slen) return vstr(J,ob.u.o->s+i,1); return vundef(); }
        char kb[64]; to_str_key(J, k, kb, sizeof(kb));
        return get_prop(J, ob, kb);
    }
    case N_CALL: {
        Val thisv = vundef();
        Val fnv;
        if (nd->a->k == N_MEMBER) {
            Val ob = eval(J, nd->a->a, e); thisv = ob;
            fnv = get_prop(J, ob, nd->a->str);
        } else if (nd->a->k == N_INDEX) {
            Val ob = eval(J, nd->a->a, e); thisv = ob;
            char kb[64]; to_str_key(J, eval(J,nd->a->b,e), kb, sizeof(kb));
            fnv = get_prop(J, ob, kb);
        } else fnv = eval(J, nd->a, e);
        Val args[32]; int na = nd->nlist < 32 ? nd->nlist : 32;
        for (int i = 0; i < na; i++) args[i] = eval(J, nd->list[i], e);
        return call_fn(J, fnv, thisv, args, na);
    }
    case N_NEW: {
        /* new C(args): cream un obiect, rulam constructorul cu this=obiect */
        Obj *o = newobj(J, O_OBJECT);
        Val thisv = vobj(o);
        Val fnv;
        if (nd->a->k == N_CALL) { fnv = eval(J, nd->a->a, e); Val args[16]; int na=nd->a->nlist<16?nd->a->nlist:16; for(int i=0;i<na;i++)args[i]=eval(J,nd->a->list[i],e); call_fn(J,fnv,thisv,args,na); }
        else { fnv = eval(J, nd->a, e); call_fn(J, fnv, thisv, 0, 0); }
        return thisv;
    }
    /* --- statements --- */
    case N_BLOCK: case N_PROG: {
        for (int i = 0; i < nd->nlist; i++) {
            eval(J, nd->list[i], e);
            if (J->comp || J->haserr) break;
        }
        return vundef();
    }
    case N_VAR: {
        for (int i = 0; i < nd->nlist; i++) {
            Val val = nd->list[i] ? eval(J, nd->list[i], e) : vundef();
            env_def(J, e, nd->keys[i], val);
        }
        return vundef();
    }
    case N_EXPR: return eval(J, nd->a, e);
    case N_IF:
        if (truthy(eval(J, nd->a, e))) eval(J, nd->b, e);
        else if (nd->c) eval(J, nd->c, e);
        return vundef();
    case N_WHILE:
        while (!J->haserr && truthy(eval(J, nd->a, e))) {
            eval(J, nd->b, e);
            if (J->comp == 2) { J->comp = 0; break; }
            if (J->comp == 3) { J->comp = 0; continue; }
            if (J->comp) break;
            if (++J->steps > STEP_LIMIT) { perr(J,"bucla infinita"); break; }
        }
        return vundef();
    case N_DO:
        do {
            eval(J, nd->b, e);
            if (J->comp == 2) { J->comp = 0; break; }
            if (J->comp == 3) { J->comp = 0; }
            else if (J->comp) break;
            if (++J->steps > STEP_LIMIT) { perr(J,"bucla infinita"); break; }
        } while (!J->haserr && truthy(eval(J, nd->a, e)));
        return vundef();
    case N_FOR: {
        Env *fe = newenv(J, e);
        if (nd->a) eval(J, nd->a, fe);
        while (!J->haserr && (!nd->b || truthy(eval(J, nd->b, fe)))) {
            eval(J, nd->d, fe);
            if (J->comp == 2) { J->comp = 0; break; }
            if (J->comp == 3) J->comp = 0;
            else if (J->comp) break;
            if (nd->c) eval(J, nd->c, fe);
            if (++J->steps > STEP_LIMIT) { perr(J,"bucla infinita"); break; }
        }
        return vundef();
    }
    case N_FORIN: {
        Val ob = eval(J, nd->a, e);
        Env *fe = newenv(J, e);
        if (ob.t == T_OBJ && ob.u.o->kind == O_ARRAY) {
            for (int i = 0; i < ob.u.o->nel && !J->haserr; i++) {
                char kb[16]; num_to_str(i, kb);
                env_def(J, fe, nd->str, vcstr(J, kb));   /* for-in da indici (string) */
                eval(J, nd->b, fe);
                if (J->comp==2){J->comp=0;break;} if(J->comp==3)J->comp=0; else if(J->comp)break;
            }
        } else if (ob.t == T_OBJ) {
            for (int i = 0; i < ob.u.o->np && !J->haserr; i++) {
                env_def(J, fe, nd->str, vcstr(J, ob.u.o->props[i].key));
                eval(J, nd->b, fe);
                if (J->comp==2){J->comp=0;break;} if(J->comp==3)J->comp=0; else if(J->comp)break;
            }
        }
        return vundef();
    }
    case N_RET:
        J->retv = nd->a ? eval(J, nd->a, e) : vundef();
        J->comp = 1;
        return vundef();
    case N_BREAK: J->comp = 2; return vundef();
    case N_CONT: J->comp = 3; return vundef();
    }
    (void)eval_member_this;
    return vundef();
}
static Val eval_member_this(JS *J, Node *nd, Env *e, Val *t) { (void)J;(void)nd;(void)e;(void)t; return vundef(); }

/* ---------- biblioteca standard ---------- */
#include "js_lib.h"

/* ---------- API public ---------- */
static void install_globals(JS *J);

JS *js_create(void *arena, uint32_t bytes)
{
    JS *J = (JS *)arena;
    memset(J, 0, sizeof(*J));
    J->arena = (char *)arena;
    J->asz = bytes;
    J->aused = (sizeof(JS) + 7) & ~7;
    J->global = newenv(J, 0);
    install_globals(J);
    return J;
}

void js_reset(JS *J)
{
    uint32_t sz = J->asz;
    char *ar = J->arena;
    struct js_dom_ops *dom = J->dom;
    memset(J, 0, sizeof(*J));
    J->arena = ar; J->asz = sz;
    J->aused = (sizeof(JS) + 7) & ~7;
    J->global = newenv(J, 0);
    J->dom = dom;
    install_globals(J);
}

void js_set_dom(JS *J, struct js_dom_ops *ops) { J->dom = ops; }
const char *js_error(JS *J) { return J->err; }

int js_eval(JS *J, const char *src, int len)
{
    J->src = src; J->slen = len; J->pos = 0;
    J->haserr = 0; J->err[0] = 0; J->comp = 0; J->steps = 0; J->depth = 0;
    advance(J);
    Node *prog = nnode(J, N_PROG);
    prog->list = (Node **)aalloc(J, 4096 * sizeof(Node*));   /* din arena */
    while (J->tk != K_EOF && !J->haserr) {
        Node *s = parse_stmt(J);
        if (prog->nlist < 4096) prog->list[prog->nlist++] = s;
    }
    if (J->haserr) return -1;
    eval(J, prog, J->global);
    return J->haserr ? -1 : 0;
}
