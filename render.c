#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pdf.h"
#include "render.h"

/* ================================================================
   Graphics state
   ================================================================ */

#define GS_STACK 32
#define PI 3.14159265358979323846

typedef struct {
    double a, b, c, d, e, f; /* CTM */
} Matrix;

static Matrix mat_identity(void) {
    Matrix m = {1,0,0,1,0,0};
    return m;
}

static Matrix mat_mul(Matrix a, Matrix b) {
    Matrix r;
    r.a = a.a*b.a + a.b*b.c;
    r.b = a.a*b.b + a.b*b.d;
    r.c = a.c*b.a + a.d*b.c;
    r.d = a.c*b.b + a.d*b.d;
    r.e = a.e*b.a + a.f*b.c + b.e;
    r.f = a.e*b.b + a.f*b.d + b.f;
    return r;
}

/* Transform point */
static void mat_pt(Matrix *m, double x, double y, double *ox, double *oy) {
    *ox = m->a*x + m->c*y + m->e;
    *oy = m->b*x + m->d*y + m->f;
}

typedef struct {
    Matrix  ctm;
    double  line_width;
    COLORREF fill_color;
    COLORREF stroke_color;
    double  font_size;
    char    font_name[128];
    double  char_spacing;
    double  word_spacing;
    double  horiz_scale;   /* percent, default 100 */
    double  text_leading;
    double  text_rise;
} GState;

typedef struct {
    GState  gs;
    Matrix  tm;   /* text matrix */
    Matrix  tlm;  /* text line matrix */
    int     in_text;
    /* path */
    POINT  *path_pts;
    int    *path_verbs; /* 0=move,1=line,2=close */
    int     path_count;
    int     path_cap;
    double  cur_x, cur_y;
    double  sub_x, sub_y;  /* subpath start */
    /* GS stack */
    GState  stack[GS_STACK];
    int     stack_top;
    /* page box */
    double  pg_x0, pg_y0, pg_x1, pg_y1;
    double  zoom;
    HDC     hdc;
    /* font resources */
    PdfDoc *doc;
    PdfObj *page;
} RenderCtx;

/* ================================================================
   Coordinate mapping: PDF → screen
   PDF: origin bottom-left, Y up
   Screen: origin top-left, Y down
   ================================================================ */

static int px(RenderCtx *r, double x) {
    return (int)((x - r->pg_x0) * r->zoom + 0.5);
}
static int py(RenderCtx *r, double y) {
    double ph = r->pg_y1 - r->pg_y0;
    return (int)((ph - (y - r->pg_y0)) * r->zoom + 0.5);
}

/* ================================================================
   Path helpers
   ================================================================ */

static void path_add(RenderCtx *r, double x, double y, int verb) {
    if (r->path_count >= r->path_cap) {
        r->path_cap = r->path_cap ? r->path_cap * 2 : 64;
        r->path_pts   = (POINT *)realloc(r->path_pts,   r->path_cap * sizeof(POINT));
        r->path_verbs = (int *)  realloc(r->path_verbs, r->path_cap * sizeof(int));
    }
    r->path_pts[r->path_count].x = px(r, x);
    r->path_pts[r->path_count].y = py(r, y);
    r->path_verbs[r->path_count] = verb;
    r->path_count++;
}

static void path_clear(RenderCtx *r) {
    r->path_count = 0;
}

/* Send current path to GDI and stroke/fill */
static void gdi_draw_path(RenderCtx *r, int do_fill, int do_stroke) {
    if (r->path_count == 0) return;

    HBRUSH hbr = NULL;
    HPEN   hpn = NULL;

    if (do_fill) {
        hbr = CreateSolidBrush(r->gs.fill_color);
        SelectObject(r->hdc, hbr);
    } else {
        SelectObject(r->hdc, GetStockObject(NULL_BRUSH));
    }
    if (do_stroke) {
        int lw = (int)(r->gs.line_width * r->zoom);
        if (lw < 1) lw = 1;
        hpn = CreatePen(PS_SOLID, lw, r->gs.stroke_color);
        SelectObject(r->hdc, hpn);
    } else {
        SelectObject(r->hdc, GetStockObject(NULL_PEN));
    }

    /* Build a Windows path */
    BeginPath(r->hdc);
    int i;
    for (i = 0; i < r->path_count; i++) {
        int v = r->path_verbs[i];
        if (v == 0) MoveToEx(r->hdc, r->path_pts[i].x, r->path_pts[i].y, NULL);
        else if (v == 1) LineTo(r->hdc, r->path_pts[i].x, r->path_pts[i].y);
        else { /* close */ CloseFigure(r->hdc); }
    }
    EndPath(r->hdc);

    if (do_fill && do_stroke) StrokeAndFillPath(r->hdc);
    else if (do_fill)         FillPath(r->hdc);
    else if (do_stroke)       StrokePath(r->hdc);

    if (hbr) DeleteObject(hbr);
    if (hpn) DeleteObject(hpn);
    path_clear(r);
}

/* ================================================================
   Font mapping
   ================================================================ */

