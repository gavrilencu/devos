/* Biblioteca standard + legatura DOM pentru motorul JS. Inclusa in js.c,
 * are acces la toate tipurile si functiile statice de acolo. */

static int str_find(const char *h, int hl, const char *n, int nl, int from)
{
    if (from < 0) from = 0;
    if (nl == 0) return from <= hl ? from : hl;
    for (int i = from; i + nl <= hl; i++) {
        int j = 0;
        while (j < nl && h[i + j] == n[j]) j++;
        if (j == nl) return i;
    }
    return -1;
}

/* ---------- metode string ---------- */
static Val nat_str_dispatch(JS *J, Obj *self, Val thisv, Val *a, int na)
{
    const char *m = self->s;
    Val sv = to_str(J, thisv);
    const char *s = sv.u.o->s; int sl = sv.u.o->slen;

    if (strcmp(m, "charAt") == 0) {
        int i = na ? (int)to_num(a[0]) : 0;
        if (i >= 0 && i < sl) return vstr(J, s + i, 1);
        return vcstr(J, "");
    }
    if (strcmp(m, "charCodeAt") == 0) {
        int i = na ? (int)to_num(a[0]) : 0;
        if (i >= 0 && i < sl) return vnum((unsigned char)s[i]);
        return vnum(0.0 / 0.0);
    }
    if (strcmp(m, "indexOf") == 0) {
        Val nv = na ? to_str(J, a[0]) : vcstr(J, "");
        int from = na > 1 ? (int)to_num(a[1]) : 0;
        return vnum(str_find(s, sl, nv.u.o->s, nv.u.o->slen, from));
    }
    if (strcmp(m, "includes") == 0) {
        Val nv = na ? to_str(J, a[0]) : vcstr(J, "");
        return vbool(str_find(s, sl, nv.u.o->s, nv.u.o->slen, 0) >= 0);
    }
    if (strcmp(m, "startsWith") == 0) {
        Val nv = na ? to_str(J, a[0]) : vcstr(J, "");
        return vbool(nv.u.o->slen <= sl && memcmp(s, nv.u.o->s, nv.u.o->slen) == 0);
    }
    if (strcmp(m, "endsWith") == 0) {
        Val nv = na ? to_str(J, a[0]) : vcstr(J, "");
        int nl = nv.u.o->slen;
        return vbool(nl <= sl && memcmp(s + sl - nl, nv.u.o->s, nl) == 0);
    }
    if (strcmp(m, "toUpperCase") == 0 || strcmp(m, "toLowerCase") == 0) {
        int up = (m[2] == 'U');
        char *b = (char *)aalloc(J, sl + 1);
        for (int i = 0; i < sl; i++) {
            char c = s[i];
            if (up && c >= 'a' && c <= 'z') c -= 32;
            else if (!up && c >= 'A' && c <= 'Z') c += 32;
            b[i] = c;
        }
        b[sl] = 0;
        Obj *o = newobj(J, O_STR); o->s = b; o->slen = sl; return vobj(o);
    }
    if (strcmp(m, "substring") == 0 || strcmp(m, "slice") == 0) {
        int b = na ? (int)to_num(a[0]) : 0;
        int e = na > 1 ? (int)to_num(a[1]) : sl;
        if (m[0] == 's' && m[1] == 'l') {   /* slice: indici negativi */
            if (b < 0) b += sl; if (e < 0) e += sl;
        }
        if (b < 0) b = 0; if (e > sl) e = sl;
        if (b > e) { int t = b; b = e; e = t; }
        if (b < 0) b = 0; if (e < b) e = b;
        return vstr(J, s + b, e - b);
    }
    if (strcmp(m, "substr") == 0) {
        int b = na ? (int)to_num(a[0]) : 0;
        int len = na > 1 ? (int)to_num(a[1]) : sl - b;
        if (b < 0) b += sl; if (b < 0) b = 0;
        if (b > sl) b = sl; if (len < 0) len = 0;
        if (b + len > sl) len = sl - b;
        return vstr(J, s + b, len);
    }
    if (strcmp(m, "trim") == 0) {
        int b = 0, e = sl;
        while (b < e && (s[b]==' '||s[b]=='\t'||s[b]=='\n'||s[b]=='\r')) b++;
        while (e > b && (s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\n'||s[e-1]=='\r')) e--;
        return vstr(J, s + b, e - b);
    }
    if (strcmp(m, "concat") == 0) {
        Val r = sv;
        for (int i = 0; i < na; i++) r = str_cat(J, r, a[i]);
        return r;
    }
    if (strcmp(m, "repeat") == 0) {
        int cnt = na ? (int)to_num(a[0]) : 0;
        if (cnt < 0) cnt = 0; if (cnt > 10000) cnt = 10000;
        char *b = (char *)aalloc(J, sl * cnt + 1);
        int p = 0; for (int k = 0; k < cnt; k++) { memcpy(b+p, s, sl); p += sl; }
        b[p] = 0; Obj *o = newobj(J, O_STR); o->s = b; o->slen = p; return vobj(o);
    }
    if (strcmp(m, "split") == 0) {
        Obj *arr = newobj(J, O_ARRAY);
        Val sep = na ? to_str(J, a[0]) : vcstr(J, "");
        const char *d = sep.u.o->s; int dl = sep.u.o->slen;
        Val tmp[512]; int cnt = 0;
        if (dl == 0) {
            for (int i = 0; i < sl && cnt < 512; i++) tmp[cnt++] = vstr(J, s + i, 1);
        } else {
            int start = 0;
            for (;;) {
                int idx = str_find(s, sl, d, dl, start);
                if (idx < 0) { if (cnt < 512) tmp[cnt++] = vstr(J, s + start, sl - start); break; }
                if (cnt < 512) tmp[cnt++] = vstr(J, s + start, idx - start);
                start = idx + dl;
            }
        }
        arr->el = (Val *)aalloc(J, (cnt?cnt:1)*sizeof(Val));
        for (int i = 0; i < cnt; i++) arr->el[i] = tmp[i];
        arr->nel = cnt; arr->elcap = cnt;
        return vobj(arr);
    }
    if (strcmp(m, "replace") == 0) {
        Val nv = na ? to_str(J, a[0]) : vcstr(J, "");
        Val rv = na > 1 ? to_str(J, a[1]) : vcstr(J, "");
        int idx = str_find(s, sl, nv.u.o->s, nv.u.o->slen, 0);
        if (idx < 0) return sv;
        int nl = nv.u.o->slen, rl = rv.u.o->slen;
        int total = sl - nl + rl;
        char *b = (char *)aalloc(J, total + 1);
        memcpy(b, s, idx);
        memcpy(b + idx, rv.u.o->s, rl);
        memcpy(b + idx + rl, s + idx + nl, sl - idx - nl);
        b[total] = 0;
        Obj *o = newobj(J, O_STR); o->s = b; o->slen = total; return vobj(o);
    }
    if (strcmp(m, "toString") == 0) return sv;
    return vundef();
}

/* ---------- metode array ---------- */
static void arr_push(JS *J, Obj *o, Val v)
{
    if (o->nel >= o->elcap) {
        int nc = o->elcap ? o->elcap * 2 : 8;
        Val *ne = (Val *)aalloc(J, nc * sizeof(Val));
        for (int i = 0; i < o->nel; i++) ne[i] = o->el[i];
        o->el = ne; o->elcap = nc;
    }
    o->el[o->nel++] = v;
}
static Val nat_arr_dispatch(JS *J, Obj *self, Val thisv, Val *a, int na)
{
    const char *m = self->s;
    if (!is_arr(thisv)) return vundef();
    Obj *o = thisv.u.o;

    if (strcmp(m, "push") == 0) { for (int i = 0; i < na; i++) arr_push(J, o, a[i]); return vnum(o->nel); }
    if (strcmp(m, "pop") == 0) { if (o->nel == 0) return vundef(); return o->el[--o->nel]; }
    if (strcmp(m, "shift") == 0) { if (o->nel==0) return vundef(); Val r=o->el[0]; for(int i=1;i<o->nel;i++)o->el[i-1]=o->el[i]; o->nel--; return r; }
    if (strcmp(m, "join") == 0) {
        Val sep = na ? to_str(J, a[0]) : vcstr(J, ",");
        Val r = vcstr(J, "");
        for (int i = 0; i < o->nel; i++) { if (i) r = str_cat(J, r, sep); r = str_cat(J, r, o->el[i]); }
        return r;
    }
    if (strcmp(m, "indexOf") == 0) {
        for (int i = 0; i < o->nel; i++) if (val_eq(J, o->el[i], na?a[0]:vundef(), 1)) return vnum(i);
        return vnum(-1);
    }
    if (strcmp(m, "includes") == 0) {
        for (int i = 0; i < o->nel; i++) if (val_eq(J, o->el[i], na?a[0]:vundef(), 1)) return vbool(1);
        return vbool(0);
    }
    if (strcmp(m, "slice") == 0) {
        int b = na ? (int)to_num(a[0]) : 0, e = na>1 ? (int)to_num(a[1]) : o->nel;
        if (b<0) b+=o->nel; if (e<0) e+=o->nel; if (b<0)b=0; if(e>o->nel)e=o->nel;
        Obj *r = newobj(J, O_ARRAY);
        for (int i = b; i < e; i++) arr_push(J, r, o->el[i]);
        return vobj(r);
    }
    if (strcmp(m, "concat") == 0) {
        Obj *r = newobj(J, O_ARRAY);
        for (int i = 0; i < o->nel; i++) arr_push(J, r, o->el[i]);
        for (int i = 0; i < na; i++) {
            if (is_arr(a[i])) for (int k = 0; k < a[i].u.o->nel; k++) arr_push(J, r, a[i].u.o->el[k]);
            else arr_push(J, r, a[i]);
        }
        return vobj(r);
    }
    if (strcmp(m, "reverse") == 0) {
        for (int i = 0, j = o->nel-1; i < j; i++, j--) { Val t=o->el[i]; o->el[i]=o->el[j]; o->el[j]=t; }
        return thisv;
    }
    if (strcmp(m, "forEach") == 0) {
        if (na < 1) return vundef();
        for (int i = 0; i < o->nel && !J->haserr; i++) { Val ar[2]={o->el[i],vnum(i)}; call_fn(J, a[0], vundef(), ar, 2); }
        return vundef();
    }
    if (strcmp(m, "map") == 0) {
        Obj *r = newobj(J, O_ARRAY);
        if (na >= 1) for (int i = 0; i < o->nel && !J->haserr; i++) { Val ar[2]={o->el[i],vnum(i)}; arr_push(J, r, call_fn(J, a[0], vundef(), ar, 2)); }
        return vobj(r);
    }
    if (strcmp(m, "filter") == 0) {
        Obj *r = newobj(J, O_ARRAY);
        if (na >= 1) for (int i = 0; i < o->nel && !J->haserr; i++) { Val ar[2]={o->el[i],vnum(i)}; if (truthy(call_fn(J, a[0], vundef(), ar, 2))) arr_push(J, r, o->el[i]); }
        return vobj(r);
    }
    if (strcmp(m, "toString") == 0 || strcmp(m, "join2") == 0) return to_str(J, thisv);
    return vundef();
}

/* ---------- DOM ---------- */
static Val nat_dom_dispatch(JS *J, Obj *self, Val thisv, Val *a, int na)
{
    const char *m = self->s;
    int h = self->dom;
    (void)thisv;
    if (!J->dom) return vundef();
    if (strcmp(m, "appendChild") == 0) {
        if (na >= 1 && a[0].t == T_OBJ && a[0].u.o->kind == O_DOM)
            J->dom->append(J->dom->ud, h, a[0].u.o->dom);
        return na ? a[0] : vundef();
    }
    if (strcmp(m, "setAttribute") == 0) {
        if (na >= 2) { char k[48], v[256]; to_str_key(J,a[0],k,sizeof(k)); to_str_key(J,a[1],v,sizeof(v)); J->dom->set_attr(J->dom->ud, h, k, v); }
        return vundef();
    }
    if (strcmp(m, "getAttribute") == 0) return vcstr(J, "");
    if (strcmp(m, "addEventListener") == 0) return vundef();   /* fara evenimente */
    if (strcmp(m, "removeChild") == 0 || strcmp(m, "remove") == 0) return vundef();
    return vundef();
}

/* rebuild style string dintr-un obiect style si aplica pe nod */
static void style_flush(JS *J, Obj *styleobj)
{
    Val *dh = find_prop(styleobj, "__dom");
    if (!dh || !J->dom) return;
    int h = (int)dh->u.n;
    char buf[256]; int p = 0;
    for (int i = 0; i < styleobj->np; i++) {
        const char *k = styleobj->props[i].key;
        if (k[0] == '_' && k[1] == '_') continue;
        Val vs = to_str(J, styleobj->props[i].val);
        /* camelCase -> kebab (doar cateva uzuale) */
        for (int c = 0; k[c] && p < 200; c++) {
            if (k[c] >= 'A' && k[c] <= 'Z') { buf[p++]='-'; buf[p++]=(char)(k[c]+32); }
            else buf[p++] = k[c];
        }
        buf[p++] = ':';
        for (int c = 0; c < vs.u.o->slen && p < 250; c++) buf[p++] = vs.u.o->s[c];
        buf[p++] = ';';
    }
    buf[p] = 0;
    J->dom->set_attr(J->dom->ud, h, "style", buf);
}

static Val dom_prop_get(JS *J, Obj *o, const char *key)
{
    int h = o->dom;
    if (!J->dom) return vundef();
    if (strcmp(key, "innerHTML") == 0 || strcmp(key, "textContent") == 0 ||
        strcmp(key, "innerText") == 0) {
        static char buf[8192];
        int n = J->dom->get_inner(J->dom->ud, h, buf, sizeof(buf));
        if (n < 0) n = 0;
        return vstr(J, buf, n);
    }
    if (strcmp(key, "style") == 0) {
        Val *c = find_prop(o, "__stylecache");
        if (c) return *c;
        Obj *st = newobj(J, O_OBJECT);
        set_own(J, st, "__dom", vnum(h));
        set_own(J, st, "__isstyle", vbool(1));
        set_own(J, o, "__stylecache", vobj(st));
        return vobj(st);
    }
    if (strcmp(key, "appendChild") == 0 || strcmp(key, "setAttribute") == 0 ||
        strcmp(key, "getAttribute") == 0 || strcmp(key, "addEventListener") == 0 ||
        strcmp(key, "removeChild") == 0 || strcmp(key, "remove") == 0) {
        Obj *nf = newobj(J, O_NATIVE);
        nf->nat = nat_dom_dispatch; nf->s = adup(J, key, (int)strlen(key)); nf->dom = h;
        return vobj(nf);
    }
    /* proprietati stocate (ex. cele scrise de script) */
    Val *p = find_prop(o, key);
    if (p) return *p;
    if (strcmp(key, "children") == 0 || strcmp(key, "childNodes") == 0) {
        return vobj(newobj(J, O_ARRAY));
    }
    return vcstr(J, "");
}

static void dom_prop_set(JS *J, Obj *o, const char *key, Val val)
{
    int h = o->dom;
    if (!J->dom) return;
    Val sv = to_str(J, val);
    if (strcmp(key, "innerHTML") == 0) { J->dom->set_inner(J->dom->ud, h, sv.u.o->s, sv.u.o->slen); return; }
    if (strcmp(key, "textContent") == 0 || strcmp(key, "innerText") == 0) { J->dom->set_text(J->dom->ud, h, sv.u.o->s, sv.u.o->slen); return; }
    if (strcmp(key, "className") == 0) { J->dom->set_attr(J->dom->ud, h, "class", sv.u.o->s); return; }
    if (strcmp(key, "id") == 0) { J->dom->set_attr(J->dom->ud, h, "id", sv.u.o->s); return; }
    if (strcmp(key, "href") == 0 || strcmp(key, "src") == 0 || strcmp(key, "title") == 0 || strcmp(key, "value") == 0) { J->dom->set_attr(J->dom->ud, h, key, sv.u.o->s); return; }
    if (strcmp(key, "style") == 0) { J->dom->set_attr(J->dom->ud, h, "style", sv.u.o->s); return; }
    set_own(J, o, key, val);
}

/* ---------- functii globale ---------- */
static Val nat_log(JS *J, Obj *s, Val t, Val *a, int na)
{
    (void)s;(void)t;
    if (!J->dom || !J->dom->log) return vundef();
    char buf[512]; int p = 0;
    for (int i = 0; i < na && p < 500; i++) {
        if (i) buf[p++] = ' ';
        Val sv = to_str(J, a[i]);
        for (int k = 0; k < sv.u.o->slen && p < 500; k++) buf[p++] = sv.u.o->s[k];
    }
    buf[p] = 0;
    J->dom->log(J->dom->ud, buf, p);
    return vundef();
}
static Val nat_parseInt(JS *J, Obj *s, Val t, Val *a, int na)
{
    (void)J;(void)s;(void)t; if (na < 1) return vnum(0.0/0.0);
    Val sv = to_str(J, a[0]); const char *p = sv.u.o->s;
    while (*p==' ') p++;
    int neg = 0; if (*p=='-'){neg=1;p++;} else if(*p=='+')p++;
    int base = na>1 ? (int)to_num(a[1]) : 10; if (base==0) base=10;
    if (base==16 && p[0]=='0' && (p[1]=='x'||p[1]=='X')) p+=2;
    long v = 0; int any = 0;
    for (;;) { int d; char c=*p; if(c>='0'&&c<='9')d=c-'0'; else if((c|32)>='a'&&(c|32)<='z')d=(c|32)-'a'+10; else break; if(d>=base)break; v=v*base+d; p++; any=1; }
    if (!any) return vnum(0.0/0.0);
    return vnum(neg?-v:v);
}
static Val nat_parseFloat(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)s;(void)t; if(na<1)return vnum(0.0/0.0); return vnum(to_num(to_str(J,a[0]))); }
static Val nat_isNaN(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)J;(void)s;(void)t; double d = na?to_num(a[0]):0.0/0.0; return vbool(d!=d); }
static Val nat_String(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)s;(void)t; return na ? to_str(J, a[0]) : vcstr(J, ""); }
static Val nat_Number(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)J;(void)s;(void)t; return vnum(na ? to_num(a[0]) : 0); }
static Val nat_Boolean(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)J;(void)s;(void)t; return vbool(na ? truthy(a[0]) : 0); }
static Val nat_Array(JS *J, Obj *s, Val t, Val *a, int na)
{
    (void)s;(void)t;
    Obj *o = newobj(J, O_ARRAY);
    if (na == 1 && a[0].t == T_NUM) { int L=(int)a[0].u.n; if(L<0)L=0; if(L>10000)L=10000; for(int i=0;i<L;i++)arr_push(J,o,vundef()); }
    else for (int i = 0; i < na; i++) arr_push(J, o, a[i]);
    return vobj(o);
}

