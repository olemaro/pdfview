#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include "pdf.hpp"
#include "render.hpp"

/* ================================================================
   Global state
   ================================================================ */
static std::unique_ptr<PdfDoc> g_doc;
static int     g_page     = 0;
static double  g_zoom     = 1.0;
static int     g_scrollY  = 0;
static HBITMAP g_bmp      = nullptr;
static HDC     g_bmpDC    = nullptr;
static int     g_bmpW     = 0, g_bmpH = 0;
static HWND    g_hwnd     = nullptr;

/* ================================================================
   Bitmap helpers
   ================================================================ */
static void destroyBmp() {
    if (g_bmpDC) { DeleteDC(g_bmpDC);    g_bmpDC=nullptr; }
    if (g_bmp)   { DeleteObject(g_bmp);  g_bmp=nullptr;   }
    g_bmpW=g_bmpH=0;
}
static void ensureBmp(HDC ref, int w, int h) {
    if (g_bmpW==w && g_bmpH==h) return;
    destroyBmp();
    g_bmpDC = CreateCompatibleDC(ref);
    g_bmp   = CreateCompatibleBitmap(ref,w,h);
    SelectObject(g_bmpDC,g_bmp);
    g_bmpW=w; g_bmpH=h;
}

/* ================================================================
   Page geometry
   ================================================================ */
static void pageSizePx(int& w, int& h) {
    if (!g_doc) { w=640; h=800; return; }
    ObjPtr pg=g_doc->getPage(g_page);
    double x0,y0,x1,y1;
    if (pg) g_doc->pageBox(pg,x0,y0,x1,y1);
    else   { x0=0;y0=0;x1=612;y1=792; }
    w=int((x1-x0)*g_zoom+0.5);
    h=int((y1-y0)*g_zoom+0.5);
}

static void updateStatus() {
    if (!g_doc) return;
    char t[256];
    snprintf(t,sizeof(t),"PDF Viewer — page %d / %d  (zoom %.0f%%)",
             g_page+1, g_doc->pageCount(), g_zoom*100.0);
    SetWindowTextA(g_hwnd,t);
}

static void renderPage() {
    if (!g_doc||!g_hwnd) return;
    ObjPtr pg=g_doc->getPage(g_page);
    if (!pg) return;
    int pw,ph; pageSizePx(pw,ph);
    HDC hdc=GetDC(g_hwnd);
    ensureBmp(hdc,pw,ph);
    pdf_render_page(g_bmpDC,*g_doc,pg,0,0,pw,ph,g_zoom);
    ReleaseDC(g_hwnd,hdc);
    InvalidateRect(g_hwnd,nullptr,FALSE);
}

static void zoomFitWidth();  // forward decl

/* ================================================================
   File open
   ================================================================ */
static void openFile(const char* path) {
    auto doc=PdfDoc::open(path);
    if (!doc) {
        MessageBoxA(g_hwnd,"Could not open PDF.\nFile may be corrupt, encrypted, or unsupported.",
                    "Error",MB_ICONERROR|MB_OK);
        return;
    }
    g_doc     = std::move(doc);
    g_page    = 0;
    g_scrollY = 0;
    g_zoom    = 1.0;

    const char* base=strrchr(path,'\\');
    if (!base) base=strrchr(path,'/');
    if (base) base++; else base=path;
    char t[512];
    snprintf(t,sizeof(t),"PDF Viewer — %s (%d pages)",base,g_doc->pageCount());
    SetWindowTextA(g_hwnd,t);

    zoomFitWidth();
}

static void openFileDialog() {
    char path[MAX_PATH]={};
    OPENFILENAMEA ofn={};
    ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=g_hwnd;
    ofn.lpstrFilter="PDF Files\0*.pdf\0All Files\0*.*\0";
    ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    ofn.lpstrTitle="Open PDF";
    if (GetOpenFileNameA(&ofn)) openFile(path);
}

/* ================================================================
   Zoom fit width
   ================================================================ */