typedef struct { const char *pdf; const char *win; int bold; int italic; } FontMap;
static const FontMap font_map[] = {
    {"Helvetica",            "Arial",           0, 0},
    {"Helvetica-Bold",       "Arial",           1, 0},
    {"Helvetica-Oblique",    "Arial",           0, 1},
    {"Helvetica-BoldOblique","Arial",           1, 1},
    {"Times-Roman",          "Times New Roman", 0, 0},
    {"Times-Bold",           "Times New Roman", 1, 0},
    {"Times-Italic",         "Times New Roman", 0, 1},
    {"Times-BoldItalic",     "Times New Roman", 1, 1},
    {"Courier",              "Courier New",     0, 0},
    {"Courier-Bold",         "Courier New",     1, 0},
    {"Courier-Oblique",      "Courier New",     0, 1},
    {"Courier-BoldOblique",  "Courier New",     1, 1},
    {"Symbol",               "Symbol",          0, 0},
    {"ZapfDingbats",         "Wingdings",       0, 0},
    {NULL, NULL, 0, 0}
};

static HFONT make_font(const char *name, double size_pts, double zoom,
                       double scaleX, double scaleY) {
    const char *face = "Arial";
    int bold = 0, italic = 0;
    int i;
    /* strip prefix like "ABCDEF+" from embedded subset */
    const char *base = strrchr(name, '+');
    if (base) base++; else base = name;

    for (i = 0; font_map[i].pdf; i++) {
        if (stricmp(base, font_map[i].pdf) == 0) {
            face  = font_map[i].win;
            bold  = font_map[i].bold;
            italic = font_map[i].italic;
            break;
        }
    }
    /* Guess bold/italic from name if not found */
    if (!font_map[i].pdf) {
        if (strstr(base, "Bold")   || strstr(base, "bold"))   bold   = 1;
        if (strstr(base, "Italic") || strstr(base, "italic") ||
            strstr(base, "Oblique")|| strstr(base, "oblique")) italic = 1;
    }

    int height = (int)(size_pts * zoom * scaleY + 0.5);
    if (height < 1) height = 1;
    if (height > 2000) height = 2000;

    int width = 0;
    if (scaleX != 1.0 && scaleX > 0.01)
        width = (int)(height * scaleX + 0.5);

    return CreateFontA(
        -height, width, 0, 0,
        bold ? FW_BOLD : FW_NORMAL,
        italic, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        face
    );
}

/* ================================================================
   Text: convert PDF string to WinAnsi text
   ================================================================ */

/* Show a PDF string at current text position using current font */
static void show_string(RenderCtx *r, const unsigned char *str, int len) {
    if (len <= 0) return;

    /* Compute screen position from Trm = Tm * CTM */
    Matrix trm = mat_mul(r->tm, r->gs.ctm);

    /* Font scale from matrix diagonal */
    double scaleX = sqrt(trm.a*trm.a + trm.b*trm.b);
    double scaleY = sqrt(trm.c*trm.c + trm.d*trm.d);
    if (scaleY < 0.01) scaleY = scaleX;
    if (scaleX < 0.01) scaleX = scaleY;

    /* Is it a UTF-16BE string? (BOM FE FF) */
    int is_wide = (len >= 2 && str[0] == 0xFE && str[1] == 0xFF);

    /* Build char buffer */
    char local[4096];
    int text_len = 0;
    if (is_wide) {
        int j;
        for (j = 2; j+1 < len && text_len < (int)sizeof(local)-1; j += 2) {
            unsigned int cp = (str[j] << 8) | str[j+1];
            if (cp < 128) local[text_len++] = (char)cp;
            else {
                /* Convert to WinAnsi approximation */
                local[text_len++] = (cp < 256) ? (char)cp : '?';
            }
        }
    } else {
        int j;
        for (j = 0; j < len && text_len < (int)sizeof(local)-1; j++)
            local[text_len++] = (char)str[j];
    }
    local[text_len] = '\0';
    if (text_len == 0) return;

    /* Create font */
    double font_scaleX = r->gs.horiz_scale / 100.0;
    HFONT hfont = make_font(r->gs.font_name, r->gs.font_size, r->zoom,
                            font_scaleX, scaleY);
    HFONT old_font = (HFONT)SelectObject(r->hdc, hfont);

    /* Position */
    double sx, sy;
    mat_pt(&trm, 0, r->gs.text_rise, &sx, &sy);

    SetBkMode(r->hdc, TRANSPARENT);
    SetTextColor(r->hdc, r->gs.fill_color);
    SetTextAlign(r->hdc, TA_BASELINE | TA_LEFT);

    /* Rotation angle from matrix */
    double angle_rad = atan2(trm.b, trm.a);
    double angle_deg = angle_rad * 180.0 / PI;

    if (fabs(angle_deg) > 0.5) {
        /* Recreate font with escapement */
        SelectObject(r->hdc, old_font);
        DeleteObject(hfont);
        int esc = (int)(angle_deg * 10.0 + 0.5);
        int height = (int)(r->gs.font_size * scaleY * r->zoom + 0.5);
        if (height < 1) height = 1;
        int bold = 0, italic = 0;
        const char *base = strrchr(r->gs.font_name, '+');
        if (base) base++; else base = r->gs.font_name;
        if (strstr(base,"Bold")||strstr(base,"bold")) bold=1;
        if (strstr(base,"Italic")||strstr(base,"italic")||strstr(base,"Oblique")) italic=1;
        hfont = CreateFontA(-height, 0, esc, 0,
                            bold ? FW_BOLD : FW_NORMAL, italic, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, "Arial");
        old_font = (HFONT)SelectObject(r->hdc, hfont);
    }

    TextOutA(r->hdc, px(r, sx), py(r, sy), local, text_len);

    /* Advance text matrix: measure pixel width, convert to user-space advance */
    SIZE sz;
    GetTextExtentPoint32A(r->hdc, local, text_len, &sz);
    double adv_user = (double)sz.cx / r->zoom
                    + r->gs.char_spacing * text_len;
    /* Project advance along text direction (Tm rows give the direction) */
    double tm_sx = sqrt(r->tm.a * r->tm.a + r->tm.b * r->tm.b);
    if (tm_sx > 1e-6) {
        r->tm.e += adv_user * r->tm.a / tm_sx;
        r->tm.f += adv_user * r->tm.b / tm_sx;
    } else {
        r->tm.e += adv_user;
    }

    SelectObject(r->hdc, old_font);
    DeleteObject(hfont);
}