/* Math */
static uint64_t js_rng = 0x2545F4914F6CDD1DULL;
static Val nat_m_random(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)J;(void)s;(void)t;(void)a;(void)na; js_rng^=js_rng<<13; js_rng^=js_rng>>7; js_rng^=js_rng<<17; return vnum((double)(js_rng>>11)/(double)(1ULL<<53)); }
static double d_abs(double x){return x<0?-x:x;}
static double d_floor(double x){ long long i=(long long)x; if((double)i>x)i--; return (double)i; }
static double d_ceil(double x){ long long i=(long long)x; if((double)i<x)i++; return (double)i; }
static double d_sqrt(double x){ if(x<=0)return 0; double g=x; for(int i=0;i<40;i++)g=(g+x/g)/2; return g; }
static double d_pow(double b,double e){ int n=(int)e; double r=1; if(n<0){for(int i=0;i<-n;i++)r*=b; return 1/r;} for(int i=0;i<n;i++)r*=b; return r; }
static Val nat_m_floor(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t;return vnum(d_floor(na?to_num(a[0]):0));}
static Val nat_m_ceil(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t;return vnum(d_ceil(na?to_num(a[0]):0));}
static Val nat_m_round(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t;return vnum(d_floor((na?to_num(a[0]):0)+0.5));}
static Val nat_m_abs(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t;return vnum(d_abs(na?to_num(a[0]):0));}
static Val nat_m_sqrt(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t;return vnum(d_sqrt(na?to_num(a[0]):0));}
static Val nat_m_pow(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t;return vnum(d_pow(na?to_num(a[0]):0,na>1?to_num(a[1]):0));}
static Val nat_m_min(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t; double r=1e308; for(int i=0;i<na;i++){double v=to_num(a[i]);if(v<r)r=v;} return vnum(r);}
static Val nat_m_max(JS*J,Obj*s,Val t,Val*a,int na){(void)J;(void)s;(void)t; double r=-1e308; for(int i=0;i<na;i++){double v=to_num(a[i]);if(v>r)r=v;} return vnum(r);}

