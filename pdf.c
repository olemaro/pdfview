#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "pdf.h"
#include "inflate.h"

/* ================================================================
   Memory helpers
   ================================================================ */

static void *xmalloc(int n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "pdf: out of memory\n"); exit(1); }
    return p;
}
static void *xrealloc(void *p, int n) {
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "pdf: out of memory\n"); exit(1); }
    return p;
}
static char *xstrdup(const char *s) {
    char *r = (char *)xmalloc((int)strlen(s) + 1);
    strcpy(r, s);
    return r;
}

/* ================================================================
   Object allocation / free
   ================================================================ */

static PdfObj *obj_new(PdfType t) {
    PdfObj *o = (PdfObj *)xmalloc(sizeof(PdfObj));
    memset(o, 0, sizeof(*o));
    o->type = t;
    return o;
}

static void obj_free(PdfObj *o);

static void dict_clear(PdfDict *d) {
    int i;
    for (i = 0; i < d->count; i++) {
        free(d->keys[i]);
        obj_free(d->vals[i]);
    }
    free(d->keys);
    free(d->vals);
}

static void obj_free(PdfObj *o) {
    int i;
    if (!o) return;
    switch (o->type) {
    case PDF_STRING:
    case PDF_NAME:
        free(o->str.data);
        break;
    case PDF_ARRAY:
        for (i = 0; i < o->arr.count; i++) obj_free(o->arr.items[i]);
        free(o->arr.items);
        break;
    case PDF_DICT:
        dict_clear(&o->dict);
        break;
    case PDF_STREAM:
        dict_clear(&o->stream.dict);
        free(o->stream.raw);
        break;
    default:
        break;
    }
    free(o);
}

/* ================================================================
   Scanner / tokenizer
   ================================================================ */

typedef struct {
    const unsigned char *buf;
    int                  len;
    int                  pos;
} Scanner;

static void sc_init(Scanner *s, const unsigned char *buf, int len, int pos) {
    s->buf = buf; s->len = len; s->pos = pos;
}

static int sc_eof(Scanner *s)  { return s->pos >= s->len; }
static int sc_peek(Scanner *s) { return sc_eof(s) ? -1 : s->buf[s->pos]; }
static int sc_get(Scanner *s)  { return sc_eof(s) ? -1 : s->buf[s->pos++]; }

static void sc_skip_ws(Scanner *s) {
    while (!sc_eof(s)) {
        int c = sc_peek(s);
        if (c == '%') { /* comment */
            while (!sc_eof(s) && sc_peek(s) != '\n' && sc_peek(s) != '\r')
                sc_get(s);
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') {
            sc_get(s);
        } else break;
    }
}

/* Read keyword or number token into buf (max len-1 chars) */
static int sc_token(Scanner *s, char *buf, int blen) {
    int i = 0;
    sc_skip_ws(s);
    while (!sc_eof(s)) {
        int c = sc_peek(s);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '/' || c == '(' || c == ')' || c == '<' || c == '>' ||
            c == '[' || c == ']' || c == '{' || c == '}' || c == '%')
            break;
        if (i < blen - 1) buf[i++] = (char)sc_get(s);
        else sc_get(s);
    }
    buf[i] = '\0';
    return i;
}

static int sc_match(Scanner *s, const char *kw) {
    int len = (int)strlen(kw);
    if (s->pos + len > s->len) return 0;
    if (memcmp(s->buf + s->pos, kw, len) != 0) return 0;
    s->pos += len;
    return 1;
}

/* ================================================================
   Object parser  (forward decls)
   ================================================================ */
static PdfObj *parse_obj(Scanner *s);

