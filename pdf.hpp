#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

enum class PdfType : uint8_t {
    Null, Bool, Int, Real, String, Name, Array, Dict, Stream, Ref
};

struct PdfObj;
using ObjPtr = std::shared_ptr<PdfObj>;

struct PdfObj {
    PdfType  type    = PdfType::Null;
    bool     bval    = false;
    int64_t  ival    = 0;
    double   rval    = 0.0;
    std::string str;                                    // String / Name
    std::vector<ObjPtr>                     arr;        // Array
    std::unordered_map<std::string,ObjPtr>  dict;       // Dict / Stream header
    std::vector<uint8_t>                    raw;        // Stream compressed bytes
    int ref_num = 0, ref_gen = 0;                       // Ref

    double numVal() const noexcept {
        return type==PdfType::Int ? double(ival) : type==PdfType::Real ? rval : 0.0;
    }
    bool isName(const char* n) const noexcept {
        return type==PdfType::Name && str==n;
    }
};

struct XRefEntry {
    int64_t offset = 0;
    int gen  = 0;
    int type = 0;   // 0=free 1=in-use 2=compressed
    int idx  = 0;   // index inside obj stream (type 2)
};

struct PdfDoc {
    std::vector<uint8_t>   buf;
    std::vector<XRefEntry> xref;
    std::vector<ObjPtr>    cache;   // indexed by obj number
    ObjPtr                 trailer;
    ObjPtr                 root;
    ObjPtr                 pagesRoot;
    std::vector<ObjPtr>    pages;   // flat ordered page list

    static std::unique_ptr<PdfDoc> open(const char* path);

    int    pageCount() const { return int(pages.size()); }
    ObjPtr getPage(int i)   const {
        return (i>=0 && i<int(pages.size())) ? pages[i] : nullptr;
    }
    ObjPtr resolve(ObjPtr obj);
    ObjPtr dictGet(ObjPtr d, const std::string& key);
    std::vector<uint8_t> streamData(ObjPtr obj);
    void   pageBox(ObjPtr page, double& x0,double& y0,double& x1,double& y1);

private:
    void   ensureXRef(int num);
    ObjPtr parseObjAt(int64_t offset);
    void   parseXRefTable(int pos, ObjPtr* trailerOut);
    void   parseXRefStream(int pos);
    int64_t findStartXRef();
    void   collectPages(ObjPtr node);
};