/* ================================================================
   Content stream tokenizer
   ================================================================ */

#define TOK_NUM    1
#define TOK_STR    2
#define TOK_NAME   3
#define TOK_ARRAY  4  /* not used */
#define TOK_OP     5
#define TOK_BOOL   6
#define TOK_EOF    0

typedef struct {
    int    type;
    double num;
    char   str[256];      /* for operator name or name */
    unsigned char *bytes; /* for string */
    int    bytes_len;
} Token;

#define OPSTACK_MAX 64

typedef struct {
    double nums[OPSTACK_MAX];
    int    n_nums;
    unsigned char *strs[OPSTACK_MAX];
    int            str_lens[OPSTACK_MAX];
    int    n_strs;
} OpStack;

static void opstack_reset(OpStack *os) {
    int i;
    for (i = 0; i < os->n_strs; i++) free(os->strs[i]);
    os->n_nums = 0;
    os->n_strs = 0;
}

static void opstack_push_num(OpStack *os, double v) {
    if (os->n_nums < OPSTACK_MAX) os->nums[os->n_nums++] = v;
}

static void opstack_push_str(OpStack *os, unsigned char *data, int len) {
    if (os->n_strs < OPSTACK_MAX) {
        unsigned char *copy = (unsigned char *)malloc(len + 1);
        if (copy) { memcpy(copy, data, len); copy[len] = 0; }
        os->strs[os->n_strs]     = copy;
        os->str_lens[os->n_strs] = len;
        os->n_strs++;
    }
}

static double get_num(OpStack *os, int from_top) {
    int idx = os->n_nums - 1 - from_top;
    return (idx >= 0) ? os->nums[idx] : 0.0;
}

/* ================================================================
   Execute one PDF graphics operator
   ================================================================ */