static PdfObj *parse_string(Scanner *s) {
    /* literal string: (...)  with nesting and backslash escapes */
    unsigned char tmp[65536];
    int len = 0, depth = 1;
    sc_get(s); /* consume '(' */
    while (!sc_eof(s) && depth > 0) {
        int c = sc_get(s);
        if (c == '\\') {
            int e = sc_get(s);
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\\': c = '\\'; break;
            case '(': c = '('; break;
            case ')': c = ')'; break;
            default:
                if (e >= '0' && e <= '7') {
                    int oc = e - '0';
                    if (sc_peek(s) >= '0' && sc_peek(s) <= '7') oc = oc*8 + sc_get(s) - '0';
                    if (sc_peek(s) >= '0' && sc_peek(s) <= '7') oc = oc*8 + sc_get(s) - '0';
                    c = oc;
                } else { c = e; }
            }
            if (len < (int)sizeof(tmp)) tmp[len++] = (unsigned char)c;
        } else if (c == '(') {
            depth++;
            if (len < (int)sizeof(tmp)) tmp[len++] = '(';
        } else if (c == ')') {
            if (--depth > 0 && len < (int)sizeof(tmp)) tmp[len++] = ')';
        } else {
            if (len < (int)sizeof(tmp)) tmp[len++] = (unsigned char)c;
        }
    }
    PdfObj *o = obj_new(PDF_STRING);
    o->str.data = (unsigned char *)xmalloc(len + 1);
    memcpy(o->str.data, tmp, len);
    o->str.data[len] = 0;
    o->str.len = len;
    return o;
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static PdfObj *parse_hex_string(Scanner *s) {
    unsigned char tmp[65536];
    int len = 0;
    sc_get(s); /* consume '<' */
    while (!sc_eof(s) && sc_peek(s) != '>') {
        int h, l;
        while (!sc_eof(s) && isspace(sc_peek(s))) sc_get(s);
        if (sc_peek(s) == '>') break;
        h = hex_val(sc_get(s));
        while (!sc_eof(s) && isspace(sc_peek(s))) sc_get(s);
        if (sc_peek(s) == '>') { if (len < (int)sizeof(tmp)) tmp[len++] = (unsigned char)(h << 4); break; }
        l = hex_val(sc_get(s));
        if (len < (int)sizeof(tmp)) tmp[len++] = (unsigned char)((h << 4) | l);
    }
    if (sc_peek(s) == '>') sc_get(s);
    PdfObj *o = obj_new(PDF_STRING);
    o->str.data = (unsigned char *)xmalloc(len + 1);
    memcpy(o->str.data, tmp, len);
    o->str.data[len] = 0;
    o->str.len = len;
    return o;
}

static PdfObj *parse_name(Scanner *s) {
    char buf[512];
    int i = 0;
    sc_get(s); /* consume '/' */
    while (!sc_eof(s)) {
        int c = sc_peek(s);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '/' || c == '(' || c == ')' || c == '<' || c == '>' ||
            c == '[' || c == ']' || c == '{' || c == '}' || c == '%')
            break;
        if (c == '#') {
            sc_get(s);
            int h = hex_val(sc_get(s)), l = hex_val(sc_get(s));
            if (i < (int)sizeof(buf)-1) buf[i++] = (char)((h << 4) | l);
        } else {
            if (i < (int)sizeof(buf)-1) buf[i++] = (char)sc_get(s);
            else sc_get(s);
        }
    }
    buf[i] = '\0';
    PdfObj *o = obj_new(PDF_NAME);
    o->str.data = (unsigned char *)xstrdup(buf);
    o->str.len  = i;
    return o;
}

static PdfObj *parse_array(Scanner *s) {
    PdfObj *o = obj_new(PDF_ARRAY);
    sc_get(s); /* consume '[' */
    for (;;) {
        sc_skip_ws(s);
        if (sc_eof(s) || sc_peek(s) == ']') { sc_get(s); break; }
        PdfObj *item = parse_obj(s);
        if (!item) break;
        if (o->arr.count >= o->arr.cap) {
            o->arr.cap = o->arr.cap ? o->arr.cap * 2 : 8;
            o->arr.items = (PdfObj **)xrealloc(o->arr.items, o->arr.cap * sizeof(PdfObj *));
        }
        o->arr.items[o->arr.count++] = item;
    }
    return o;
}

