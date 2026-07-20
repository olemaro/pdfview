#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdf.h"
#include "render.h"

/* ================================================================
   Global state
   ================================================================ */

static PdfDoc  *g_doc      = NULL;
static int      g_page     = 0;
static double   g_zoom     = 1.0;   /* 1.0 = 100% */
static int      g_scroll_y = 0;
static HBITMAP  g_bmp      = NULL;
static HDC      g_bmp_dc   = NULL;
static int      g_bmp_w    = 0;
static int      g_bmp_h    = 0;
static HWND     g_hwnd     = NULL;

static char g_title[512] = "PDF Viewer";

/* ================================================================
   Bitmap management
   ================================================================ */

static void destroy_bmp(void) {
    if (g_bmp_dc) { DeleteDC(g_bmp_dc); g_bmp_dc = NULL; }
    if (g_bmp)    { DeleteObject(g_bmp); g_bmp    = NULL; }
    g_bmp_w = g_bmp_h = 0;
}

static void ensure_bmp(HDC ref_dc, int w, int h) {
    if (g_bmp_w == w && g_bmp_h == h) return;
    destroy_bmp();
    g_bmp_dc = CreateCompatibleDC(ref_dc);
    g_bmp    = CreateCompatibleBitmap(ref_dc, w, h);
    SelectObject(g_bmp_dc, g_bmp);
    g_bmp_w = w;
    g_bmp_h = h;
}

static void zoom_fit_width(void); /* forward declaration */

/* ================================================================
   Rendering
   ================================================================ */

/* Page dimensions in pixels at current zoom */
static void page_size_px(int *w, int *h) {
    if (!g_doc) { *w = 640; *h = 800; return; }
    PdfObj *pg = pdf_get_page(g_doc, g_page);
    double x0,y0,x1,y1;
    if (pg) pdf_page_box(g_doc, pg, &x0, &y0, &x1, &y1);
    else { x0=0; y0=0; x1=612; y1=792; }
    *w = (int)((x1-x0) * g_zoom + 0.5);
    *h = (int)((y1-y0) * g_zoom + 0.5);
}