static void exec_op(RenderCtx *r, OpStack *os, const char *op) {
    int n = os->n_nums;
    double a = get_num(os,0), b = get_num(os,1), c2 = get_num(os,2),
           d = get_num(os,3), e = get_num(os,4), f = get_num(os,5);

    /* ---- graphics state ---- */
    if (strcmp(op,"q")==0) {
        if (r->stack_top < GS_STACK) r->stack[r->stack_top++] = r->gs;
    } else if (strcmp(op,"Q")==0) {
        if (r->stack_top > 0) r->gs = r->stack[--r->stack_top];
    } else if (strcmp(op,"cm")==0 && n >= 6) {
        Matrix m = {f, e, d, c2, b, a};  /* operands pushed as f,e,d,c,b,a (top=a) */
        /* Actually operands are: a b c d e f → stack top is f */
        m.a = get_num(os,5); m.b = get_num(os,4); m.c = get_num(os,3);
        m.d = get_num(os,2); m.e = get_num(os,1); m.f = get_num(os,0);
        r->gs.ctm = mat_mul(m, r->gs.ctm);
    } else if (strcmp(op,"w")==0 && n>=1) {
        r->gs.line_width = a;
    } else if (strcmp(op,"gs")==0) {
        /* ignore extended graphics state for now */
    }

    /* ---- color ---- */
    else if (strcmp(op,"rg")==0 && n>=3) {
        int R=(int)(c2*255), G=(int)(b*255), B=(int)(a*255);
        r->gs.fill_color = RGB(R,G,B);
    } else if (strcmp(op,"RG")==0 && n>=3) {
        int R=(int)(c2*255), G=(int)(b*255), B=(int)(a*255);
        r->gs.stroke_color = RGB(R,G,B);
    } else if (strcmp(op,"g")==0 && n>=1) {
        int v=(int)(a*255);
        r->gs.fill_color = RGB(v,v,v);
    } else if (strcmp(op,"G")==0 && n>=1) {
        int v=(int)(a*255);
        r->gs.stroke_color = RGB(v,v,v);
    } else if (strcmp(op,"k")==0 && n>=4) {
        /* CMYK fill */
        double C=get_num(os,3),M=get_num(os,2),Y=get_num(os,1),K=get_num(os,0);
        int R=(int)((1-C)*(1-K)*255), G=(int)((1-M)*(1-K)*255), B2=(int)((1-Y)*(1-K)*255);
        r->gs.fill_color = RGB(R,G,B2);
    } else if (strcmp(op,"K")==0 && n>=4) {
        double C=get_num(os,3),M=get_num(os,2),Y=get_num(os,1),K=get_num(os,0);
        int R=(int)((1-C)*(1-K)*255), G=(int)((1-M)*(1-K)*255), B2=(int)((1-Y)*(1-K)*255);
        r->gs.stroke_color = RGB(R,G,B2);
    } else if (strcmp(op,"sc")==0 || strcmp(op,"scn")==0) {
        if (n==1) { int v=(int)(a*255); r->gs.fill_color=RGB(v,v,v); }
        else if (n>=3) { int R=(int)(c2*255),G=(int)(b*255),B2=(int)(a*255); r->gs.fill_color=RGB(R,G,B2); }
    } else if (strcmp(op,"SC")==0 || strcmp(op,"SCN")==0) {
        if (n==1) { int v=(int)(a*255); r->gs.stroke_color=RGB(v,v,v); }
        else if (n>=3) { int R=(int)(c2*255),G=(int)(b*255),B2=(int)(a*255); r->gs.stroke_color=RGB(R,G,B2); }
    } else if (strcmp(op,"cs")==0 || strcmp(op,"CS")==0) {
        /* color space - ignore */
    }

    /* ---- path construction ---- */
    else if (strcmp(op,"m")==0 && n>=2) {
        double sx, sy;
        mat_pt(&r->gs.ctm, b, a, &sx, &sy);
        path_add(r, sx, sy, 0);
        r->cur_x = sx; r->cur_y = sy;
        r->sub_x = sx; r->sub_y = sy;
    } else if (strcmp(op,"l")==0 && n>=2) {
        double sx, sy;
        mat_pt(&r->gs.ctm, b, a, &sx, &sy);
        path_add(r, sx, sy, 1);
        r->cur_x = sx; r->cur_y = sy;
    } else if (strcmp(op,"c")==0 && n>=6) {
        /* Bezier: approximate with 4 line segments */
        double x0=r->cur_x, y0=r->cur_y;
        double x1,y1, x2,y2, x3,y3;
        mat_pt(&r->gs.ctm, get_num(os,5),get_num(os,4), &x1,&y1);
        mat_pt(&r->gs.ctm, get_num(os,3),get_num(os,2), &x2,&y2);
        mat_pt(&r->gs.ctm, get_num(os,1),get_num(os,0), &x3,&y3);
        int seg; double t;
        for (seg=1; seg<=4; seg++) {
            t = seg/4.0;
            double mt=1-t;
            double bx = mt*mt*mt*x0 + 3*mt*mt*t*x1 + 3*mt*t*t*x2 + t*t*t*x3;
            double by = mt*mt*mt*y0 + 3*mt*mt*t*y1 + 3*mt*t*t*y2 + t*t*t*y3;
            path_add(r, bx, by, 1);
        }
        r->cur_x = x3; r->cur_y = y3;
    } else if (strcmp(op,"v")==0 && n>=4) {
        double x0=r->cur_x, y0=r->cur_y;
        double x2,y2,x3,y3;
        mat_pt(&r->gs.ctm, get_num(os,3),get_num(os,2), &x2,&y2);
        mat_pt(&r->gs.ctm, get_num(os,1),get_num(os,0), &x3,&y3);
        int seg; double t;
        for (seg=1; seg<=4; seg++) {
            t=seg/4.0; double mt=1-t;
            double bx=mt*mt*mt*x0+3*mt*mt*t*x0+3*mt*t*t*x2+t*t*t*x3;
            double by=mt*mt*mt*y0+3*mt*mt*t*y0+3*mt*t*t*y2+t*t*t*y3;
            path_add(r, bx, by, 1);
        }
        r->cur_x=x3; r->cur_y=y3;
    } else if (strcmp(op,"y")==0 && n>=4) {
        double x0=r->cur_x, y0=r->cur_y;
        double x1,y1,x3,y3;
        mat_pt(&r->gs.ctm, get_num(os,3),get_num(os,2), &x1,&y1);
        mat_pt(&r->gs.ctm, get_num(os,1),get_num(os,0), &x3,&y3);
        int seg; double t;
        for (seg=1; seg<=4; seg++) {
            t=seg/4.0; double mt=1-t;
            double bx=mt*mt*mt*x0+3*mt*mt*t*x1+3*mt*t*t*x3+t*t*t*x3;
            double by=mt*mt*mt*y0+3*mt*mt*t*y1+3*mt*t*t*y3+t*t*t*y3;
            path_add(r, bx, by, 1);
        }
        r->cur_x=x3; r->cur_y=y3;
    } else if (strcmp(op,"h")==0) {
        path_add(r, r->sub_x, r->sub_y, 2);
        r->cur_x = r->sub_x; r->cur_y = r->sub_y;
    } else if (strcmp(op,"re")==0 && n>=4) {
        double rx, ry, rw, rh;
        rx = get_num(os,3); ry = get_num(os,2); rw = get_num(os,1); rh = get_num(os,0);
        double p0x,p0y, p1x,p1y, p2x,p2y, p3x,p3y;
        mat_pt(&r->gs.ctm, rx,    ry,    &p0x,&p0y);
        mat_pt(&r->gs.ctm, rx+rw, ry,    &p1x,&p1y);
        mat_pt(&r->gs.ctm, rx+rw, ry+rh, &p2x,&p2y);
        mat_pt(&r->gs.ctm, rx,    ry+rh, &p3x,&p3y);
        path_add(r, p0x,p0y, 0);
        path_add(r, p1x,p1y, 1);
        path_add(r, p2x,p2y, 1);
        path_add(r, p3x,p3y, 1);
        path_add(r, p0x,p0y, 2);
        r->sub_x=p0x; r->sub_y=p0y;
        r->cur_x=p0x; r->cur_y=p0y;
    }

    /* ---- path painting ---- */
    else if (strcmp(op,"S")==0)  { gdi_draw_path(r, 0, 1); }
    else if (strcmp(op,"s")==0)  { path_add(r,r->sub_x,r->sub_y,2); gdi_draw_path(r,0,1); }
    else if (strcmp(op,"f")==0 || strcmp(op,"F")==0) { gdi_draw_path(r,1,0); }
    else if (strcmp(op,"f*")==0) { gdi_draw_path(r,1,0); }
    else if (strcmp(op,"B")==0)  { gdi_draw_path(r,1,1); }
    else if (strcmp(op,"B*")==0) { gdi_draw_path(r,1,1); }
    else if (strcmp(op,"b")==0)  { path_add(r,r->sub_x,r->sub_y,2); gdi_draw_path(r,1,1); }
    else if (strcmp(op,"b*")==0) { path_add(r,r->sub_x,r->sub_y,2); gdi_draw_path(r,1,1); }
    else if (strcmp(op,"n")==0)  { path_clear(r); }
    else if (strcmp(op,"W")==0 || strcmp(op,"W*")==0) { path_clear(r); } /* clip - skip */

    /* ---- text state ---- */
    else if (strcmp(op,"BT")==0) {
        r->in_text = 1;
        r->tm  = mat_identity();
        r->tlm = mat_identity();
    } else if (strcmp(op,"ET")==0) {
        r->in_text = 0;
    } else if (strcmp(op,"Tf")==0 && n>=1 && os->n_strs>=1) {
        /* /FontName size Tf — name is on string stack */
        r->gs.font_size = a;
        strncpy(r->gs.font_name, (char *)os->strs[os->n_strs-1],
                sizeof(r->gs.font_name)-1);
        r->gs.font_name[sizeof(r->gs.font_name)-1] = '\0';

        /* Try to resolve actual BaseFont name from page resources */
        PdfObj *fonts = NULL;
        PdfObj *res = pdf_dict_get(r->doc, r->page, "Resources");
        if (res) fonts = pdf_dict_get(r->doc, res, "Font");
        if (fonts) {
            PdfObj *fobj = pdf_dict_get(r->doc, fonts, r->gs.font_name);
            if (fobj) {
                PdfObj *base = pdf_dict_get(r->doc, fobj, "BaseFont");
                if (base && base->type == PDF_NAME)
                    strncpy(r->gs.font_name, (char *)base->str.data, sizeof(r->gs.font_name)-1);
            }
        }
    } else if (strcmp(op,"Tc")==0 && n>=1) {
        r->gs.char_spacing = a;
    } else if (strcmp(op,"Tw")==0 && n>=1) {
        r->gs.word_spacing = a;
    } else if (strcmp(op,"Tz")==0 && n>=1) {
        r->gs.horiz_scale = a;
    } else if (strcmp(op,"TL")==0 && n>=1) {
        r->gs.text_leading = a;
    } else if (strcmp(op,"Ts")==0 && n>=1) {
        r->gs.text_rise = a;
    }

    /* ---- text positioning ---- */
    else if (strcmp(op,"Td")==0 && n>=2) {
        /* move to next line */
        double dx=b, dy=a;
        Matrix trans = {1,0,0,1,dx,dy};
        r->tlm = mat_mul(trans, r->tlm);
        r->tm  = r->tlm;
    } else if (strcmp(op,"TD")==0 && n>=2) {
        r->gs.text_leading = -a;
        double dx=b, dy=a;
        Matrix trans = {1,0,0,1,dx,dy};
        r->tlm = mat_mul(trans, r->tlm);
        r->tm  = r->tlm;
    } else if (strcmp(op,"Tm")==0 && n>=6) {
        r->tm.a = get_num(os,5); r->tm.b = get_num(os,4);
        r->tm.c = get_num(os,3); r->tm.d = get_num(os,2);
        r->tm.e = get_num(os,1); r->tm.f = get_num(os,0);
        r->tlm = r->tm;
    } else if (strcmp(op,"T*")==0) {
        Matrix trans = {1,0,0,1,0,-r->gs.text_leading};
        r->tlm = mat_mul(trans, r->tlm);
        r->tm  = r->tlm;
    }

    /* ---- text showing ---- */
    else if ((strcmp(op,"Tj")==0 || strcmp(op,"'")==0) && os->n_strs>=1) {
        if (strcmp(op,"'")==0) {
            Matrix trans = {1,0,0,1,0,-r->gs.text_leading};
            r->tlm = mat_mul(trans, r->tlm);
            r->tm  = r->tlm;
        }
        int si = os->n_strs - 1;
        show_string(r, os->strs[si], os->str_lens[si]);
    } else if (strcmp(op,"\"")==0 && n>=2 && os->n_strs>=1) {
        r->gs.word_spacing  = b;
        r->gs.char_spacing  = a;
        Matrix trans = {1,0,0,1,0,-r->gs.text_leading};
        r->tlm = mat_mul(trans, r->tlm);
        r->tm  = r->tlm;
        int si = os->n_strs - 1;
        show_string(r, os->strs[si], os->str_lens[si]);
    } else if (strcmp(op,"TJ")==0 && os->n_strs>=1) {
        /* TJ: array of strings and numbers */
        /* The array is in os->strs[0] as a raw byte array — we encode differently below */
    }

    /* ---- do (XObject) ---- */
    /* skip inline images and XObjects for now */

    (void)b; (void)c2; (void)d; (void)e; (void)f;
}