static void dict_put(PdfDict *d, const char *key, PdfObj *val) {
    int i;
    for (i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key) == 0) {
            obj_free(d->vals[i]);
            d->vals[i] = val;
            return;
        }
    }
    if (d->count >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->keys = (char **)xrealloc(d->keys, d->cap * sizeof(char *));
        d->vals = (PdfObj **)xrealloc(d->vals, d->cap * sizeof(PdfObj *));
    }
    d->keys[d->count] = xstrdup(key);
    d->vals[d->count] = val;
    d->count++;
}

static PdfObj *parse_dict_or_stream(Scanner *s, const unsigned char *file_buf, int file_len) {
    PdfObj *o = obj_new(PDF_DICT);
    sc_get(s); sc_get(s); /* consume '<<' */
    for (;;) {
        sc_skip_ws(s);
        if (sc_eof(s) || (sc_peek(s) == '>' && s->pos+1 < s->len && s->buf[s->pos+1] == '>')) {
            sc_get(s); sc_get(s); /* consume '>>' */
            break;
        }
        if (sc_peek(s) != '/') { sc_get(s); continue; }
        PdfObj *kobj = parse_name(s);
        if (!kobj) break;
        sc_skip_ws(s);
        PdfObj *vobj = parse_obj(s);
        if (!vobj) { obj_free(kobj); break; }
        dict_put(&o->dict, (char *)kobj->str.data, vobj);
        obj_free(kobj);
    }

    /* Check for stream keyword */
    sc_skip_ws(s);
    if (s->pos + 6 <= s->len && memcmp(s->buf + s->pos, "stream", 6) == 0) {
        s->pos += 6;
        /* skip single \r\n or \n after 'stream' */
        if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
        if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;

        /* get /Length from dict */
        int stream_start = s->pos;
        int length = -1;
        int i;
        for (i = 0; i < o->dict.count; i++) {
            if (strcmp(o->dict.keys[i], "Length") == 0) {
                PdfObj *lobj = o->dict.vals[i];
                if (lobj->type == PDF_INT) length = (int)lobj->ival;
                else if (lobj->type == PDF_REAL) length = (int)lobj->rval;
                break;
            }
        }

        if (length < 0) {
            /* Try to find 'endstream' */
            const unsigned char *p = s->buf + stream_start;
            int remaining = s->len - stream_start;
            const char *es = "endstream";
            int j;
            length = 0;
            for (j = 0; j <= remaining - 9; j++) {
                if (memcmp(p + j, es, 9) == 0) { length = j; break; }
            }
        }
        if (length < 0 || stream_start + length > s->len)
            length = s->len - stream_start;

        /* Convert dict to stream */
        o->type = PDF_STREAM;
        o->stream.raw     = (unsigned char *)xmalloc(length);
        o->stream.raw_len = length;
        memcpy(o->stream.raw, s->buf + stream_start, length);
        s->pos = stream_start + length;
        /* skip 'endstream' */
        sc_skip_ws(s);
        if (s->pos + 9 <= s->len && memcmp(s->buf + s->pos, "endstream", 9) == 0)
            s->pos += 9;
    }
    (void)file_buf; (void)file_len;
    return o;
}