/* document */
static Val nat_doc_getid(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)s;(void)t; if(!J->dom||na<1)return vnull(); char id[64]; to_str_key(J,a[0],id,sizeof(id)); int h=J->dom->get_by_id(J->dom->ud,id); if(h<0)return vnull(); Obj*o=newobj(J,O_DOM);o->dom=h;return vobj(o); }
static Val nat_doc_write(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)s;(void)t; if(J->dom) for(int i=0;i<na;i++){Val sv=to_str(J,a[i]); J->dom->write(J->dom->ud,sv.u.o->s,sv.u.o->slen);} return vundef(); }
static Val nat_doc_create(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)s;(void)t; if(!J->dom||na<1)return vnull(); char tag[24]; to_str_key(J,a[0],tag,sizeof(tag)); int h=J->dom->create(J->dom->ud,tag); if(h<0)return vnull(); Obj*o=newobj(J,O_DOM);o->dom=h;return vobj(o); }
static Val nat_doc_getbytag(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)J;(void)s;(void)t;(void)a;(void)na; return vobj(newobj(J,O_ARRAY)); }
static Val nat_noop(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)J;(void)s;(void)t;(void)a;(void)na; return vundef(); }
static Val nat_setTimeout(JS *J, Obj *s, Val t, Val *a, int na)
{ (void)s;(void)t; if(na>=1 && is_fn(a[0])) call_fn(J,a[0],vundef(),0,0); return vnum(0); }

