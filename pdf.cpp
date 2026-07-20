#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cassert>
#include "pdf.hpp"
#include "inflate.h"

/* ================================================================
   Scanner
   ================================================================ */
struct Scanner {
    const uint8_t* buf;
    int len, pos;

    Scanner(const uint8_t* b, int l, int p=0) : buf(b), len(l), pos(p) {}
    bool eof()      const { return pos >= len; }
    int  peek()     const { return eof() ? -1 : buf[pos]; }
    int  get()            { return eof() ? -1 : buf[pos++]; }

    void skipWS() {
        while (!eof()) {
            int c = peek();
            if (c == '%') { while (!eof() && peek()!='\n' && peek()!='\r') get(); }
            else if (c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f') get();
            else break;
        }
    }

    std::string token() {
        skipWS();
        std::string t;
        while (!eof()) {
            int c = peek();
            if (c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||
                c=='/'||c=='('||c==')'||c=='<'||c=='>'||
                c=='['||c==']'||c=='{'||c=='}'||c=='%') break;
            t += char(get());
        }
        return t;
    }

    bool match(const char* kw) {
        int n = int(strlen(kw));
        if (pos+n > len) return false;
        if (memcmp(buf+pos, kw, n) != 0) return false;
        pos += n; return true;
    }
};

/* ================================================================
   Inflate wrapper
   ================================================================ */
static std::vector<uint8_t> doInflate(const uint8_t* src, int slen) {
    int olen = 0;
    unsigned char* p = zinflate(src, slen, &olen);
    if (!p) return {};
    std::vector<uint8_t> v(p, p+olen);
    free(p);
    return v;
}

/* ================================================================
   Object parser
   ================================================================ */
static int hexVal(int c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}

static ObjPtr parseObj(Scanner& s);

static ObjPtr parseLiteralStr(Scanner& s) {
    s.get(); // '('
    std::string tmp;
    int depth = 1;
    while (!s.eof() && depth > 0) {
        int c = s.get();
        if (c == '\\') {
            int e = s.get();
            switch (e) {
            case 'n': tmp+='\n'; break; case 'r': tmp+='\r'; break;
            case 't': tmp+='\t'; break; case 'b': tmp+='\b'; break;
            case 'f': tmp+='\f'; break; case '\\':tmp+='\\';break;
            case '(': tmp+='('; break;  case ')': tmp+=')'; break;
            default:
                if (e>='0'&&e<='7') {
                    int oc=e-'0';
                    if (s.peek()>='0'&&s.peek()<='7') oc=oc*8+s.get()-'0';
                    if (s.peek()>='0'&&s.peek()<='7') oc=oc*8+s.get()-'0';
                    tmp += char(oc);
                } else tmp += char(e);
            }
        } else if (c=='(') { depth++; tmp+='('; }
        else if (c==')') { if (--depth>0) tmp+=')'; }
        else tmp += char(c);
    }
    auto o = std::make_shared<PdfObj>();
    o->type = PdfType::String;
    o->str  = std::move(tmp);
    return o;
}

static ObjPtr parseHexStr(Scanner& s) {
    s.get(); // '<'
    std::string tmp;
    while (!s.eof() && s.peek()!='>') {
        while (!s.eof() && isspace(s.peek())) s.get();
        if (s.peek()=='>') break;
        int h = hexVal(s.get());
        while (!s.eof() && isspace(s.peek())) s.get();
        int l = 0;
        if (s.peek()!='>') l = hexVal(s.get());
        tmp += char((h<<4)|l);
    }
    if (s.peek()=='>') s.get();
    auto o = std::make_shared<PdfObj>();
    o->type = PdfType::String;
    o->str  = std::move(tmp);
    return o;
}

static ObjPtr parseName(Scanner& s) {
    s.get(); // '/'
    std::string tmp;
    while (!s.eof()) {
        int c = s.peek();
        if (c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||
            c=='/'||c=='('||c==')'||c=='<'||c=='>'||
            c=='['||c==']'||c=='{'||c=='}'||c=='%') break;
        if (c=='#') {
            s.get();
            int h=hexVal(s.get()), l=hexVal(s.get());
            tmp += char((h<<4)|l);
        } else {
            tmp += char(s.get());
        }
    }
    auto o = std::make_shared<PdfObj>();
    o->type = PdfType::Name;
    o->str  = std::move(tmp);
    return o;
}

static ObjPtr parseArray(Scanner& s) {
    s.get(); // '['
    auto o = std::make_shared<PdfObj>();
    o->type = PdfType::Array;
    for (;;) {
        s.skipWS();
        if (s.eof() || s.peek()==']') { s.get(); break; }
        auto item = parseObj(s);
        if (item) o->arr.push_back(std::move(item));
    }
    return o;
}

static ObjPtr parseDictOrStream(Scanner& s) {
    s.get(); s.get(); // '<<'
    auto o = std::make_shared<PdfObj>();
    o->type = PdfType::Dict;
    for (;;) {
        s.skipWS();
        if (s.eof()) break;
        if (s.peek()=='>' && s.pos+1<s.len && s.buf[s.pos+1]=='>') {
            s.get(); s.get(); break;
        }
        if (s.peek()!='/') { s.get(); continue; }
        auto kobj = parseName(s);
        if (!kobj) break;
        s.skipWS();
        auto vobj = parseObj(s);
        if (!vobj) break;
        o->dict[kobj->str] = std::move(vobj);
    }
    // stream?
    s.skipWS();
    if (s.pos+6<=s.len && memcmp(s.buf+s.pos,"stream",6)==0) {
        s.pos += 6;
        if (s.pos<s.len && s.buf[s.pos]=='\r') s.pos++;
        if (s.pos<s.len && s.buf[s.pos]=='\n') s.pos++;
        int start = s.pos;
        // determine length
        int length = -1;
        auto it = o->dict.find("Length");
        if (it!=o->dict.end()) {
            auto& lobj = it->second;
            if (lobj->type==PdfType::Int)  length = int(lobj->ival);
            if (lobj->type==PdfType::Real) length = int(lobj->rval);
        }
        if (length < 0) {
            // scan for 'endstream'
            const uint8_t* p = s.buf+start;
            int rem = s.len-start;
            for (int j=0; j<=rem-9; j++) {
                if (memcmp(p+j,"endstream",9)==0) { length=j; break; }
            }
        }
        if (length<0 || start+length>s.len) length = s.len-start;
        o->type = PdfType::Stream;
        o->raw.assign(s.buf+start, s.buf+start+length);
        s.pos = start+length;
        s.skipWS();
        if (s.pos+9<=s.len && memcmp(s.buf+s.pos,"endstream",9)==0)
            s.pos += 9;
    }
    return o;
}

static ObjPtr parseObj(Scanner& s) {
    s.skipWS();
    if (s.eof()) return nullptr;
    int c = s.peek();
    if (c=='(') return parseLiteralStr(s);
    if (c=='/') return parseName(s);
    if (c=='[') return parseArray(s);
    if (c=='<') {
        if (s.pos+1<s.len && s.buf[s.pos+1]=='<') return parseDictOrStream(s);
        return parseHexStr(s);
    }
    if (c=='t' && s.match("true"))  { auto o=std::make_shared<PdfObj>(); o->type=PdfType::Bool; o->bval=true;  return o; }
    if (c=='f' && s.match("false")) { auto o=std::make_shared<PdfObj>(); o->type=PdfType::Bool; o->bval=false; return o; }
    if (c=='n' && s.match("null"))  { return std::make_shared<PdfObj>(); }
    if (c=='R') return nullptr;

    if (c=='+'||c=='-'||c=='.'||(c>='0'&&c<='9')) {
        int save = s.pos;
        std::string tok = s.token();
        bool isInt = true;
        for (char ch : tok) if (ch=='.'||ch=='e'||ch=='E') { isInt=false; break; }
        if (isInt) {
            // peek for "gen R"
            int cur = s.pos;
            std::string tok2 = s.token();
            std::string tok3 = s.token();
            if (tok3=="R") {
                auto o=std::make_shared<PdfObj>();
                o->type=PdfType::Ref;
                o->ref_num=atoi(tok.c_str());
                o->ref_gen=atoi(tok2.c_str());
                return o;
            }
            s.pos = cur;
            auto o=std::make_shared<PdfObj>();
            o->type=PdfType::Int;
            o->ival=atoll(tok.c_str());
            return o;
        }
        (void)save;
        auto o=std::make_shared<PdfObj>();
        o->type=PdfType::Real;
        o->rval=atof(tok.c_str());
        return o;
    }
    s.token(); // skip unknown
    return nullptr;
}

/* ================================================================
   PdfDoc internals
   ================================================================ */
void PdfDoc::ensureXRef(int num) {
    if (num >= int(xref.size())) {
        xref.resize(num+256);
        cache.resize(num+256);
    }
}

ObjPtr PdfDoc::parseObjAt(int64_t offset) {
    if (offset<0 || offset>=int64_t(buf.size())) return nullptr;
    Scanner s(buf.data(), int(buf.size()), int(offset));
    std::string t1=s.token(), t2=s.token(), t3=s.token();
    if (t3!="obj") return nullptr;
    return parseObj(s);
}

void PdfDoc::parseXRefTable(int pos, ObjPtr* trailerOut) {
    Scanner s(buf.data(), int(buf.size()), pos);
    std::string kw = s.token();
    if (kw!="xref") return;
    for (;;) {
        s.skipWS();
        int sp = s.pos;
        std::string t1 = s.token();
        if (t1=="trailer") break;
        std::string t2 = s.token();
        int first=atoi(t1.c_str()), count=atoi(t2.c_str());
        for (int i=0; i<count; i++) {
            std::string off=s.token(), gen=s.token(), flag=s.token();
            int num=first+i;
            ensureXRef(num);
            // only set if not already in-use (earlier xref takes priority)
            if (xref[num].type==0) {
                xref[num].offset = atoll(off.c_str());
                xref[num].gen    = atoi(gen.c_str());
                xref[num].type   = (flag.size()&&flag[0]=='n') ? 1 : 0;
            }
        }
        (void)sp;
    }
    s.skipWS();
    ObjPtr td = parseObj(s);
    if (trailerOut && !*trailerOut) *trailerOut = td;
}

void PdfDoc::parseXRefStream(int pos) {
    ObjPtr o = parseObjAt(pos);
    if (!o || o->type!=PdfType::Stream) return;

    // /W widths
    int w[3]={1,2,1};
    auto wi = o->dict.find("W");
    if (wi!=o->dict.end() && wi->second->type==PdfType::Array) {
        auto& wa = wi->second->arr;
        for (int j=0; j<3&&j<int(wa.size()); j++)
            w[j] = wa[j] ? int(wa[j]->numVal()) : 0;
    }
    // /Size
    int size=0;
    auto si = o->dict.find("Size");
    if (si!=o->dict.end()) size = int(si->second->numVal());

    // decompress
    std::vector<uint8_t> data;
    auto fi = o->dict.find("Filter");
    if (fi!=o->dict.end()) {
        auto& f = fi->second;
        std::string fn;
        if (f->type==PdfType::Name) fn=f->str;
        else if (f->type==PdfType::Array&&!f->arr.empty()&&f->arr[0]->type==PdfType::Name)
            fn=f->arr[0]->str;
        if (fn=="FlateDecode"||fn=="Fl")
            data = doInflate(o->raw.data(), int(o->raw.size()));
    }
    if (data.empty()) data = o->raw;

    // /Index
    std::vector<int> idx_first, idx_count;
    auto ii = o->dict.find("Index");
    if (ii!=o->dict.end() && ii->second->type==PdfType::Array) {
        auto& ia = ii->second->arr;
        for (int j=0; j+1<int(ia.size()); j+=2) {
            idx_first.push_back(int(ia[j]->numVal()));
            idx_count.push_back(int(ia[j+1]->numVal()));
        }
    } else {
        idx_first.push_back(0);
        idx_count.push_back(size);
    }

    int stride=w[0]+w[1]+w[2], dpos=0;
    for (int p=0; p<int(idx_first.size()); p++) {
        for (int k=0; k<idx_count[p]; k++) {
            if (dpos+stride>int(data.size())) break;
            int64_t f0=0,f1=0,f2=0;
            for (int j=0;j<w[0];j++) f0=(f0<<8)|data[dpos++];
            for (int j=0;j<w[1];j++) f1=(f1<<8)|data[dpos++];
            for (int j=0;j<w[2];j++) f2=(f2<<8)|data[dpos++];
            if (w[0]==0) f0=1;
            int num=idx_first[p]+k;
            ensureXRef(num);
            if (xref[num].type==0) {
                if (f0==1) { xref[num].type=1; xref[num].offset=f1; xref[num].gen=int(f2); }
                else if(f0==2) { xref[num].type=2; xref[num].offset=f1; xref[num].idx=int(f2); }
            }
        }
    }
    // save trailer
    if (!trailer) {
        trailer = std::make_shared<PdfObj>();
        trailer->type = PdfType::Dict;
        trailer->dict = o->dict;
    }
}

int64_t PdfDoc::findStartXRef() {
    int search = int(buf.size())>1024 ? int(buf.size())-1024 : 0;
    const uint8_t* p = buf.data()+search;
    int n = int(buf.size())-search;
    for (int i=n-9; i>=0; i--) {
        if (memcmp(p+i,"startxref",9)==0) {
            Scanner s(buf.data(), int(buf.size()), search+i+9);
            return atoll(s.token().c_str());
        }
    }
    return -1;
}

ObjPtr PdfDoc::resolve(ObjPtr obj) {
    int depth=0;
    while (obj && obj->type==PdfType::Ref && depth++<64) {
        int num=obj->ref_num;
        if (num<0||num>=int(xref.size())) return nullptr;
        if (cache[num]) { obj=cache[num]; continue; }
        XRefEntry& e = xref[num];
        ObjPtr parsed;
        if (e.type==1) {
            parsed = parseObjAt(e.offset);
        } else if (e.type==2) {
            int sn=int(e.offset);
            if (sn<0||sn>=int(xref.size())) return nullptr;
            if (!cache[sn]) {
                if (xref[sn].type==1)
                    cache[sn]=parseObjAt(xref[sn].offset);
            }
            ObjPtr stm=cache[sn];
            if (!stm||stm->type!=PdfType::Stream) return nullptr;
            // decompress
            std::vector<uint8_t> data;
            auto fi=stm->dict.find("Filter");
            if (fi!=stm->dict.end()) {
                std::string fn;
                if (fi->second->type==PdfType::Name) fn=fi->second->str;
                if (fn=="FlateDecode"||fn=="Fl")
                    data=doInflate(stm->raw.data(),int(stm->raw.size()));
            }
            if (data.empty()) data=stm->raw;
            // /First offset
            int first_off=0;
            auto foi=stm->dict.find("First");
            if (foi!=stm->dict.end()) first_off=int(foi->second->numVal());
            // read offset table for obj e.idx
            Scanner ts(data.data(),int(data.size()),0);
            int obj_off=0;
            for (int j=0; j<=e.idx; j++) {
                std::string n1=ts.token(), n2=ts.token();
                if (j==e.idx) obj_off=atoi(n2.c_str());
            }
            Scanner os(data.data(),int(data.size()),first_off+obj_off);
            parsed=parseObj(os);
        }
        cache[num]=parsed;
        obj=parsed;
    }
    return obj;
}

ObjPtr PdfDoc::dictGet(ObjPtr d, const std::string& key) {
    if (!d) return nullptr;
    std::unordered_map<std::string,ObjPtr>* dp = nullptr;
    if (d->type==PdfType::Dict||d->type==PdfType::Stream) dp=&d->dict;
    if (!dp) return nullptr;
    auto it=dp->find(key);
    if (it==dp->end()) return nullptr;
    return resolve(it->second);
}

std::vector<uint8_t> PdfDoc::streamData(ObjPtr obj) {
    if (!obj||obj->type!=PdfType::Stream) return {};
    // /Filter
    auto fi=obj->dict.find("Filter");
    if (fi!=obj->dict.end()) {
        std::string fn;
        auto& fv=fi->second;
        if (fv->type==PdfType::Name) fn=fv->str;
        else if (fv->type==PdfType::Array&&!fv->arr.empty()&&fv->arr[0]->type==PdfType::Name)
            fn=fv->arr[0]->str;
        if (fn=="FlateDecode"||fn=="Fl")
            return doInflate(obj->raw.data(),int(obj->raw.size()));
    }
    return obj->raw;
}

void PdfDoc::pageBox(ObjPtr page, double& x0,double& y0,double& x1,double& y1) {
    ObjPtr box=dictGet(page,"MediaBox");
    if (!box) box=dictGet(page,"CropBox");
    if (box&&box->type==PdfType::Array&&box->arr.size()>=4) {
        x0=resolve(box->arr[0])->numVal();
        y0=resolve(box->arr[1])->numVal();
        x1=resolve(box->arr[2])->numVal();
        y1=resolve(box->arr[3])->numVal();
    } else { x0=0;y0=0;x1=612;y1=792; }
}

void PdfDoc::collectPages(ObjPtr node) {
    node = resolve(node);
    if (!node) return;
    ObjPtr type=dictGet(node,"Type");
    if (type&&type->type==PdfType::Name&&type->str=="Pages") {
        ObjPtr kids=dictGet(node,"Kids");
        if (!kids||kids->type!=PdfType::Array) return;
        for (auto& kid : kids->arr) collectPages(kid);
    } else {
        pages.push_back(node);
    }
}

std::unique_ptr<PdfDoc> PdfDoc::open(const char* path) {
    FILE* f=fopen(path,"rb");
    if (!f) return nullptr;
    fseek(f,0,SEEK_END);
    long sz=ftell(f);
    fseek(f,0,SEEK_SET);
    if (sz<=0) { fclose(f); return nullptr; }
    auto doc=std::make_unique<PdfDoc>();
    doc->buf.resize(sz+1);
    fread(doc->buf.data(),1,sz,f);
    doc->buf[sz]=0;
    fclose(f);
    if (sz<8||memcmp(doc->buf.data(),"%PDF-",5)!=0) return nullptr;

    int64_t sxref=doc->findStartXRef();
    if (sxref<0) return nullptr;

    const uint8_t* p=doc->buf.data()+sxref;
    int rem=int(doc->buf.size())-int(sxref);
    if (rem>=4&&memcmp(p,"xref",4)==0)
        doc->parseXRefTable(int(sxref),&doc->trailer);
    else
        doc->parseXRefStream(int(sxref));

    // follow /Prev chains
    ObjPtr cur=doc->trailer;
    for (int chain=0; chain<16&&cur; chain++) {
        auto pi=cur->dict.find("Prev");
        if (pi==cur->dict.end()) break;
        int64_t poff=pi->second->numVal();
        const uint8_t* pp=doc->buf.data()+poff;
        int pr=int(doc->buf.size())-int(poff);
        ObjPtr old_tr;
        if (pr>=4&&memcmp(pp,"xref",4)==0)
            doc->parseXRefTable(int(poff),&old_tr);
        else
            doc->parseXRefStream(int(poff));
        cur=old_tr;
    }

    if (!doc->trailer) return nullptr;
    doc->root=doc->dictGet(doc->trailer,"Root");
    if (!doc->root) return nullptr;
    doc->pagesRoot=doc->dictGet(doc->root,"Pages");
    if (!doc->pagesRoot) return nullptr;
    doc->collectPages(doc->pagesRoot);
    return doc;
}