static void render_page(void) {
    if (!g_doc || !g_hwnd) return;
    PdfObj *pg = pdf_get_page(g_doc, g_page);
    if (!pg) return;

    int pw, ph;
    page_size_px(&pw, &ph);

    HDC hdc = GetDC(g_hwnd);
    ensure_bmp(hdc, pw, ph);

    pdf_render_page(g_bmp_dc, g_doc, pg, 0, 0, pw, ph, g_zoom);

    ReleaseDC(g_hwnd, hdc);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ================================================================
   File opening
   ================================================================ */

static void open_file(const char *path) {
    PdfDoc *doc = pdf_open(path);
    if (!doc) {
        MessageBoxA(g_hwnd, "Could not open PDF file.\n"
                    "File may be corrupt, encrypted, or unsupported.",
                    "Error", MB_ICONERROR | MB_OK);
        return;
    }
    if (g_doc) pdf_free(g_doc);
    g_doc      = doc;
    g_page     = 0;
    g_scroll_y = 0;
    g_zoom     = 1.0;

    /* Update window title */
    const char *base = strrchr(path, '\\');
    if (!base) base = strrchr(path, '/');
    if (base) base++; else base = path;
    char title[512];
    snprintf(title, sizeof(title), "PDF Viewer — %s (%d pages)",
             base, pdf_page_count(doc));
    SetWindowTextA(g_hwnd, title);

    zoom_fit_width();
}

static void open_file_dialog(void) {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = "PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = "Open PDF";
    if (GetOpenFileNameA(&ofn))
        open_file(path);
}

/* ================================================================
   WndProc
   ================================================================ */

#define IDM_OPEN    101
#define IDM_PREV    102
#define IDM_NEXT    103
#define IDM_ZOOM_IN 104
#define IDM_ZOOM_OUT 105
#define IDM_FIT     106

static void update_status(void) {
    if (!g_doc) return;
    char title[512];
    const char *name = strrchr(g_title, '\\');
    if (!name) name = g_title; else name++;
    snprintf(title, sizeof(title), "PDF Viewer — page %d / %d  (zoom %.0f%%)",
             g_page+1, pdf_page_count(g_doc), g_zoom*100.0);
    SetWindowTextA(g_hwnd, title);
}

static void zoom_fit_width(void) {
    if (!g_doc) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    PdfObj *pg = pdf_get_page(g_doc, g_page);
    double x0,y0,x1,y1;
    if (pg) pdf_page_box(g_doc, pg, &x0, &y0, &x1, &y1);
    else { x0=0;y0=0;x1=612;y1=792; }
    double page_w = x1 - x0;
    if (page_w < 1) return;
    int client_w = rc.right - rc.left - 20; /* 10px margin each side */
    if (client_w < 10) client_w = 10;
    g_zoom = (double)client_w / page_w;
    g_scroll_y = 0;
    render_page();
    update_status();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        /* Menu */
        HMENU menu  = CreateMenu();
        HMENU file  = CreatePopupMenu();
        HMENU view  = CreatePopupMenu();
        AppendMenuA(file, MF_STRING, IDM_OPEN,    "&Open...\tCtrl+O");
        AppendMenuA(file, MF_SEPARATOR, 0, NULL);
        AppendMenuA(file, MF_STRING, IDCANCEL,    "E&xit");
        AppendMenuA(view, MF_STRING, IDM_PREV,    "&Previous page\tLeft");
        AppendMenuA(view, MF_STRING, IDM_NEXT,    "&Next page\tRight");
        AppendMenuA(view, MF_SEPARATOR, 0, NULL);
        AppendMenuA(view, MF_STRING, IDM_ZOOM_IN, "Zoom &In\t+");
        AppendMenuA(view, MF_STRING, IDM_ZOOM_OUT,"Zoom &Out\t-");
        AppendMenuA(view, MF_STRING, IDM_FIT,     "&Fit Width\tF");
        AppendMenuA(menu, MF_POPUP, (UINT_PTR)file, "&File");
        AppendMenuA(menu, MF_POPUP, (UINT_PTR)view, "&View");
        SetMenu(hwnd, menu);
        DragAcceptFiles(hwnd, TRUE);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;

        /* background */
        HBRUSH bg = CreateSolidBrush(RGB(60,60,60));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        if (g_bmp && g_bmp_dc) {
            /* center page horizontally, account for scroll vertically */
            int pw = g_bmp_w, ph = g_bmp_h;
            int ox = (cw - pw) / 2;
            if (ox < 0) ox = 0;
            int oy = -g_scroll_y;

            /* page shadow */
            RECT sr = {ox+3, oy+3, ox+pw+3, oy+ph+3};
            HBRUSH shadow = CreateSolidBrush(RGB(20,20,20));
            FillRect(hdc, &sr, shadow);
            DeleteObject(shadow);

            BitBlt(hdc, ox, oy, pw, ph, g_bmp_dc, 0, 0, SRCCOPY);
        } else if (!g_doc) {
            /* hint text */
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(180,180,180));
            HFONT hf = CreateFontA(-24,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
            HFONT old = (HFONT)SelectObject(hdc, hf);
            const char *hint = "Open a PDF:  File > Open  or  drag & drop";
            RECT tr = {0,0,cw,ch};
            DrawTextA(hdc, hint, -1, &tr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(hdc, old);
            DeleteObject(hf);
        }
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:   open_file_dialog(); break;
        case IDCANCEL:   PostQuitMessage(0); break;
        case IDM_PREV:
            if (g_doc && g_page > 0) {
                g_page--; g_scroll_y = 0;
                render_page(); update_status();
            }
            break;
        case IDM_NEXT:
            if (g_doc && g_page < pdf_page_count(g_doc)-1) {
                g_page++; g_scroll_y = 0;
                render_page(); update_status();
            }
            break;
        case IDM_ZOOM_IN:
            g_zoom *= 1.25; if (g_zoom > 8.0) g_zoom = 8.0;
            render_page(); update_status(); break;
        case IDM_ZOOM_OUT:
            g_zoom /= 1.25; if (g_zoom < 0.1) g_zoom = 0.1;
            render_page(); update_status(); break;
        case IDM_FIT:
            zoom_fit_width(); break;
        }
        break;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  case VK_PRIOR:
            if (g_doc && g_page > 0) { g_page--; g_scroll_y=0; render_page(); update_status(); }
            break;
        case VK_RIGHT: case VK_NEXT:
            if (g_doc && g_page < pdf_page_count(g_doc)-1) { g_page++; g_scroll_y=0; render_page(); update_status(); }
            break;
        case VK_UP:
            g_scroll_y -= 80;
            if (g_scroll_y < 0) g_scroll_y = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case VK_DOWN:
            g_scroll_y += 80;
            { int ph = g_bmp_h; RECT rc2; GetClientRect(hwnd,&rc2);
              int max_scroll = ph - (rc2.bottom - rc2.top) + 10;
              if (max_scroll < 0) max_scroll = 0;
              if (g_scroll_y > max_scroll) g_scroll_y = max_scroll; }
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case VK_HOME:
            if (g_doc) { g_page=0; g_scroll_y=0; render_page(); update_status(); }
            break;
        case VK_END:
            if (g_doc) { g_page=pdf_page_count(g_doc)-1; g_scroll_y=0; render_page(); update_status(); }
            break;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) open_file_dialog();
            break;
        case VK_OEM_PLUS: case VK_ADD:
            g_zoom *= 1.25; if (g_zoom>8.0) g_zoom=8.0;
            render_page(); update_status(); break;
        case VK_OEM_MINUS: case VK_SUBTRACT:
            g_zoom /= 1.25; if (g_zoom<0.1) g_zoom=0.1;
            render_page(); update_status(); break;
        case 'F':
            zoom_fit_width(); break;
        }
        break;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            /* Ctrl+wheel = zoom */
            if (delta > 0) { g_zoom *= 1.15; if (g_zoom>8.0) g_zoom=8.0; }
            else           { g_zoom /= 1.15; if (g_zoom<0.1) g_zoom=0.1; }
            render_page(); update_status();
        } else {
            /* Scroll */
            g_scroll_y -= delta / 3;
            if (g_scroll_y < 0) g_scroll_y = 0;
            RECT rc2; GetClientRect(hwnd,&rc2);
            int max_scroll = g_bmp_h - (rc2.bottom - rc2.top) + 10;
            if (max_scroll < 0) max_scroll = 0;
            if (g_scroll_y > max_scroll) g_scroll_y = max_scroll;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }

    case WM_SIZE:
        if (g_doc) { zoom_fit_width(); }
        break;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wp;
        char dropped[MAX_PATH] = {0};
        DragQueryFileA(hDrop, 0, dropped, MAX_PATH);
        DragFinish(hDrop);
        open_file(dropped);
        break;
    }

    case WM_DESTROY:
        destroy_bmp();
        if (g_doc) { pdf_free(g_doc); g_doc = NULL; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    return 0;
}

/* ================================================================
   WinMain
   ================================================================ */

int WINAPI WinMainA(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow);

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    (void)hPrev;

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName = "PDFViewerWnd";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        "PDFViewerWnd", "PDF Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
        NULL, NULL, hInst, NULL
    );
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    /* Open file from command line if given */
    if (cmdLine && cmdLine[0]) {
        char path[MAX_PATH];
        strncpy(path, cmdLine, MAX_PATH-1);
        path[MAX_PATH-1] = '\0';
        /* strip surrounding quotes */
        int len = (int)strlen(path);
        if (len >= 2 && path[0]=='"' && path[len-1]=='"') {
            memmove(path, path+1, len-2);
            path[len-2] = '\0';
        }
        open_file(path);
        zoom_fit_width();
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