/* ================================================================
   TJ special handling: parse array inline in the stream
   ================================================================ */

/* ================================================================
   Main content stream executor
   ================================================================ */

static void exec_stream(RenderCtx *r, const unsigned char *data, int len) {
    int pos = 0;
    OpStack os;
    memset(&os, 0, sizeof(os));

    while (pos < len) {
        /* skip whitespace */
        while (pos < len && (data[pos]==' '||data[pos]=='\t'||data[pos]=='\r'||
                              data[pos]=='\n'||data[pos]=='\f')) pos++;
        if (pos >= len) break;

        int c = data[pos];

        /* Comment */
        if (c == '%') {
            while (pos < len && data[pos] != '\n' && data[pos] != '\r') pos++;
            continue;
        }

        /* Literal string */
        if (c == '(') {
            /* parse string */
            unsigned char tmp[65536];
            int tlen = 0, depth = 1;
            pos++; /* skip '(' */
            while (pos < len && depth > 0) {
                int ch = data[pos++];
                if (ch == '\\') {
                    int e2 = (pos < len) ? data[pos++] : 0;
                    switch (e2) {
                    case 'n': ch='\n'; break; case 'r': ch='\r'; break;
                    case 't': ch='\t'; break; case 'b': ch='\b'; break;
                    case 'f': ch='\f'; break;
                    case '(': ch='('; break; case ')': ch=')'; break;
                    case '\\': ch='\\'; break;
                    default:
                        if (e2>='0'&&e2<='7') {
                            int oc=e2-'0';
                            if (pos<len&&data[pos]>='0'&&data[pos]<='7') oc=oc*8+data[pos++]-'0';
                            if (pos<len&&data[pos]>='0'&&data[pos]<='7') oc=oc*8+data[pos++]-'0';
                            ch=oc;
                        } else ch=e2;
                    }
                    if (tlen<(int)sizeof(tmp)) tmp[tlen++]=(unsigned char)ch;
                } else if (ch=='(') { depth++; if(tlen<(int)sizeof(tmp))tmp[tlen++]='('; }
                else if (ch==')') { if(--depth>0&&tlen<(int)sizeof(tmp))tmp[tlen++]=')'; }
                else { if(tlen<(int)sizeof(tmp))tmp[tlen++]=(unsigned char)ch; }
            }
            opstack_push_str(&os, tmp, tlen);
            continue;
        }

        /* Hex string */
        if (c == '<' && pos+1 < len && data[pos+1] != '<') {
            pos++;
            unsigned char tmp[65536];
            int tlen=0;
            while (pos < len && data[pos] != '>') {
                while (pos<len && isspace(data[pos])) pos++;
                if (pos>=len||data[pos]=='>') break;
                int h = (data[pos]>='0'&&data[pos]<='9')? data[pos]-'0' :
                        (data[pos]>='a'&&data[pos]<='f')? data[pos]-'a'+10 :
                        (data[pos]>='A'&&data[pos]<='F')? data[pos]-'A'+10 : 0;
                pos++;
                while (pos<len&&isspace(data[pos]))pos++;
                int l=0;
                if (pos<len && data[pos] != '>') {
                    l=(data[pos]>='0'&&data[pos]<='9')? data[pos]-'0' :
                      (data[pos]>='a'&&data[pos]<='f')? data[pos]-'a'+10 :
                      (data[pos]>='A'&&data[pos]<='F')? data[pos]-'A'+10 : 0;
                    pos++;
                }
                if (tlen<(int)sizeof(tmp)) tmp[tlen++]=(unsigned char)((h<<4)|l);
            }
            if (pos<len) pos++; /* skip '>' */
            opstack_push_str(&os, tmp, tlen);
            continue;
        }

        /* Name */
        if (c == '/') {
            pos++;
            char nbuf[256]; int ni=0;
            while (pos<len) {
                int nc=data[pos];
                if (nc==' '||nc=='\t'||nc=='\r'||nc=='\n'||nc=='/'||nc=='('||nc==')'||
                    nc=='<'||nc=='>'||nc=='['||nc==']'||nc=='%') break;
                if (nc=='#' && pos+2<len) {
                    pos++;
                    int h2=(data[pos]>='0'&&data[pos]<='9')?data[pos]-'0':(data[pos]>='a'&&data[pos]<='f')?data[pos]-'a'+10:(data[pos]>='A'&&data[pos]<='F')?data[pos]-'A'+10:0; pos++;
                    int l2=(data[pos]>='0'&&data[pos]<='9')?data[pos]-'0':(data[pos]>='a'&&data[pos]<='f')?data[pos]-'a'+10:(data[pos]>='A'&&data[pos]<='F')?data[pos]-'A'+10:0; pos++;
                    if (ni<(int)sizeof(nbuf)-1) nbuf[ni++]=(char)((h2<<4)|l2);
                } else {
                    if (ni<(int)sizeof(nbuf)-1) nbuf[ni++]=(char)nc;
                    pos++;
                }
            }
            nbuf[ni]='\0';
            /* push name as string */
            opstack_push_str(&os, (unsigned char *)nbuf, ni);
            continue;
        }

        /* Array: used by TJ */
        if (c == '[') {
            pos++;
            /* Process TJ array inline */
            while (pos < len) {
                while (pos<len && (data[pos]==' '||data[pos]=='\t'||data[pos]=='\r'||data[pos]=='\n')) pos++;
                if (pos>=len || data[pos]==']') { if(pos<len)pos++; break; }
                int ac = data[pos];
                if (ac == '(') {
                    /* show this string */
                    unsigned char tmp[65536]; int tlen=0, depth=1;
                    pos++;
                    while (pos<len && depth>0) {
                        int ch=data[pos++];
                        if (ch=='\\') {
                            int e2=(pos<len)?data[pos++]:0;
                            switch(e2){case'n':ch='\n';break;case'r':ch='\r';break;case't':ch='\t';break;case'b':ch='\b';break;case'f':ch='\f';break;case'(':ch='(';break;case')':ch=')';break;case'\\':ch='\\';break;default:if(e2>='0'&&e2<='7'){int oc=e2-'0';if(pos<len&&data[pos]>='0'&&data[pos]<='7')oc=oc*8+data[pos++]-'0';if(pos<len&&data[pos]>='0'&&data[pos]<='7')oc=oc*8+data[pos++]-'0';ch=oc;}else ch=e2;}
                            if(tlen<(int)sizeof(tmp))tmp[tlen++]=(unsigned char)ch;
                        } else if (ch=='(') { depth++; if(tlen<(int)sizeof(tmp))tmp[tlen++]='('; }
                        else if (ch==')') { if(--depth>0&&tlen<(int)sizeof(tmp))tmp[tlen++]=')'; }
                        else { if(tlen<(int)sizeof(tmp))tmp[tlen++]=(unsigned char)ch; }
                    }
                    show_string(r, tmp, tlen);
                } else if (ac == '<' && pos+1<len && data[pos+1]!='>') {
                    pos++;
                    unsigned char tmp[65536]; int tlen=0;
                    while(pos<len&&data[pos]!='>'){
                        while(pos<len&&isspace(data[pos]))pos++;
                        if(pos>=len||data[pos]=='>')break;
                        int h=(data[pos]>='0'&&data[pos]<='9')?data[pos]-'0':(data[pos]>='a'&&data[pos]<='f')?data[pos]-'a'+10:(data[pos]>='A'&&data[pos]<='F')?data[pos]-'A'+10:0;pos++;
                        while(pos<len&&isspace(data[pos]))pos++;
                        int l=0;if(pos<len&&data[pos]!='>'){l=(data[pos]>='0'&&data[pos]<='9')?data[pos]-'0':(data[pos]>='a'&&data[pos]<='f')?data[pos]-'a'+10:(data[pos]>='A'&&data[pos]<='F')?data[pos]-'A'+10:0;pos++;}
                        if(tlen<(int)sizeof(tmp))tmp[tlen++]=(unsigned char)((h<<4)|l);
                    }
                    if(pos<len)pos++;
                    show_string(r, tmp, tlen);
                } else if ((ac>='0'&&ac<='9')||ac=='-'||ac=='+') {
                    /* kerning adjustment */
                    char nbuf[32]; int ni=0;
                    while(pos<len&&((data[pos]>='0'&&data[pos]<='9')||data[pos]=='-'||data[pos]=='+'||data[pos]=='.'))
                        if(ni<30)nbuf[ni++]=(char)data[pos++]; else pos++;
                    nbuf[ni]='\0';
                    double kern = atof(nbuf);
                    /* apply kerning: shift Tm by -kern/1000 * font_size in text space */
                    double adv = -kern / 1000.0 * r->gs.font_size;
                    r->tm.e += adv * r->gs.ctm.a;
                    r->tm.f += adv * r->gs.ctm.c;
                } else {
                    pos++;
                }
            }
            opstack_reset(&os);
            continue;
        }

        /* Number */
        if ((c>='0'&&c<='9')||c=='-'||c=='+'||(c=='.'&&pos+1<len&&data[pos+1]>='0'&&data[pos+1]<='9')) {
            char nbuf[64]; int ni=0;
            while (pos<len && ((data[pos]>='0'&&data[pos]<='9')||data[pos]=='-'||
                               data[pos]=='+'||data[pos]=='.')) {
                if (ni<62) nbuf[ni++]=(char)data[pos];
                pos++;
            }
            nbuf[ni]='\0';
            opstack_push_num(&os, atof(nbuf));
            continue;
        }

        /* Skip '<<' dict (inline image params etc.) */
        if (c=='<' && pos+1<len && data[pos+1]=='<') {
            pos+=2; int depth2=1;
            while(pos<len&&depth2>0){
                if(data[pos]=='<'&&pos+1<len&&data[pos+1]=='<'){depth2++;pos+=2;}
                else if(data[pos]=='>'&&pos+1<len&&data[pos+1]=='>'){depth2--;pos+=2;}
                else pos++;
            }
            continue;
        }

        /* Operator / keyword */
        char op[32]; int oi=0;
        while (pos<len) {
            int nc=data[pos];
            if (nc==' '||nc=='\t'||nc=='\r'||nc=='\n'||nc=='('||nc==')'||
                nc=='/'||nc=='<'||nc=='>'||nc=='['||nc==']'||nc=='%') break;
            if (oi<30) op[oi++]=(char)nc;
            pos++;
        }
        op[oi]='\0';
        if (oi==0) { pos++; continue; }

        /* Inline image: skip until EI */
        if (strcmp(op,"BI")==0) {
            const char *found = (const char *)data + pos;
            while (pos+2 < len) {
                if (data[pos]==' '&&data[pos+1]=='E'&&data[pos+2]=='I') { pos+=3; break; }
                if (data[pos]=='E'&&data[pos+1]=='I'&&(pos+2>=len||data[pos+2]==' '||data[pos+2]=='\n'||data[pos+2]=='\r')) { pos+=2; break; }
                pos++;
            }
            opstack_reset(&os);
            (void)found;
            continue;
        }

        exec_op(r, &os, op);
        opstack_reset(&os);
    }

    opstack_reset(&os);
}

