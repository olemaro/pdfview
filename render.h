#ifndef RENDER_H
#define RENDER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pdf.h"

/* Render page at (dst_x, dst_y) with given zoom factor.
   zoom = 1.0 means 1 pt → 1 px (96 dpi equivalent handled inside). */
void pdf_render_page(HDC hdc, PdfDoc *doc, PdfObj *page,
                     int dst_x, int dst_y, int dst_w, int dst_h,
                     double zoom);

#endif
