#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pdf.hpp"

void pdf_render_page(HDC hdc, PdfDoc& doc, ObjPtr page,
                     int dst_x, int dst_y, int dst_w, int dst_h,
                     double zoom);
