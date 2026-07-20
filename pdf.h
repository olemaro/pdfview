#ifndef PDF_H
#define PDF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PDF object types ---- */
typedef enum {
    PDF_NULL = 0,
    PDF_BOOL,
    PDF_INT,
    PDF_REAL,
    PDF_STRING,   /* byte string */
    PDF_NAME,     /* /Name */
    PDF_ARRAY,
    PDF_DICT,
    PDF_STREAM,
    PDF_REF       /* indirect reference N G R */
} PdfType;

typedef struct PdfObj PdfObj;

typedef struct {
    char    **keys;
    PdfObj  **vals;
    int       count;
    int       cap;
} PdfDict;

typedef struct {
    PdfObj **items;
    int      count;
    int      cap;
} PdfArray;

typedef struct {
    unsigned char *data;
    int            len;
} PdfBuf;

struct PdfObj {
    PdfType type;
    union {
        int       bval;           /* PDF_BOOL */
        long long ival;           /* PDF_INT  */
        double    rval;           /* PDF_REAL */
        PdfBuf    str;            /* PDF_STRING, PDF_NAME */
        PdfArray  arr;            /* PDF_ARRAY */
        PdfDict   dict;           /* PDF_DICT  */
        struct {
            PdfDict        dict;
            unsigned char *raw;   /* compressed stream data */
            int            raw_len;
        } stream;                 /* PDF_STREAM */
        struct { int num; int gen; } ref; /* PDF_REF */
    };
};

/* ---- Cross-reference entry ---- */
typedef struct {
    long long offset; /* byte offset (type 1) or obj num (type 2) */
    int       gen;
    int       type;   /* 0=free, 1=in-use, 2=compressed */
    int       idx;    /* index within compressed object stream */
} XRefEntry;

/* ---- Document ---- */
typedef struct {
    unsigned char *buf;
    int            buf_len;

    XRefEntry     *xref;
    int            xref_cap;
    int            xref_size;  /* max valid object number + 1 */

    PdfObj       **cache;      /* parsed objects, indexed by obj num */

    PdfObj        *trailer;    /* trailer dict */
    PdfObj        *root;       /* catalog */
    PdfObj        *pages_root; /* Pages node */
    int            page_count;
} PdfDoc;

/* ---- Public API ---- */
PdfDoc *pdf_open(const char *path);
void    pdf_free(PdfDoc *doc);

int     pdf_page_count(PdfDoc *doc);
PdfObj *pdf_get_page(PdfDoc *doc, int index);  /* 0-based */

/* Resolve indirect reference */
PdfObj *pdf_resolve(PdfDoc *doc, PdfObj *obj);

/* Get decoded stream data (caller must free) */
unsigned char *pdf_stream_data(PdfDoc *doc, PdfObj *obj, int *out_len);

/* Convenience dict lookup (resolves refs) */
PdfObj *pdf_dict_get(PdfDoc *doc, PdfObj *dict, const char *key);

/* Get numeric value (works for INT and REAL) */
double pdf_num(PdfObj *obj);

/* Get /MediaBox or /CropBox [x0 y0 x1 y1] for page */
void pdf_page_box(PdfDoc *doc, PdfObj *page,
                  double *x0, double *y0, double *x1, double *y1);

#ifdef __cplusplus
}
#endif

#endif /* PDF_H */
