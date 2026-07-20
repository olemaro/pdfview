#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_map>
#include "pdf.hpp"
#include "render.hpp"

static void dbgLog(const char* fmt, ...) {
    FILE* f = fopen("c:\\tmp\\pdfview\\font_debug.txt", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

static constexpr double PI = 3.14159265358979323846;

/* ================================================================
   Matrix (PDF affine: [a b c d e f])
   ================================================================ */
struct Mat {
    double a=1,b=0,c=0,d=1,e=0,f=0;
};
static Mat matMul(const Mat& a, const Mat& b) {
    Mat r;
    r.a=a.a*b.a+a.b*b.c; r.b=a.a*b.b+a.b*b.d;
    r.c=a.c*b.a+a.d*b.c; r.d=a.c*b.b+a.d*b.d;
    r.e=a.e*b.a+a.f*b.c+b.e; r.f=a.e*b.b+a.f*b.d+b.f;
    return r;
}
static void matPt(const Mat& m, double x, double y, double& ox, double& oy) {
    ox=m.a*x+m.c*y+m.e; oy=m.b*x+m.d*y+m.f;
}

/* ================================================================
   Font info: resolved from page /Resources/Font
   ================================================================ */
struct FontInfo {
    std::string baseName;   // /BaseFont value
    std::string winFace;    // mapped Windows face
    bool bold   = false;
    bool italic = false;
    int  firstChar = 0;
    int  lastChar  = 255;
    std::vector<double> widths;            // 1/1000 units per character code
    std::unordered_map<int,int> toUnicode; // code -> unicode codepoint
};

/* ================================================================
   Graphics state
   ================================================================ */
struct GState {
    Mat      ctm;
    double   lineWidth  = 1.0;
    COLORREF fillColor  = RGB(0,0,0);
    COLORREF strokeColor= RGB(0,0,0);
    double   fontSize   = 12.0;
    std::string fontKey;   // resource key, e.g. "F1"
    double   charSp     = 0.0;
    double   wordSp     = 0.0;
    double   horizScale = 100.0;
    double   leading    = 0.0;
    double   textRise   = 0.0;
};

/* Standard 14 font mapping */
struct FontMapEntry { const char* pdf; const char* win; bool bold; bool italic; };
static const FontMapEntry kFontMap[] = {
    {"Helvetica",             "Arial",           false,false},
    {"Helvetica-Bold",        "Arial",           true, false},
    {"Helvetica-Oblique",     "Arial",           false,true },
    {"Helvetica-BoldOblique", "Arial",           true, true },
    {"Times-Roman",           "Times New Roman", false,false},
    {"Times-Bold",            "Times New Roman", true, false},
    {"Times-Italic",          "Times New Roman", false,true },
    {"Times-BoldItalic",      "Times New Roman", true, true },
    {"Courier",               "Courier New",     false,false},
    {"Courier-Bold",          "Courier New",     true, false},
    {"Courier-Oblique",       "Courier New",     false,true },
    {"Courier-BoldOblique",   "Courier New",     true, true },
    {"Symbol",                "Symbol",          false,false},
    {"ZapfDingbats",          "Wingdings",       false,false},
    {nullptr,nullptr,false,false}
};

static void mapFontName(const std::string& pdf, std::string& face, bool& bold, bool& italic) {
    // strip subset prefix ABCDEF+
    const char* base = pdf.c_str();
    const char* p = strrchr(base, '+');
    if (p) base = p+1;
    for (int i=0; kFontMap[i].pdf; i++) {
#ifdef _WIN32
        if (_stricmp(base, kFontMap[i].pdf)==0) {
#else
        if (strcasecmp(base, kFontMap[i].pdf)==0) {
#endif
            face  = kFontMap[i].win;
            bold  = kFontMap[i].bold;
            italic= kFontMap[i].italic;
            return;
        }
    }
    face = "Arial";
    bold   = strstr(base,"Bold")  ||strstr(base,"bold");
    italic = strstr(base,"Italic")||strstr(base,"italic")||
             strstr(base,"Oblique")||strstr(base,"oblique");
}

/* Parse a ToUnicode CMap stream to build code->unicode table */
static std::unordered_map<int,int> parseToUnicode(const std::vector<uint8_t>& data) {
    std::unordered_map<int,int> tbl;
    const char* s = reinterpret_cast<const char*>(data.data());
    int n = int(data.size());
    int i = 0;
    auto skipWS = [&]() { while (i<n && (s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n')) i++; };
    auto readHex = [&]() -> int {
        skipWS();
        if (i>=n||s[i]!='<') return -1;
        i++;
        int val=0;
        while (i<n&&s[i]!='>') {
            int c=s[i++];
            if (c>='0'&&c<='9') val=(val<<4)|(c-'0');
            else if (c>='a'&&c<='f') val=(val<<4)|(c-'a'+10);
            else if (c>='A'&&c<='F') val=(val<<4)|(c-'A'+10);
        }
        if (i<n) i++; // '>'
        return val;
    };

    while (i < n) {
        // find "beginbfchar" or "beginbfrange"
        if (i+11<n && memcmp(s+i,"beginbfchar",11)==0) {
            i+=11;
            for (;;) {
                skipWS();
                if (i+9<n && memcmp(s+i,"endbfchar",9)==0) { i+=9; break; }
                int code=readHex(); int uni=readHex();
                if (code<0||uni<0) { i++; continue; }
                tbl[code]=uni;
            }
        } else if (i+12<n && memcmp(s+i,"beginbfrange",12)==0) {
            i+=12;
            for (;;) {
                skipWS();
                if (i+10<n && memcmp(s+i,"endbfrange",10)==0) { i+=10; break; }
                int lo=readHex(); int hi=readHex();
                if (lo<0||hi<0) { i++; continue; }
                skipWS();
                if (i<n&&s[i]=='[') {
                    i++;
                    for (int c=lo; c<=hi; c++) {
                        skipWS();
                        if (i<n&&s[i]==']') break;
                        int u=readHex();
                        if (u>=0) tbl[c]=u;
                    }
                    skipWS();
                    if (i<n&&s[i]==']') i++;
                } else {
                    int base=readHex();
                    if (base>=0) for (int c=lo; c<=hi; c++) tbl[c]=base+(c-lo);
                }
            }
        } else {
            i++;
        }
    }
    return tbl;
}

/* Resolve FontInfo from page resources for a given font key */
static FontInfo resolveFontInfo(PdfDoc& doc, ObjPtr page, const std::string& key) {
    FontInfo fi;
    fi.winFace = "Arial";

    ObjPtr res  = doc.dictGet(page, "Resources");
    ObjPtr fonts= res ? doc.dictGet(res, "Font") : nullptr;
    if (!fonts) return fi;
    ObjPtr fobj = doc.dictGet(fonts, key);
    if (!fobj)  return fi;

    // BaseFont
    ObjPtr bf = doc.dictGet(fobj, "BaseFont");
    if (bf && bf->type==PdfType::Name) {
        fi.baseName = bf->str;
        mapFontName(fi.baseName, fi.winFace, fi.bold, fi.italic);
    }

    // /FirstChar /LastChar /Widths
    ObjPtr fc = doc.dictGet(fobj, "FirstChar");
    ObjPtr lc = doc.dictGet(fobj, "LastChar");
    ObjPtr wa = doc.dictGet(fobj, "Widths");
    if (fc) fi.firstChar = int(fc->numVal());
    if (lc) fi.lastChar  = int(lc->numVal());
    if (wa && wa->type==PdfType::Array) {
        fi.widths.resize(wa->arr.size());
        for (int j=0; j<int(wa->arr.size()); j++) {
            ObjPtr w=doc.resolve(wa->arr[j]);
            fi.widths[j] = w ? w->numVal() : 0.0;
        }
    }

    // ToUnicode CMap
    ObjPtr tou = doc.dictGet(fobj, "ToUnicode");
    if (tou && tou->type==PdfType::Stream) {
        auto data = doc.streamData(tou);
        if (!data.empty()) fi.toUnicode = parseToUnicode(data);
    }

    // Embedded subsets (name has '+') with monospace substitution: prefer Arial.
    // Most modern embedded fonts are proportional; Courier-labeled subsets are usually
    // a naming quirk, not a genuine monospace font.
    if (fi.baseName.find('+') != std::string::npos && fi.winFace == "Courier New") {
        fi.winFace = "Arial";
    }

    dbgLog("Font key='%s' base='%s' win='%s' widths=%d toUnicode=%d\n",
           key.c_str(), fi.baseName.c_str(), fi.winFace.c_str(),
           (int)fi.widths.size(), (int)fi.toUnicode.size());

    return fi;
}

/* ================================================================
   Render context
   ================================================================ */
struct RCtx {
    PdfDoc& doc;
    ObjPtr  page;
    HDC     hdc;
    double  zoom;
    double  pgX0,pgY0,pgX1,pgY1;

    GState  gs;
    GState  gsStack[32];
    int     gsTop = 0;

    Mat     tm, tlm;
    bool    inText = false;

    std::vector<POINT> pathPts;
    std::vector<int>   pathVerbs; // 0=move 1=line 2=close
    double  curX=0,curY=0,subX=0,subY=0;

    // font cache: key string -> FontInfo
    std::unordered_map<std::string,FontInfo> fontCache;

    RCtx(PdfDoc& d, ObjPtr pg, HDC dc, double z,
         double x0,double y0,double x1,double y1)
        : doc(d),page(pg),hdc(dc),zoom(z)
        , pgX0(x0),pgY0(y0),pgX1(x1),pgY1(y1) {}

    int px(double x) { return int((x-pgX0)*zoom+0.5); }
    int py(double y) { double ph=pgY1-pgY0; return int((ph-(y-pgY0))*zoom+0.5); }

    FontInfo& getFont(const std::string& key) {
        auto it=fontCache.find(key);
        if (it!=fontCache.end()) return it->second;
        fontCache[key]=resolveFontInfo(doc,page,key);
        return fontCache[key];
    }
};

/* ================================================================
   Path
   ================================================================ */
static void pathAdd(RCtx& r, double x, double y, int verb) {
    POINT pt{ r.px(x), r.py(y) };
    r.pathPts.push_back(pt);
    r.pathVerbs.push_back(verb);
}
static void pathClear(RCtx& r) { r.pathPts.clear(); r.pathVerbs.clear(); }

static void gdiDrawPath(RCtx& r, bool doFill, bool doStroke) {
    if (r.pathPts.empty()) return;
    HBRUSH hbr=nullptr; HPEN hpn=nullptr;
    if (doFill) { hbr=CreateSolidBrush(r.gs.fillColor); SelectObject(r.hdc,hbr); }
    else SelectObject(r.hdc,GetStockObject(NULL_BRUSH));
    if (doStroke) {
        int lw=std::max(1,int(r.gs.lineWidth*r.zoom));
        hpn=CreatePen(PS_SOLID,lw,r.gs.strokeColor);
        SelectObject(r.hdc,hpn);
    } else SelectObject(r.hdc,GetStockObject(NULL_PEN));

    BeginPath(r.hdc);
    for (int i=0; i<int(r.pathPts.size()); i++) {
        int v=r.pathVerbs[i];
        if (v==0) MoveToEx(r.hdc,r.pathPts[i].x,r.pathPts[i].y,nullptr);
        else if(v==1) LineTo(r.hdc,r.pathPts[i].x,r.pathPts[i].y);
        else CloseFigure(r.hdc);
    }
    EndPath(r.hdc);
    if (doFill&&doStroke) StrokeAndFillPath(r.hdc);
    else if (doFill)      FillPath(r.hdc);
    else if (doStroke)    StrokePath(r.hdc);
    if (hbr) DeleteObject(hbr);
    if (hpn) DeleteObject(hpn);
    pathClear(r);
}

/* ================================================================
   Font creation
   ================================================================ */
static HFONT makeFont(const FontInfo& fi, double sizePts, double zoom,
                      double scaleY, double horizScale, int escapement=0) {
    int height = int(sizePts * zoom * scaleY + 0.5);
    if (height<1) height=1;
    if (height>2000) height=2000;
    int width = (horizScale!=1.0&&horizScale>0.01) ? int(height*horizScale+0.5) : 0;
    return CreateFontA(-height, width, escapement, 0,
        fi.bold?FW_BOLD:FW_NORMAL, fi.italic, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE,
        fi.winFace.c_str());
}

/* ================================================================
   Show string with proper glyph advance from /Widths
   ================================================================ */
static void showString(RCtx& r, const uint8_t* bytes, int len) {
    if (len<=0) return;

    // Trm = Tm * CTM
    Mat trm = matMul(r.tm, r.gs.ctm);

    // Scale factors from Trm
    double scaleY = sqrt(trm.c*trm.c+trm.d*trm.d);
    double scaleX = sqrt(trm.a*trm.a+trm.b*trm.b);
    if (scaleY<0.01) scaleY=scaleX;
    if (scaleX<0.01) scaleX=scaleY;

    // Get font info (cached)
    FontInfo& fi = r.getFont(r.gs.fontKey);

    // Decode bytes → wide chars for display
    bool isUtf16be = (len>=2 && bytes[0]==0xFE && bytes[1]==0xFF);
    std::wstring wtext;
    std::vector<int> codes; // parallel: character codes for width lookup

    if (isUtf16be) {
        for (int j=2; j+1<len; j+=2) {
            int cp = (bytes[j]<<8)|bytes[j+1];
            wtext += wchar_t(cp);
            // for UTF-16BE, code is a 2-byte value; try toUnicode by cp directly
            codes.push_back(cp);
        }
    } else {
        for (int j=0; j<len; j++) {
            int code = bytes[j];
            codes.push_back(code);
            // Map code -> unicode via ToUnicode CMap
            auto it = fi.toUnicode.find(code);
            if (it!=fi.toUnicode.end()) {
                int cp = it->second;
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    wtext += wchar_t(0xD800 + (cp>>10));
                    wtext += wchar_t(0xDC00 + (cp&0x3FF));
                } else {
                    wtext += wchar_t(cp);
                }
            } else {
                wtext += wchar_t(code); // fallback: assume Latin-1 / cp1252
            }
        }
    }
    if (wtext.empty()) return;

    // Screen position from Trm (with text rise)
    double sx, sy;
    matPt(trm, 0, r.gs.textRise, sx, sy);

    double angle_deg = atan2(trm.b, trm.a) * 180.0 / PI;
    double horizScale = r.gs.horizScale / 100.0;

    // Create and select font (keep selected while we measure advance)
    bool rotated = fabs(angle_deg) > 0.5;
    HFONT hf = nullptr;
    if (rotated) {
        int esc = int(angle_deg*10.0+0.5);
        hf = CreateFontA(-int(r.gs.fontSize*scaleY*r.zoom+0.5), 0, esc, 0,
                fi.bold?FW_BOLD:FW_NORMAL, fi.italic, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, fi.winFace.c_str());
    } else {
        hf = makeFont(fi, r.gs.fontSize, r.zoom, scaleY, horizScale);
    }
    HFONT old = HFONT(SelectObject(r.hdc, hf));
    SetBkMode(r.hdc, TRANSPARENT);
    SetTextColor(r.hdc, r.gs.fillColor);
    SetTextAlign(r.hdc, TA_BASELINE|TA_LEFT);
    TextOutW(r.hdc, r.px(sx), r.py(sy), wtext.c_str(), int(wtext.size()));

    // ---- Text advance ----
    // For embedded subset fonts (baseName contains '+') we substitute a different
    // font, so PDF /Widths don't match the rendered glyphs → use GDI measurement.
    // For referenced standard fonts the widths match our substitutes closely enough.
    bool isEmbedded = fi.baseName.find('+') != std::string::npos;
    bool useWidths  = !fi.widths.empty() && !isEmbedded;

    double advance = 0.0;
    if (useWidths) {
        for (int j=0; j<int(codes.size()); j++) {
            int code = codes[j];
            double gw = 1000.0;
            int idx = code - fi.firstChar;
            if (idx>=0 && idx<int(fi.widths.size()))
                gw = fi.widths[idx];
            double adv = (gw/1000.0) * r.gs.fontSize;
            adv += r.gs.charSp;
            if (code==32) adv += r.gs.wordSp;
            advance += adv;
        }
        advance *= horizScale;
    } else {
        // GDI measurement: matches what was actually rendered (font still selected).
        // We intentionally skip charSp here: Tc was calibrated for the original
        // embedded font's metrics; adding it on top of a substitute font's GDI
        // advance produces double-spacing artefacts (visible as "I n v o i c e").
        // Word spacing (Tw) is still applied: it drives text-justification and
        // column layout independently of glyph widths.
        SIZE sz{};
        GetTextExtentPoint32W(r.hdc, wtext.c_str(), int(wtext.size()), &sz);
        advance = double(sz.cx) / r.zoom;
        for (int code : codes) if (code==32) advance += r.gs.wordSp;
    }

    SelectObject(r.hdc, old);
    DeleteObject(hf);

    // Apply advance along text direction (handles rotation and Tm scaling)
    double tm_sx = sqrt(r.tm.a*r.tm.a + r.tm.b*r.tm.b);
    if (tm_sx > 1e-9) {
        r.tm.e += advance * r.tm.a / tm_sx;
        r.tm.f += advance * r.tm.b / tm_sx;
    } else {
        r.tm.e += advance;
    }
}

/* ================================================================
   Operator stack
   ================================================================ */
struct OpStack {
    double nums[64];
    int    n_nums = 0;
    struct Str { std::vector<uint8_t> data; };
    std::vector<Str> strs;

    void reset() { n_nums=0; strs.clear(); }
    void pushNum(double v) { if(n_nums<64) nums[n_nums++]=v; }
    void pushStr(const uint8_t* p, int l) { strs.push_back({{p,p+l}}); }
    void pushStr(const std::string& s)     { strs.push_back({{(const uint8_t*)s.data(),(const uint8_t*)s.data()+s.size()}}); }
    double get(int fromTop) const { int i=n_nums-1-fromTop; return i>=0?nums[i]:0.0; }
    int    n() const { return n_nums; }
};

/* ================================================================
   Execute one graphics operator
   ================================================================ */
static void execOp(RCtx& r, OpStack& os, const std::string& op) {
    int n=os.n();
    auto num=[&](int ft){ return os.get(ft); };

    // — graphics state —
    if (op=="q") {
        if (r.gsTop<32) r.gsStack[r.gsTop++]=r.gs;
    } else if (op=="Q") {
        if (r.gsTop>0) r.gs=r.gsStack[--r.gsTop];
    } else if (op=="cm" && n>=6) {
        Mat m; m.a=num(5);m.b=num(4);m.c=num(3);m.d=num(2);m.e=num(1);m.f=num(0);
        r.gs.ctm=matMul(m,r.gs.ctm);
    } else if (op=="w"&&n>=1) { r.gs.lineWidth=num(0); }
    else if (op=="gs") { /* ext GState: ignore */ }

    // — color —
    else if (op=="rg"&&n>=3) { r.gs.fillColor  =RGB(int(num(2)*255),int(num(1)*255),int(num(0)*255)); }
    else if (op=="RG"&&n>=3) { r.gs.strokeColor=RGB(int(num(2)*255),int(num(1)*255),int(num(0)*255)); }
    else if (op=="g" &&n>=1) { int v=int(num(0)*255); r.gs.fillColor  =RGB(v,v,v); }
    else if (op=="G" &&n>=1) { int v=int(num(0)*255); r.gs.strokeColor=RGB(v,v,v); }
    else if (op=="k" &&n>=4) {
        double C=num(3),M=num(2),Y=num(1),K=num(0);
        r.gs.fillColor=RGB(int((1-C)*(1-K)*255),int((1-M)*(1-K)*255),int((1-Y)*(1-K)*255));
    } else if (op=="K"&&n>=4) {
        double C=num(3),M=num(2),Y=num(1),K=num(0);
        r.gs.strokeColor=RGB(int((1-C)*(1-K)*255),int((1-M)*(1-K)*255),int((1-Y)*(1-K)*255));
    } else if ((op=="sc"||op=="scn")&&n>=1) {
        if (n==1){int v=int(num(0)*255);r.gs.fillColor=RGB(v,v,v);}
        else if(n>=3) r.gs.fillColor=RGB(int(num(2)*255),int(num(1)*255),int(num(0)*255));
    } else if ((op=="SC"||op=="SCN")&&n>=1) {
        if (n==1){int v=int(num(0)*255);r.gs.strokeColor=RGB(v,v,v);}
        else if(n>=3) r.gs.strokeColor=RGB(int(num(2)*255),int(num(1)*255),int(num(0)*255));
    } else if (op=="cs"||op=="CS") { /* color space: ignore */ }

    // — path construction —
    else if (op=="m"&&n>=2) {
        double sx,sy; matPt(r.gs.ctm,num(1),num(0),sx,sy);
        pathAdd(r,sx,sy,0); r.curX=sx;r.curY=sy;r.subX=sx;r.subY=sy;
    } else if (op=="l"&&n>=2) {
        double sx,sy; matPt(r.gs.ctm,num(1),num(0),sx,sy);
        pathAdd(r,sx,sy,1); r.curX=sx;r.curY=sy;
    } else if (op=="c"&&n>=6) {
        double x0=r.curX,y0=r.curY, x1,y1,x2,y2,x3,y3;
        matPt(r.gs.ctm,num(5),num(4),x1,y1);
        matPt(r.gs.ctm,num(3),num(2),x2,y2);
        matPt(r.gs.ctm,num(1),num(0),x3,y3);
        for (int s=1;s<=4;s++){double t=s/4.0,mt=1-t;
            pathAdd(r,mt*mt*mt*x0+3*mt*mt*t*x1+3*mt*t*t*x2+t*t*t*x3,
                      mt*mt*mt*y0+3*mt*mt*t*y1+3*mt*t*t*y2+t*t*t*y3,1);}
        r.curX=x3;r.curY=y3;
    } else if (op=="v"&&n>=4) {
        double x0=r.curX,y0=r.curY,x2,y2,x3,y3;
        matPt(r.gs.ctm,num(3),num(2),x2,y2);
        matPt(r.gs.ctm,num(1),num(0),x3,y3);
        for (int s=1;s<=4;s++){double t=s/4.0,mt=1-t;
            pathAdd(r,mt*mt*mt*x0+3*mt*mt*t*x0+3*mt*t*t*x2+t*t*t*x3,
                      mt*mt*mt*y0+3*mt*mt*t*y0+3*mt*t*t*y2+t*t*t*y3,1);}
        r.curX=x3;r.curY=y3;
    } else if (op=="y"&&n>=4) {
        double x0=r.curX,y0=r.curY,x1,y1,x3,y3;
        matPt(r.gs.ctm,num(3),num(2),x1,y1);
        matPt(r.gs.ctm,num(1),num(0),x3,y3);
        for (int s=1;s<=4;s++){double t=s/4.0,mt=1-t;
            pathAdd(r,mt*mt*mt*x0+3*mt*mt*t*x1+3*mt*t*t*x3+t*t*t*x3,
                      mt*mt*mt*y0+3*mt*mt*t*y1+3*mt*t*t*y3+t*t*t*y3,1);}
        r.curX=x3;r.curY=y3;
    } else if (op=="h") {
        pathAdd(r,r.subX,r.subY,2); r.curX=r.subX;r.curY=r.subY;
    } else if (op=="re"&&n>=4) {
        double rx=num(3),ry=num(2),rw=num(1),rh=num(0);
        double p0x,p0y,p1x,p1y,p2x,p2y,p3x,p3y;
        matPt(r.gs.ctm,rx,ry,p0x,p0y);      matPt(r.gs.ctm,rx+rw,ry,p1x,p1y);
        matPt(r.gs.ctm,rx+rw,ry+rh,p2x,p2y);matPt(r.gs.ctm,rx,ry+rh,p3x,p3y);
        pathAdd(r,p0x,p0y,0);pathAdd(r,p1x,p1y,1);
        pathAdd(r,p2x,p2y,1);pathAdd(r,p3x,p3y,1);pathAdd(r,p0x,p0y,2);
        r.subX=p0x;r.subY=p0y;r.curX=p0x;r.curY=p0y;
    }

    // — path painting —
    else if (op=="S")          gdiDrawPath(r,false,true);
    else if (op=="s")  { pathAdd(r,r.subX,r.subY,2); gdiDrawPath(r,false,true); }
    else if (op=="f"||op=="F"||op=="f*") gdiDrawPath(r,true,false);
    else if (op=="B"||op=="B*") gdiDrawPath(r,true,true);
    else if (op=="b"||op=="b*") { pathAdd(r,r.subX,r.subY,2); gdiDrawPath(r,true,true); }
    else if (op=="n"||op=="W"||op=="W*") pathClear(r);

    // — text state —
    else if (op=="BT") {
        r.inText=true; r.tm={}; r.tlm={};
    } else if (op=="ET") { r.inText=false; }
    else if (op=="Tf"&&n>=1&&!os.strs.empty()) {
        r.gs.fontSize = num(0);
        auto& raw=os.strs.back().data;
        r.gs.fontKey.assign(raw.begin(),raw.end());
    } else if (op=="Tc"&&n>=1) { r.gs.charSp=num(0); dbgLog("Tc=%f\n",num(0)); }
    else if (op=="Tw"&&n>=1) { r.gs.wordSp=num(0); }
    else if (op=="Tz"&&n>=1) { r.gs.horizScale=num(0); }
    else if (op=="TL"&&n>=1) { r.gs.leading=num(0); }
    else if (op=="Ts"&&n>=1) { r.gs.textRise=num(0); }

    // — text positioning —
    else if (op=="Td"&&n>=2) {
        Mat t; t.e=num(1);t.f=num(0); r.tlm=matMul(t,r.tlm); r.tm=r.tlm;
    } else if (op=="TD"&&n>=2) {
        r.gs.leading=-num(0);
        Mat t; t.e=num(1);t.f=num(0); r.tlm=matMul(t,r.tlm); r.tm=r.tlm;
    } else if (op=="Tm"&&n>=6) {
        r.tm.a=num(5);r.tm.b=num(4);r.tm.c=num(3);r.tm.d=num(2);r.tm.e=num(1);r.tm.f=num(0);
        r.tlm=r.tm;
    } else if (op=="T*") {
        Mat t; t.f=-r.gs.leading; r.tlm=matMul(t,r.tlm); r.tm=r.tlm;
    }

    // — text showing —
    else if ((op=="Tj"||op=="'")&&!os.strs.empty()) {
        if (op=="'") { Mat t;t.f=-r.gs.leading; r.tlm=matMul(t,r.tlm); r.tm=r.tlm; }
        auto& s=os.strs.back();
        showString(r,s.data.data(),int(s.data.size()));
    } else if (op=="\""&&n>=2&&!os.strs.empty()) {
        r.gs.wordSp=num(1); r.gs.charSp=num(0);
        Mat t;t.f=-r.gs.leading; r.tlm=matMul(t,r.tlm); r.tm=r.tlm;
        auto& s=os.strs.back();
        showString(r,s.data.data(),int(s.data.size()));
    }
    // TJ is handled inline in execStream
}

/* ================================================================
   Content stream tokenizer / executor
   ================================================================ */
static void execStream(RCtx& r, const uint8_t* data, int len) {
    int pos=0;
    OpStack os;

    auto skipWS=[&](){ while(pos<len&&(data[pos]==' '||data[pos]=='\t'||data[pos]=='\r'||data[pos]=='\n'||data[pos]=='\f'))pos++; };
    auto hexNib=[](int c)->int{
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        return 0;
    };

    auto parseStr=[&]()->std::vector<uint8_t> {
        pos++; // '('
        std::vector<uint8_t> tmp;
        int depth=1;
        while(pos<len&&depth>0){
            int c=data[pos++];
            if(c=='\\'){
                int e=(pos<len)?data[pos++]:0;
                switch(e){case'n':c='\n';break;case'r':c='\r';break;case't':c='\t';break;
                case'b':c='\b';break;case'f':c='\f';break;case'\\':c='\\';break;
                case'(':c='(';break;case')':c=')';break;
                default:if(e>='0'&&e<='7'){int oc=e-'0';if(pos<len&&data[pos]>='0'&&data[pos]<='7')oc=oc*8+data[pos++]-'0';if(pos<len&&data[pos]>='0'&&data[pos]<='7')oc=oc*8+data[pos++]-'0';c=oc;}else c=e;}
                tmp.push_back(uint8_t(c));
            } else if(c=='('){depth++;tmp.push_back('(');}
            else if(c==')'){if(--depth>0)tmp.push_back(')');}
            else tmp.push_back(uint8_t(c));
        }
        return tmp;
    };

    auto parseHexStr=[&]()->std::vector<uint8_t> {
        pos++; // '<'
        std::vector<uint8_t> tmp;
        while(pos<len&&data[pos]!='>'){
            while(pos<len&&isspace(data[pos]))pos++;
            if(pos>=len||data[pos]=='>')break;
            int h=hexNib(data[pos++]);
            while(pos<len&&isspace(data[pos]))pos++;
            int l=0;if(pos<len&&data[pos]!='>'){l=hexNib(data[pos++]);}
            tmp.push_back(uint8_t((h<<4)|l));
        }
        if(pos<len)pos++;
        return tmp;
    };

    while (pos<len) {
        skipWS();
        if (pos>=len) break;
        int c=data[pos];

        if (c=='%') { while(pos<len&&data[pos]!='\n'&&data[pos]!='\r')pos++; continue; }

        if (c=='(') { auto s=parseStr(); os.pushStr(s.data(),int(s.size())); continue; }

        if (c=='<'&&pos+1<len&&data[pos+1]!='<') {
            auto s=parseHexStr(); os.pushStr(s.data(),int(s.size())); continue;
        }

        // Name → push as string
        if (c=='/') {
            pos++;
            std::string name;
            while(pos<len){
                int nc=data[pos];
                if(nc==' '||nc=='\t'||nc=='\r'||nc=='\n'||nc=='/'||nc=='('||nc==')'||nc=='<'||nc=='>'||nc=='['||nc==']'||nc=='%')break;
                if(nc=='#'&&pos+2<len){pos++;int h=hexNib(data[pos++]);int l=hexNib(data[pos++]);name+=char((h<<4)|l);}
                else name+=char(data[pos++]);
            }
            os.pushStr((const uint8_t*)name.data(),int(name.size()));
            continue;
        }

        // TJ array
        if (c=='[') {
            pos++;
            while(pos<len){
                skipWS();
                if(pos>=len||data[pos]==']'){if(pos<len)pos++;break;}
                int ac=data[pos];
                if(ac=='(') {
                    auto s=parseStr(); showString(r,s.data(),int(s.size()));
                } else if(ac=='<'&&pos+1<len&&data[pos+1]!='>') {
                    auto s=parseHexStr(); showString(r,s.data(),int(s.size()));
                } else if((ac>='0'&&ac<='9')||ac=='-'||ac=='+'||(ac=='.')){
                    std::string nb; while(pos<len&&((data[pos]>='0'&&data[pos]<='9')||data[pos]=='-'||data[pos]=='+'||data[pos]=='.'))nb+=char(data[pos++]);
                    double kern=atof(nb.c_str());
                    // kerning: shift Tm by -kern/1000 * fontSize in text direction
                    double adv=-kern/1000.0*r.gs.fontSize*(r.gs.horizScale/100.0);
                    double tsx=sqrt(r.tm.a*r.tm.a+r.tm.b*r.tm.b);
                    if(tsx>1e-9){r.tm.e+=adv*r.tm.a/tsx;r.tm.f+=adv*r.tm.b/tsx;}
                    else r.tm.e+=adv;
                } else pos++;
            }
            os.reset();
            continue;
        }

        // Number
        if((c>='0'&&c<='9')||c=='-'||c=='+'||(c=='.'&&pos+1<len&&data[pos+1]>='0'&&data[pos+1]<='9')){
            std::string nb;
            while(pos<len&&((data[pos]>='0'&&data[pos]<='9')||data[pos]=='-'||data[pos]=='+'||data[pos]=='.'))nb+=char(data[pos++]);
            os.pushNum(atof(nb.c_str()));
            continue;
        }

        // Skip '<<' dict (inline image params etc.)
        if(c=='<'&&pos+1<len&&data[pos+1]=='<'){
            pos+=2; int d2=1;
            while(pos<len&&d2>0){
                if(data[pos]=='<'&&pos+1<len&&data[pos+1]=='<'){d2++;pos+=2;}
                else if(data[pos]=='>'&&pos+1<len&&data[pos+1]=='>'){d2--;pos+=2;}
                else pos++;
            }
            continue;
        }

        // Operator
        std::string op;
        while(pos<len){
            int nc=data[pos];
            if(nc==' '||nc=='\t'||nc=='\r'||nc=='\n'||nc=='('||nc==')'||nc=='/'||nc=='<'||nc=='>'||nc=='['||nc==']'||nc=='%')break;
            op+=char(nc); pos++;
        }
        if(op.empty()){pos++;continue;}

        // BI/EI: skip inline image data
        if(op=="BI"){
            while(pos+2<len){
                if(data[pos]==' '&&data[pos+1]=='E'&&data[pos+2]=='I'){pos+=3;break;}
                if(data[pos]=='E'&&data[pos+1]=='I'&&(pos+2>=len||data[pos+2]==' '||data[pos+2]=='\n'||data[pos+2]=='\r')){pos+=2;break;}
                pos++;
            }
            os.reset(); continue;
        }

        execOp(r, os, op);
        os.reset();
    }
}

/* ================================================================
   Public: render page
   ================================================================ */
void pdf_render_page(HDC hdc, PdfDoc& doc, ObjPtr page,
                     int dst_x, int dst_y, int dst_w, int dst_h,
                     double zoom) {
    double x0,y0,x1,y1;
    doc.pageBox(page, x0,y0,x1,y1);

    RECT rc{dst_x,dst_y,dst_x+dst_w,dst_y+dst_h};
    HBRUSH white=CreateSolidBrush(RGB(255,255,255));
    FillRect(hdc,&rc,white); DeleteObject(white);

    RCtx r(doc, page, hdc, zoom, x0,y0,x1,y1);
    r.gs.lineWidth  = 1.0;
    r.gs.fillColor  = RGB(0,0,0);
    r.gs.strokeColor= RGB(0,0,0);
    r.gs.fontSize   = 12.0;
    r.gs.horizScale = 100.0;

    // Expand clip by 4px: rounding at the page edge can push rightmost glyphs 1-2px over
    HRGN clip=CreateRectRgn(dst_x, dst_y, dst_x+dst_w+4, dst_y+dst_h+4);
    SelectClipRgn(hdc,clip); DeleteObject(clip);

    ObjPtr contents=doc.dictGet(page,"Contents");
    if (!contents) { SelectClipRgn(hdc,nullptr); return; }

    auto renderStream=[&](ObjPtr cs){
        if (!cs||cs->type!=PdfType::Stream) return;
        auto data=doc.streamData(cs);
        if (!data.empty()) execStream(r,data.data(),int(data.size()));
    };

    if (contents->type==PdfType::Stream) {
        renderStream(contents);
    } else if (contents->type==PdfType::Array) {
        for (auto& item : contents->arr)
            renderStream(doc.resolve(item));
    }

    SelectClipRgn(hdc,nullptr);
}