static PdfObj *parse_obj(Scanner *s) {
    sc_skip_ws(s);
    if (sc_eof(s)) return NULL;

    int c = sc_peek(s);

    if (c == '(') return parse_string(s);
    if (c == '/') return parse_name(s);
    if (c == '[') return parse_array(s);
    if (c == '<') {
        if (s->pos + 1 < s->len && s->buf[s->pos+1] == '<')
            return parse_dict_or_stream(s, s->buf, s->len);
        return parse_hex_string(s);
    }
    if (c == 't' && sc_match(s, "true"))  {
        PdfObj *o = obj_new(PDF_BOOL); o->bval = 1; return o;
    }
    if (c == 'f' && sc_match(s, "false")) {
        PdfObj *o = obj_new(PDF_BOOL); o->bval = 0; return o;
    }
    if (c == 'n' && sc_match(s, "null"))  return obj_new(PDF_NULL);
    if (c == 'R') { /* isolated R — shouldn't happen here */ return NULL; }

    /* number or indirect ref */
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        int save = s->pos;
        char tok[64];
        sc_token(s, tok, sizeof(tok));
        int is_int = 1;
        int j;
        for (j = 0; tok[j]; j++) if (tok[j] == '.' || tok[j] == 'e' || tok[j] == 'E') { is_int = 0; break; }

        if (is_int) {
            /* peek ahead: could be "N G R" (indirect ref) or "N G obj" */
            int cur = s->pos;
            char tok2[32], tok3[16];
            sc_skip_ws(s);
            sc_token(s, tok2, sizeof(tok2));
            sc_skip_ws(s);
            sc_token(s, tok3, sizeof(tok3));
            if (strcmp(tok3, "R") == 0) {
                PdfObj *o = obj_new(PDF_REF);
                o->ref.num = atoi(tok);
                o->ref.gen = atoi(tok2);
                return o;
            }
            s->pos = cur; /* restore */
            PdfObj *o = obj_new(PDF_INT);
            o->ival = atoll(tok);
            return o;
        }

        PdfObj *o = obj_new(PDF_REAL);
        o->rval = atof(tok);
        return o;
        (void)save;
    }

    /* skip unknown token */
    char tmp[64];
    sc_token(s, tmp, sizeof(tmp));
    return NULL;
}

/* ================================================================
   XRef / document loading
   ================================================================ */

static void doc_ensure_xref(PdfDoc *doc, int num) {
    if (num >= doc->xref_cap) {
        int nc = num + 256;
        doc->xref  = (XRefEntry *)xrealloc(doc->xref, nc * sizeof(XRefEntry));
        doc->cache = (PdfObj **)xrealloc(doc->cache, nc * sizeof(PdfObj *));
        int i;
        for (i = doc->xref_cap; i < nc; i++) {
            memset(&doc->xref[i], 0, sizeof(XRefEntry));
            doc->cache[i] = NULL;
        }
        doc->xref_cap = nc;
    }
    if (num + 1 > doc->xref_size) doc->xref_size = num + 1;
}

/* Parse traditional xref table starting at 'pos' in doc->buf */
static void parse_xref_table(PdfDoc *doc, int pos, PdfObj **trailer_out) {
    Scanner s;
    sc_init(&s, doc->buf, doc->buf_len, pos);
    char tok[64];
    sc_token(&s, tok, sizeof(tok)); /* "xref" */
    if (strcmp(tok, "xref") != 0) return;

    for (;;) {
        sc_skip_ws(&s);
        char t1[32], t2[32];
        int spos = s.pos;
        sc_token(&s, t1, sizeof(t1));
        if (strcmp(t1, "trailer") == 0) break;
        sc_skip_ws(&s);
        sc_token(&s, t2, sizeof(t2));
        int first = atoi(t1), count = atoi(t2);
        int i;
        for (i = 0; i < count; i++) {
            char off[21], gen[8], flag[4];
            sc_skip_ws(&s);
            sc_token(&s, off,  sizeof(off));
            sc_skip_ws(&s);
            sc_token(&s, gen,  sizeof(gen));
            sc_skip_ws(&s);
            sc_token(&s, flag, sizeof(flag));
            int num = first + i;
            doc_ensure_xref(doc, num);
            doc->xref[num].offset = atoll(off);
            doc->xref[num].gen    = atoi(gen);
            doc->xref[num].type   = (flag[0] == 'n') ? 1 : 0;
        }
        (void)spos;
    }

    /* parse trailer dict */
    sc_skip_ws(&s);
    PdfObj *td = parse_obj(&s);
    if (trailer_out) *trailer_out = td;
}