static void defm(JS *J, Obj *o, const char *name, NatFn fn)
{ set_own(J, o, name, make_native(J, fn)); }

static void install_globals(JS *J)
{
    Env *g = J->global;

    /* console */
    Obj *con = newobj(J, O_OBJECT);
    defm(J, con, "log", nat_log);
    defm(J, con, "error", nat_log);
    defm(J, con, "warn", nat_log);
    defm(J, con, "info", nat_log);
    env_def(J, g, "console", vobj(con));

    /* Math */
    Obj *ma = newobj(J, O_OBJECT);
    defm(J, ma, "floor", nat_m_floor); defm(J, ma, "ceil", nat_m_ceil);
    defm(J, ma, "round", nat_m_round); defm(J, ma, "abs", nat_m_abs);
    defm(J, ma, "sqrt", nat_m_sqrt); defm(J, ma, "pow", nat_m_pow);
    defm(J, ma, "min", nat_m_min); defm(J, ma, "max", nat_m_max);
    defm(J, ma, "random", nat_m_random);
    set_own(J, ma, "PI", vnum(3.141592653589793));
    set_own(J, ma, "E", vnum(2.718281828459045));
    env_def(J, g, "Math", vobj(ma));

    /* functii globale */
    env_def(J, g, "parseInt", make_native(J, nat_parseInt));
    env_def(J, g, "parseFloat", make_native(J, nat_parseFloat));
    env_def(J, g, "isNaN", make_native(J, nat_isNaN));
    env_def(J, g, "String", make_native(J, nat_String));
    env_def(J, g, "Number", make_native(J, nat_Number));
    env_def(J, g, "Boolean", make_native(J, nat_Boolean));
    env_def(J, g, "Array", make_native(J, nat_Array));
    env_def(J, g, "alert", make_native(J, nat_noop));
    env_def(J, g, "setTimeout", make_native(J, nat_setTimeout));
    env_def(J, g, "setInterval", make_native(J, nat_noop));
    env_def(J, g, "NaN", vnum(0.0/0.0));
    env_def(J, g, "undefined", vundef());

    /* document */
    Obj *doc = newobj(J, O_OBJECT);
    defm(J, doc, "getElementById", nat_doc_getid);
    defm(J, doc, "write", nat_doc_write);
    defm(J, doc, "writeln", nat_doc_write);
    defm(J, doc, "createElement", nat_doc_create);
    defm(J, doc, "getElementsByTagName", nat_doc_getbytag);
    defm(J, doc, "getElementsByClassName", nat_doc_getbytag);
    defm(J, doc, "querySelector", nat_doc_getid);
    if (J->dom) { Obj *body = newobj(J, O_DOM); body->dom = J->dom->body; set_own(J, doc, "body", vobj(body)); }
    env_def(J, g, "document", vobj(doc));

    /* window = obiect global cu document */
    Obj *win = newobj(J, O_OBJECT);
    set_own(J, win, "document", vobj(doc));
    defm(J, win, "addEventListener", nat_noop);
    env_def(J, g, "window", vobj(win));
}
