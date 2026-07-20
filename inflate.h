/* inflate.h — RFC 1950 (zlib) + RFC 1951 (deflate) decompressor
   Single-header, pure C89, no dependencies.
   Call: zinflate(src, src_len, &out_len) → malloc'd buffer or NULL */

#ifndef INFLATE_H
#define INFLATE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *src;
    int            src_len;
    int            pos;       /* next byte to read from src */
    uint32_t       bits;      /* bit accumulator (LSB first) */
    int            n_bits;    /* valid bits in accumulator */
    uint8_t       *dst;
    int            dst_cap;
    int            dst_len;
    int            err;
} ZStream;

static void _zs_put(ZStream *z, uint8_t b) {
    if (z->dst_len >= z->dst_cap) {
        int nc = z->dst_cap ? z->dst_cap * 2 : 4096;
        uint8_t *nb = (uint8_t *)realloc(z->dst, nc);
        if (!nb) { z->err = 1; return; }
        z->dst = nb;
        z->dst_cap = nc;
    }
    z->dst[z->dst_len++] = b;
}

static void _zs_fill(ZStream *z) {
    while (z->n_bits <= 24 && z->pos < z->src_len) {
        z->bits |= (uint32_t)z->src[z->pos++] << z->n_bits;
        z->n_bits += 8;
    }
}

static uint32_t _zs_read(ZStream *z, int n) {
    uint32_t v;
    _zs_fill(z);
    v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->n_bits -= n;
    return v;
}

static uint8_t _zs_byte(ZStream *z) {
    /* read a byte, consuming from bit buffer if available */
    if (z->n_bits >= 8) {
        uint8_t b = (uint8_t)(z->bits & 0xFF);
        z->bits >>= 8;
        z->n_bits -= 8;
        return b;
    }
    if (z->pos < z->src_len) return z->src[z->pos++];
    z->err = 1;
    return 0;
}

/* Huffman table: count[length] + symbol[] sorted by (length, value) */
typedef struct {
    uint16_t count[16];
    uint16_t sym[288 + 32];
} _Huff;

static void _huff_build(_Huff *h, const uint8_t *lens, int n) {
    uint16_t off[17];
    int i;
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++) if (lens[i] <= 15) h->count[lens[i]]++;
    h->count[0] = 0;
    off[1] = 0;
    for (i = 1; i < 16; i++) off[i+1] = off[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (lens[i]) h->sym[off[lens[i]]++] = (uint16_t)i;
}

static int _huff_dec(_Huff *h, ZStream *z) {
    int code = 0, first = 0, idx = 0, len;
    for (len = 1; len <= 15; len++) {
        code |= (int)_zs_read(z, 1);
        {
            int c = h->count[len];
            if (code - c < first) return h->sym[idx + (code - first)];
            idx   += c;
            first  = (first + c) << 1;
            code <<= 1;
        }
    }
    z->err = 1;
    return -1;
}

static const uint16_t _len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t _len_ext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t _dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const uint8_t _dist_ext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
    9,9,10,10,11,11,12,12,13,13
};

static void _inflate_codes(ZStream *z, _Huff *lit, _Huff *dst_huff) {
    for (;;) {
        int sym = _huff_dec(lit, z);
        if (z->err) return;
        if (sym < 256) {
            _zs_put(z, (uint8_t)sym);
        } else if (sym == 256) {
            break;
        } else {
            int li = sym - 257;
            int len, di, dist;
            if (li >= 29) { z->err = 1; return; }
            len  = _len_base[li]  + (int)_zs_read(z, _len_ext[li]);
            di   = _huff_dec(dst_huff, z);
            if (z->err || di >= 30) { z->err = 1; return; }
            dist = _dist_base[di] + (int)_zs_read(z, _dist_ext[di]);
            if (dist > z->dst_len) { z->err = 1; return; }
            while (len-- > 0)
                _zs_put(z, z->dst[z->dst_len - dist]);
        }
    }
}