/* Parse object at byte offset in doc->buf */
static PdfObj *parse_obj_at(PdfDoc *doc, long long offset) {
    if (offset < 0 || offset >= doc->buf_len) return NULL;
    Scanner s;
    sc_init(&s, doc->buf, doc->buf_len, (int)offset);
    char t1[32], t2[32], t3[16];
    sc_skip_ws(&s);
    sc_token(&s, t1, sizeof(t1));
    sc_skip_ws(&s);
    sc_token(&s, t2, sizeof(t2));
    sc_skip_ws(&s);
    sc_token(&s, t3, sizeof(t3));
    if (strcmp(t3, "obj") != 0) return NULL;
    sc_skip_ws(&s);
    return parse_obj(&s);
}

/* ---- Cross-reference stream (PDF 1.5+) ---- */
static void parse_xref_stream(PdfDoc *doc, int pos) {
    PdfObj *o = parse_obj_at(doc, pos);
    if (!o || o->type != PDF_STREAM) { obj_free(o); return; }

    /* Read /W and /Index arrays from stream dict */
    PdfDict *d = &o->stream.dict;
    int i, w[3] = {1, 2, 1}; /* default widths */
    /* /W */
    for (i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], "W") == 0 && d->vals[i]->type == PDF_ARRAY) {
            PdfArray *wa = &d->vals[i]->arr;
            int j;
            for (j = 0; j < 3 && j < wa->count; j++)
                w[j] = (wa->items[j]->type == PDF_INT) ? (int)wa->items[j]->ival : 0;
        }
    }
    /* /Size */
    int size = 0;
    for (i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], "Size") == 0 && d->vals[i]->type == PDF_INT)
            size = (int)d->vals[i]->ival;
    }

    /* decompress stream */
    int dlen = 0;
    unsigned char *data = NULL;
    /* check /Filter */
    for (i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], "Filter") == 0) {
            PdfObj *f = d->vals[i];
            const char *fn = NULL;
            if (f->type == PDF_NAME) fn = (char *)f->str.data;
            else if (f->type == PDF_ARRAY && f->arr.count > 0 && f->arr.items[0]->type == PDF_NAME)
                fn = (char *)f->arr.items[0]->str.data;
            if (fn && (strcmp(fn, "FlateDecode") == 0 || strcmp(fn, "Fl") == 0))
                data = zinflate(o->stream.raw, o->stream.raw_len, &dlen);
        }
    }
    if (!data) {
        data = (unsigned char *)xmalloc(o->stream.raw_len);
        memcpy(data, o->stream.raw, o->stream.raw_len);
        dlen = o->stream.raw_len;
    }

    /* /Index */
    int *idx_first = NULL, *idx_count = NULL, idx_pairs = 0;
    for (i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], "Index") == 0 && d->vals[i]->type == PDF_ARRAY) {
            PdfArray *ia = &d->vals[i]->arr;
            idx_pairs = ia->count / 2;
            idx_first = (int *)xmalloc(idx_pairs * sizeof(int));
            idx_count = (int *)xmalloc(idx_pairs * sizeof(int));
            int j;
            for (j = 0; j < idx_pairs; j++) {
                idx_first[j] = (int)(ia->items[j*2]->ival);
                idx_count[j] = (int)(ia->items[j*2+1]->ival);
            }
        }
    }
    if (!idx_first) {
        idx_pairs = 1;
        idx_first = (int *)xmalloc(sizeof(int));
        idx_count = (int *)xmalloc(sizeof(int));
        idx_first[0] = 0; idx_count[0] = size;
    }

    int stride = w[0] + w[1] + w[2];
    int dpos = 0, pair;
    for (pair = 0; pair < idx_pairs; pair++) {
        int first = idx_first[pair], count2 = idx_count[pair];
        int k;
        for (k = 0; k < count2; k++) {
            if (dpos + stride > dlen) break;
            long long f0 = 0, f1 = 0, f2 = 0;
            int j;
            for (j = 0; j < w[0]; j++) f0 = (f0 << 8) | data[dpos++];
            for (j = 0; j < w[1]; j++) f1 = (f1 << 8) | data[dpos++];
            for (j = 0; j < w[2]; j++) f2 = (f2 << 8) | data[dpos++];
            if (w[0] == 0) f0 = 1; /* default type */
            int num = first + k;
            doc_ensure_xref(doc, num);
            if (f0 == 0) { doc->xref[num].type = 0; }
            else if (f0 == 1) { doc->xref[num].type = 1; doc->xref[num].offset = f1; doc->xref[num].gen = (int)f2; }
            else if (f0 == 2) { doc->xref[num].type = 2; doc->xref[num].offset = f1; doc->xref[num].idx = (int)f2; }
        }
    }

    /* save trailer info */
    PdfObj *td = obj_new(PDF_DICT);
    td->dict = o->stream.dict;
    /* prevent double-free: clear stream dict */
    memset(&o->stream.dict, 0, sizeof(PdfDict));
    if (!doc->trailer) doc->trailer = td;
    else obj_free(td);

    free(data);
    free(idx_first);
    free(idx_count);
    obj_free(o);
}