/* ================================================================
   Public: render a page to HDC
   ================================================================ */

void pdf_render_page(HDC hdc, PdfDoc *doc, PdfObj *page,
                     int dst_x, int dst_y, int dst_w, int dst_h,
                     double zoom) {
    double x0, y0, x1, y1;
    pdf_page_box(doc, page, &x0, &y0, &x1, &y1);

    /* Fill white background */
    RECT rc = { dst_x, dst_y, dst_x + dst_w, dst_y + dst_h };
    HBRUSH white = CreateSolidBrush(RGB(255,255,255));
    FillRect(hdc, &rc, white);
    DeleteObject(white);

    RenderCtx r;
    memset(&r, 0, sizeof(r));
    r.hdc    = hdc;
    r.doc    = doc;
    r.page   = page;
    r.pg_x0  = x0; r.pg_y0 = y0;
    r.pg_x1  = x1; r.pg_y1 = y1;
    r.zoom   = zoom;

    r.gs.ctm          = mat_identity();
    r.gs.line_width   = 1.0;
    r.gs.fill_color   = RGB(0,0,0);
    r.gs.stroke_color = RGB(0,0,0);
    r.gs.font_size    = 12.0;
    r.gs.horiz_scale  = 100.0;
    strcpy(r.gs.font_name, "Helvetica");

    /* Set clipping to page rect */
    HRGN clip = CreateRectRgn(dst_x, dst_y, dst_x+dst_w, dst_y+dst_h);
    SelectClipRgn(hdc, clip);
    DeleteObject(clip);

    /* Collect content streams */
    PdfObj *contents = pdf_dict_get(doc, page, "Contents");
    if (!contents) { SelectClipRgn(hdc, NULL); return; }

    if (contents->type == PDF_STREAM) {
        int dlen = 0;
        unsigned char *data = pdf_stream_data(doc, contents, &dlen);
        if (data) { exec_stream(&r, data, dlen); free(data); }
    } else if (contents->type == PDF_ARRAY) {
        int i;
        for (i = 0; i < contents->arr.count; i++) {
            PdfObj *cs = pdf_resolve(doc, contents->arr.items[i]);
            if (!cs || cs->type != PDF_STREAM) continue;
            int dlen = 0;
            unsigned char *data = pdf_stream_data(doc, cs, &dlen);
            if (data) { exec_stream(&r, data, dlen); free(data); }
        }
    }

    SelectClipRgn(hdc, NULL);
    free(r.path_pts);
    free(r.path_verbs);
}