static void zoomFitWidth() {
    if (!g_doc) return;
    RECT rc; GetClientRect(g_hwnd,&rc);
    ObjPtr pg=g_doc->getPage(g_page);
    double x0,y0,x1,y1;
    if (pg) g_doc->pageBox(pg,x0,y0,x1,y1);
    else   { x0=0;y0=0;x1=612;y1=792; }
    double pw=x1-x0;
    if (pw<1) return;
    int cw=rc.right-rc.left-20;
    if (cw<10) cw=10;
    g_zoom=(double)cw/pw;
    g_scrollY=0;
    renderPage();
    updateStatus();
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        HMENU menu=CreateMenu(), file=CreatePopupMenu(), view=CreatePopupMenu();
        AppendMenuA(file,MF_STRING,IDM_OPEN,"&Open...\tCtrl+O");
        AppendMenuA(file,MF_SEPARATOR,0,nullptr);
        AppendMenuA(file,MF_STRING,IDCANCEL,"E&xit");
        AppendMenuA(view,MF_STRING,IDM_PREV,"&Previous page\tLeft");
        AppendMenuA(view,MF_STRING,IDM_NEXT,"&Next page\tRight");
        AppendMenuA(view,MF_SEPARATOR,0,nullptr);
        AppendMenuA(view,MF_STRING,IDM_ZOOM_IN,"Zoom &In\t+");
        AppendMenuA(view,MF_STRING,IDM_ZOOM_OUT,"Zoom &Out\t-");
        AppendMenuA(view,MF_STRING,IDM_FIT,"&Fit Width\tF");
        AppendMenuA(menu,MF_POPUP,UINT_PTR(file),"&File");
        AppendMenuA(menu,MF_POPUP,UINT_PTR(view),"&View");
        SetMenu(hwnd,menu);
        DragAcceptFiles(hwnd,TRUE);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc=BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);
        int cw=rc.right, ch=rc.bottom;
        HBRUSH bg=CreateSolidBrush(RGB(60,60,60));
        FillRect(hdc,&rc,bg); DeleteObject(bg);
        if (g_bmp&&g_bmpDC) {
            int pw=g_bmpW,ph=g_bmpH;
            int ox=(cw-pw)/2; if(ox<0)ox=0;
            int oy=-g_scrollY;
            RECT sr={ox+3,oy+3,ox+pw+3,oy+ph+3};
            HBRUSH sh=CreateSolidBrush(RGB(20,20,20));
            FillRect(hdc,&sr,sh); DeleteObject(sh);
            BitBlt(hdc,ox,oy,pw,ph,g_bmpDC,0,0,SRCCOPY);
        } else if (!g_doc) {
            SetBkMode(hdc,TRANSPARENT);
            SetTextColor(hdc,RGB(180,180,180));
            HFONT hf=CreateFontA(-24,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
            HFONT old=HFONT(SelectObject(hdc,hf));
            RECT tr={0,0,cw,ch};
            DrawTextA(hdc,"Open a PDF:  File > Open  or  drag && drop",-1,&tr,
                      DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            SelectObject(hdc,old); DeleteObject(hf);
        }
        EndPaint(hwnd,&ps);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN:   openFileDialog(); break;
        case IDCANCEL:   PostQuitMessage(0); break;
        case IDM_PREV:
            if (g_doc&&g_page>0){g_page--;g_scrollY=0;renderPage();updateStatus();}
            break;
        case IDM_NEXT:
            if (g_doc&&g_page<g_doc->pageCount()-1){g_page++;g_scrollY=0;renderPage();updateStatus();}
            break;
        case IDM_ZOOM_IN:  g_zoom*=1.25;if(g_zoom>8.0)g_zoom=8.0;renderPage();updateStatus();break;
        case IDM_ZOOM_OUT: g_zoom/=1.25;if(g_zoom<0.1)g_zoom=0.1;renderPage();updateStatus();break;
        case IDM_FIT:      zoomFitWidth(); break;
        }
        break;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: case VK_PRIOR:
            if(g_doc&&g_page>0){g_page--;g_scrollY=0;renderPage();updateStatus();} break;
        case VK_RIGHT: case VK_NEXT:
            if(g_doc&&g_page<g_doc->pageCount()-1){g_page++;g_scrollY=0;renderPage();updateStatus();} break;
        case VK_UP:
            g_scrollY-=80; if(g_scrollY<0)g_scrollY=0;
            InvalidateRect(hwnd,nullptr,FALSE); break;
        case VK_DOWN: {
            g_scrollY+=80;
            RECT rc2; GetClientRect(hwnd,&rc2);
            int mx=g_bmpH-(rc2.bottom-rc2.top)+10; if(mx<0)mx=0;
            if(g_scrollY>mx)g_scrollY=mx;
            InvalidateRect(hwnd,nullptr,FALSE); break;
        }
        case VK_HOME:
            if(g_doc){g_page=0;g_scrollY=0;renderPage();updateStatus();} break;
        case VK_END:
            if(g_doc){g_page=g_doc->pageCount()-1;g_scrollY=0;renderPage();updateStatus();} break;
        case 'O':
            if(GetKeyState(VK_CONTROL)&0x8000) openFileDialog();
            break;
        case VK_OEM_PLUS: case VK_ADD:
            g_zoom*=1.25;if(g_zoom>8.0)g_zoom=8.0;renderPage();updateStatus();break;
        case VK_OEM_MINUS: case VK_SUBTRACT:
            g_zoom/=1.25;if(g_zoom<0.1)g_zoom=0.1;renderPage();updateStatus();break;
        case 'F': zoomFitWidth(); break;
        }
        break;

    case WM_MOUSEWHEEL: {
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_CONTROL)&0x8000) {
            if(delta>0){g_zoom*=1.15;if(g_zoom>8.0)g_zoom=8.0;}
            else{g_zoom/=1.15;if(g_zoom<0.1)g_zoom=0.1;}
            renderPage(); updateStatus();
        } else {
            g_scrollY-=delta/3; if(g_scrollY<0)g_scrollY=0;
            RECT rc2; GetClientRect(hwnd,&rc2);
            int mx=g_bmpH-(rc2.bottom-rc2.top)+10; if(mx<0)mx=0;
            if(g_scrollY>mx)g_scrollY=mx;
            InvalidateRect(hwnd,nullptr,FALSE);
        }
        break;
    }

    case WM_SIZE:
        if (g_doc) zoomFitWidth();
        break;

    case WM_DROPFILES: {
        HDROP hd=HDROP(wp);
        char dropped[MAX_PATH]={};
        DragQueryFileA(hd,0,dropped,MAX_PATH);
        DragFinish(hd);
        openFile(dropped);
        break;
    }

    case WM_DESTROY:
        destroyBmp();
        g_doc.reset();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd,msg,wp,lp);
    }
    return 0;
}

/* ================================================================
   WinMain
   ================================================================ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int nShow) {
    WNDCLASSEXA wc={};
    wc.cbSize=sizeof(wc);
    wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=WndProc;
    wc.hInstance=hInst;
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=HBRUSH(COLOR_WINDOW+1);
    wc.lpszClassName="PDFViewerWnd";
    wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
    RegisterClassExA(&wc);

    g_hwnd=CreateWindowExA(WS_EX_ACCEPTFILES,"PDFViewerWnd","PDF Viewer",
                            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,
                            900,700,nullptr,nullptr,hInst,nullptr);
    if (!g_hwnd) return 1;
    ShowWindow(g_hwnd,nShow);
    UpdateWindow(g_hwnd);

    if (cmdLine&&cmdLine[0]) {
        char path[MAX_PATH];
        strncpy(path,cmdLine,MAX_PATH-1); path[MAX_PATH-1]='\0';
        int l=int(strlen(path));
        if (l>=2&&path[0]=='"'&&path[l-1]=='"'){memmove(path,path+1,l-2);path[l-2]='\0';}
        openFile(path);
    }

    MSG msg;
    while (GetMessageA(&msg,nullptr,0,0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return int(msg.wParam);
}