/* Find 'startxref' near end of file */
static long long find_startxref(PdfDoc *doc) {
    int search = doc->buf_len > 1024 ? doc->buf_len - 1024 : 0;
    const unsigned char *p = doc->buf + search;
    int n = doc->buf_len - search;
    /* scan backwards */
    int i;
    for (i = n - 9; i >= 0; i--) {
        if (memcmp(p + i, "startxref", 9) == 0) {
            Scanner s;
            sc_init(&s, doc->buf, doc->buf_len, search + i + 9);
            sc_skip_ws(&s);
            char tok[32];
            sc_token(&s, tok, sizeof(tok));
            return atoll(tok);
        }
    }
    return -1;
}

/* ================================================================
   Public API
   ================================================================ */

PdfObj *pdf_resolve(PdfDoc *doc, PdfObj *obj) {
    int depth = 0;
    while (obj && obj->type == PDF_REF && depth++ < 64) {
        int num = obj->ref.num;
        if (num < 0 || num >= doc->xref_size) return NULL;
        if (doc->cache[num]) { obj = doc->cache[num]; continue; }
        XRefEntry *e = &doc->xref[num];
        if (e->type == 0) return NULL;
        PdfObj *parsed = NULL;
        if (e->type == 1) {
            parsed = parse_obj_at(doc, e->offset);
        } else if (e->type == 2) {
            /* Compressed object: fetch container stream */
            int stm_num = (int)e->offset;
            if (stm_num < 0 || stm_num >= doc->xref_size) return NULL;
            if (!doc->cache[stm_num]) {
                XRefEntry *se = &doc->xref[stm_num];
                doc->cache[stm_num] = (se->type == 1) ? parse_obj_at(doc, se->offset) : NULL;
            }
            PdfObj *stm = doc->cache[stm_num];
            if (!stm || stm->type != PDF_STREAM) return NULL;
            int dlen = 0;
            unsigned char *data = NULL;
            /* check filter */
            int fi;
            for (fi = 0; fi < stm->stream.dict.count; fi++) {
                if (strcmp(stm->stream.dict.keys[fi], "Filter") == 0) {
                    PdfObj *f = stm->stream.dict.vals[fi];
                    const char *fn = NULL;
                    if (f->type == PDF_NAME) fn = (char *)f->str.data;
                    if (fn && (strcmp(fn, "FlateDecode") == 0 || strcmp(fn, "Fl") == 0))
                        data = zinflate(stm->stream.raw, stm->stream.raw_len, &dlen);
                }
            }
            if (!data) {
                data = (unsigned char *)xmalloc(stm->stream.raw_len);
                memcpy(data, stm->stream.raw, stm->stream.raw_len);
                dlen = stm->stream.raw_len;
            }
            /* /First: offset of first object */
            int first_off = 0;
            for (fi = 0; fi < stm->stream.dict.count; fi++) {
                if (strcmp(stm->stream.dict.keys[fi], "First") == 0 && stm->stream.dict.vals[fi]->type == PDF_INT)
                    first_off = (int)stm->stream.dict.vals[fi]->ival;
            }
            /* read offset table */
            Scanner ts;
            sc_init(&ts, data, dlen, 0);
            int idx = e->idx, j;
            int obj_off = 0;
            for (j = 0; j <= idx; j++) {
                char n1[32], n2[32];
                sc_skip_ws(&ts);
                sc_token(&ts, n1, sizeof(n1));
                sc_skip_ws(&ts);
                sc_token(&ts, n2, sizeof(n2));
                if (j == idx) obj_off = atoi(n2);
            }
            Scanner os;
            sc_init(&os, data, dlen, first_off + obj_off);
            parsed = parse_obj(&os);
            free(data);
        }
        doc->cache[num] = parsed;
        obj = parsed;
    }
    return obj;
}