static void _inflate_stored(ZStream *z) {
    uint8_t b[4]; int i;
    uint16_t len, nlen;
    /* byte-align bit buffer */
    z->bits   = 0;
    z->n_bits = 0;
    for (i = 0; i < 4; i++) b[i] = _zs_byte(z);
    if (z->err) return;
    len  = (uint16_t)(b[0] | (b[1] << 8));
    nlen = (uint16_t)(b[2] | (b[3] << 8));
    if ((uint16_t)(len ^ nlen) != 0xFFFFu) { z->err = 1; return; }
    while (len--) {
        uint8_t c;
        if (z->pos >= z->src_len) { z->err = 1; return; }
        c = z->src[z->pos++];
        _zs_put(z, c);
    }
}

static void _inflate_fixed(ZStream *z) {
    uint8_t ll[288], dl[30]; int i;
    _Huff lit, dst;
    for (i=0;   i<144; i++) ll[i]=8;
    for (i=144; i<256; i++) ll[i]=9;
    for (i=256; i<280; i++) ll[i]=7;
    for (i=280; i<288; i++) ll[i]=8;
    for (i=0;   i<30;  i++) dl[i]=5;
    _huff_build(&lit, ll, 288);
    _huff_build(&dst, dl, 30);
    _inflate_codes(z, &lit, &dst);
}

static void _inflate_dynamic(ZStream *z) {
    static const int cl_ord[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    int hlit  = (int)_zs_read(z, 5) + 257;
    int hdist = (int)_zs_read(z, 5) + 1;
    int hclen = (int)_zs_read(z, 4) + 4;
    uint8_t cl[19] = {0};
    _Huff clh, lit, dst;
    uint8_t lens[288 + 32];
    int i, n = hlit + hdist, j = 0;

    for (i = 0; i < hclen; i++) cl[cl_ord[i]] = (uint8_t)_zs_read(z, 3);
    _huff_build(&clh, cl, 19);

    while (j < n && !z->err) {
        int s = _huff_dec(&clh, z);
        if (z->err) return;
        if (s < 16) {
            lens[j++] = (uint8_t)s;
        } else if (s == 16) {
            uint8_t prev = j ? lens[j-1] : 0;
            int rep = (int)_zs_read(z, 2) + 3;
            while (rep-- && j < n) lens[j++] = prev;
        } else if (s == 17) {
            int rep = (int)_zs_read(z, 3) + 3;
            while (rep-- && j < n) lens[j++] = 0;
        } else {
            int rep = (int)_zs_read(z, 7) + 11;
            while (rep-- && j < n) lens[j++] = 0;
        }
    }

    _huff_build(&lit, lens,       hlit);
    _huff_build(&dst, lens + hlit, hdist);
    _inflate_codes(z, &lit, &dst);
}

static unsigned char *zinflate(const unsigned char *src, int src_len, int *out_len) {
    ZStream z;
    int bfinal;

    if (!src || src_len < 2) return NULL;
    if ((src[0] & 0x0F) != 8) return NULL;              /* CM must be deflate */
    if (((unsigned)(src[0]*256 + src[1])) % 31 != 0) return NULL; /* FCHECK */

    memset(&z, 0, sizeof(z));
    z.src     = src + 2;          /* skip 2-byte zlib header */
    z.src_len = src_len - 2;

    do {
        bfinal = (int)_zs_read(&z, 1);
        switch (_zs_read(&z, 2)) {
        case 0: _inflate_stored(&z);  break;
        case 1: _inflate_fixed(&z);   break;
        case 2: _inflate_dynamic(&z); break;
        default: z.err = 1;           break;
        }
    } while (!bfinal && !z.err);

    if (z.err) { free(z.dst); return NULL; }
    *out_len = z.dst_len;
    return z.dst;
}

#endif /* INFLATE_H */