PdfObj *pdf_dict_get(PdfDoc *doc, PdfObj *dict, const char *key) {
    PdfDict *d = NULL;
    if (!dict) return NULL;
    if (dict->type == PDF_DICT)   d = &dict->dict;
    if (dict->type == PDF_STREAM) d = &dict->stream.dict;
    if (!d) return NULL;
    int i;
    for (i = 0; i < d->count; i++) {
        if (strcmp(d->keys[i], key) == 0)
            return pdf_resolve(doc, d->vals[i]);
    }
    return NULL;
}

double pdf_num(PdfObj *obj) {
    if (!obj) return 0.0;
    if (obj->type == PDF_INT)  return (double)obj->ival;
    if (obj->type == PDF_REAL) return obj->rval;
    return 0.0;
}

unsigned char *pdf_stream_data(PdfDoc *doc, PdfObj *obj, int *out_len) {
    if (!obj || obj->type != PDF_STREAM) return NULL;
    PdfObj *filter = pdf_dict_get(doc, obj, "Filter");
    const char *fname = NULL;
    if (filter) {
        if (filter->type == PDF_NAME) fname = (char *)filter->str.data;
        else if (filter->type == PDF_ARRAY && filter->arr.count > 0 &&
                 filter->arr.items[0]->type == PDF_NAME)
            fname = (char *)filter->arr.items[0]->str.data;
    }
    if (fname && (strcmp(fname, "FlateDecode") == 0 || strcmp(fname, "Fl") == 0)) {
        return zinflate(obj->stream.raw, obj->stream.raw_len, out_len);
    }
    /* No filter or unsupported: return raw copy */
    unsigned char *copy = (unsigned char *)xmalloc(obj->stream.raw_len + 1);
    memcpy(copy, obj->stream.raw, obj->stream.raw_len);
    copy[obj->stream.raw_len] = 0;
    *out_len = obj->stream.raw_len;
    return copy;
}

/* Count pages by walking the page tree */
static int count_pages(PdfDoc *doc, PdfObj *node) {
    PdfObj *type = pdf_dict_get(doc, node, "Type");
    if (type && type->type == PDF_NAME && strcmp((char *)type->str.data, "Pages") == 0) {
        PdfObj *cnt = pdf_dict_get(doc, node, "Count");
        if (cnt && cnt->type == PDF_INT) return (int)cnt->ival;
    }
    return 1;
}

int pdf_page_count(PdfDoc *doc) {
    return doc->page_count;
}

/* Collect all leaf page nodes into a flat array */
typedef struct { PdfObj **pages; int count; int cap; } PageList;

static void collect_pages(PdfDoc *doc, PdfObj *node, PageList *pl) {
    node = pdf_resolve(doc, node);
    if (!node) return;
    PdfObj *type = pdf_dict_get(doc, node, "Type");
    if (type && type->type == PDF_NAME && strcmp((char *)type->str.data, "Pages") == 0) {
        PdfObj *kids = pdf_dict_get(doc, node, "Kids");
        if (!kids || kids->type != PDF_ARRAY) return;
        int i;
        for (i = 0; i < kids->arr.count; i++)
            collect_pages(doc, kids->arr.items[i], pl);
    } else {
        if (pl->count >= pl->cap) {
            pl->cap = pl->cap ? pl->cap * 2 : 64;
            pl->pages = (PdfObj **)xrealloc(pl->pages, pl->cap * sizeof(PdfObj *));
        }
        pl->pages[pl->count++] = node;
    }
}

PdfObj *pdf_get_page(PdfDoc *doc, int index) {
    if (index < 0 || index >= doc->page_count) return NULL;
    /* Walk page tree */
    PageList pl = {0};
    if (doc->pages_root) collect_pages(doc, doc->pages_root, &pl);
    PdfObj *result = (index < pl.count) ? pl.pages[index] : NULL;
    free(pl.pages);
    return result;
}

void pdf_page_box(PdfDoc *doc, PdfObj *page, double *x0, double *y0, double *x1, double *y1) {
    PdfObj *box = pdf_dict_get(doc, page, "MediaBox");
    if (!box) box = pdf_dict_get(doc, page, "CropBox");
    if (box && box->type == PDF_ARRAY && box->arr.count >= 4) {
        *x0 = pdf_num(pdf_resolve(doc, box->arr.items[0]));
        *y0 = pdf_num(pdf_resolve(doc, box->arr.items[1]));
        *x1 = pdf_num(pdf_resolve(doc, box->arr.items[2]));
        *y1 = pdf_num(pdf_resolve(doc, box->arr.items[3]));
    } else {
        *x0 = 0; *y0 = 0; *x1 = 612; *y1 = 792; /* A4-ish default */
    }
}

PdfDoc *pdf_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }

    PdfDoc *doc = (PdfDoc *)xmalloc(sizeof(PdfDoc));
    memset(doc, 0, sizeof(*doc));
    doc->buf     = (unsigned char *)xmalloc((int)size + 1);
    doc->buf_len = (int)size;
    fread(doc->buf, 1, size, f);
    doc->buf[size] = 0;
    fclose(f);

    if (size < 8 || memcmp(doc->buf, "%PDF-", 5) != 0) {
        pdf_free(doc);
        return NULL;
    }

    long long sxref = find_startxref(doc);
    if (sxref < 0) { pdf_free(doc); return NULL; }

    /* Determine if xref is a table or a stream */
    const unsigned char *p = doc->buf + sxref;
    int rem = doc->buf_len - (int)sxref;
    if (rem >= 4 && memcmp(p, "xref", 4) == 0)
        parse_xref_table(doc, (int)sxref, &doc->trailer);
    else
        parse_xref_stream(doc, (int)sxref);

    /* Handle /Prev chains */
    PdfObj *trailer = doc->trailer;
    while (trailer) {
        PdfObj *prev = NULL;
        int i;
        PdfDict *td = (trailer->type == PDF_DICT) ? &trailer->dict : NULL;
        if (!td) break;
        for (i = 0; i < td->count; i++) {
            if (strcmp(td->keys[i], "Prev") == 0 && td->vals[i]->type == PDF_INT) {
                long long poff = td->vals[i]->ival;
                const unsigned char *pp = doc->buf + poff;
                int prem = doc->buf_len - (int)poff;
                if (prem >= 4 && memcmp(pp, "xref", 4) == 0) {
                    PdfObj *old_trailer = NULL;
                    parse_xref_table(doc, (int)poff, &old_trailer);
                    prev = old_trailer;
                } else {
                    parse_xref_stream(doc, (int)poff);
                }
            }
        }
        if (!prev) break;
        trailer = prev;
    }

    /* Resolve root */
    if (!doc->trailer) { pdf_free(doc); return NULL; }
    doc->root = pdf_dict_get(doc, doc->trailer, "Root");
    if (!doc->root) { pdf_free(doc); return NULL; }
    doc->pages_root = pdf_dict_get(doc, doc->root, "Pages");
    if (!doc->pages_root) { pdf_free(doc); return NULL; }
    doc->page_count = count_pages(doc, doc->pages_root);

    return doc;
}

void pdf_free(PdfDoc *doc) {
    if (!doc) return;
    free(doc->buf);
    int i;
    if (doc->cache) {
        for (i = 0; i < doc->xref_cap; i++) obj_free(doc->cache[i]);
        free(doc->cache);
    }
    free(doc->xref);
    obj_free(doc->trailer);
    free(doc);
}
