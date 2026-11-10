// NNUE evaluation: (768 -> NN_H) x 2 perspectives, SCReLU, 8 output buckets by piece count.
// Features are mirrored horizontally so the perspective's king is on files a-d.
#include "nnue.h"
#include <cstdio>
#include <vector>
#include <algorithm>

__asm__(".section .rdata,\"dr\"\n.balign 64\n.globl g_netData\ng_netData:\n.incbin \"net.bin\"\n.globl g_netEnd\ng_netEnd:\n.byte 0\n.text\n");
extern "C" const unsigned char g_netData[];
extern "C" const unsigned char g_netEnd[];

namespace NNUE {

constexpr int QA = 1020, QB = 1024, QACT = 4064, SCALE = 400;   // act = ((v<<4)*(v<<4))>>16 = v*v/256

alignas(64) static i16 W0[768 * NN_KB * NN_H];
alignas(64) static i16 B0[NN_H];
alignas(64) static i16 W1[NN_BUCKETS][2 * NN_H];
static i32 B1[NN_BUCKETS];
static bool loaded = false;

static bool loadMem(const unsigned char* p, size_t size) {
    size_t need = 16 + sizeof(W0) + sizeof(B0) + sizeof(W1) + sizeof(B1);
    if (size < need) return false;
    i32 hdr[4];
    memcpy(hdr, p, 16);
    if (hdr[0] != 0x4E4E5548 || hdr[1] != NN_H || hdr[2] != NN_BUCKETS || (hdr[3] == 0 ? 1 : hdr[3]) != NN_KB) return false;
    p += 16;
    memcpy(W0, p, sizeof(W0)); p += sizeof(W0);
    memcpy(B0, p, sizeof(B0)); p += sizeof(B0);
    memcpy(W1, p, sizeof(W1)); p += sizeof(W1);
    memcpy(B1, p, sizeof(B1));
    loaded = true;
    return true;
}

void init() { loadMem(g_netData, (size_t)(g_netEnd - g_netData)); }

bool loadFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> buf;
    unsigned char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    return loadMem(buf.data(), buf.size());
}

bool available() { return loaded; }

// king bucket of the perspective's own king (square made relative to the perspective and mirrored to files a-d)
static inline int kingBucket(int persp, int ksq) {
#if NN_KB == 1
    (void)persp; (void)ksq;
    return 0;
#else
#if NN_KB == 5
    static const u8 MAP[32] = {0,0,1,2, 0,0,1,2, 3,3,3,3, 3,3,3,3, 4,4,4,4, 4,4,4,4, 4,4,4,4, 4,4,4,4};
#else
    static const u8 MAP[32] = {0,1,2,3, 4,4,5,5, 6,6,7,7, 6,6,7,7, 8,8,8,8, 8,8,8,8, 9,9,9,9, 9,9,9,9};
#endif
    int s = ksq ^ (persp == BLACK ? 56 : 0);
    int r = s >> 3, f = s & 7;
    if (f >= 4) f = 7 - f;
    return MAP[r * 4 + f];
#endif
}

int kingZone(int persp, int ksq) { return kingBucket(persp, ksq) * 2 + ((ksq & 7) >= 4 ? 1 : 0); }

static inline int featIdx(int persp, int ksq, int pc, int sq) {
    int c = pieceColor(pc) != persp;
    int s = sq ^ (persp == BLACK ? 56 : 0) ^ ((ksq & 7) >= 4 ? 7 : 0);
    return (kingBucket(persp, ksq) * 768 + c * 384 + pieceType(pc) * 64 + s) * NN_H;
}

void refresh(Accumulator& acc, const Position& pos, int persp) {
    i16* __restrict d = acc.v[persp];
    alignas(32) i32 dummy = 0; (void)dummy;
    memcpy(d, B0, sizeof(B0));
    int ksq = pos.kingSq(persp);
    u64 b = pos.occ();
    while (b) {
        int sq = poplsb(b);
        const i16* __restrict r = &W0[featIdx(persp, ksq, pos.board[sq], sq)];
        for (int i = 0; i < NN_H; i++) d[i] += r[i];
    }
    acc.computed[persp] = true;
}

void update(Accumulator& dst, const Accumulator& src, int persp, int ksq) {
    const i16* __restrict s = src.v[persp];
    i16* __restrict d = dst.v[persp];
    if (dst.nAdd == 0) {
        memcpy(d, s, sizeof(i16) * NN_H);
    } else {
        const i16* __restrict a0 = &W0[featIdx(persp, ksq, dst.addPc[0], dst.addSq[0])];
        const i16* __restrict s0 = &W0[featIdx(persp, ksq, dst.subPc[0], dst.subSq[0])];
        if (dst.nAdd == 1 && dst.nSub == 1) {
            for (int i = 0; i < NN_H; i++) d[i] = s[i] + a0[i] - s0[i];
        } else if (dst.nAdd == 1) {
            const i16* __restrict s1 = &W0[featIdx(persp, ksq, dst.subPc[1], dst.subSq[1])];
            for (int i = 0; i < NN_H; i++) d[i] = s[i] + a0[i] - s0[i] - s1[i];
        } else {
            const i16* __restrict a1 = &W0[featIdx(persp, ksq, dst.addPc[1], dst.addSq[1])];
            const i16* __restrict s1 = &W0[featIdx(persp, ksq, dst.subPc[1], dst.subSq[1])];
            for (int i = 0; i < NN_H; i++) d[i] = s[i] + a0[i] - s0[i] + a1[i] - s1[i];
        }
    }
    dst.computed[persp] = true;
}

int evaluate(const Accumulator& acc, const Position& pos) {
    const int stm = pos.side;
    const int bucket = std::min((popcount(pos.occ()) - 2) / 4, NN_BUCKETS - 1);
    const i16* w = W1[bucket];
    const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi16(QA);
    __m256i sum = zero;
    const i16* a = acc.v[stm];
    for (int i = 0; i < NN_H; i += 16) {
        __m256i v = _mm256_slli_epi16(_mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((const __m256i*)(a + i)), zero), qa), 4);
        __m256i wv = _mm256_load_si256((const __m256i*)(w + i));
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_mulhi_epi16(v, v), wv));
    }
    a = acc.v[stm ^ 1];
    w += NN_H;
    for (int i = 0; i < NN_H; i += 16) {
        __m256i v = _mm256_slli_epi16(_mm256_min_epi16(_mm256_max_epi16(_mm256_load_si256((const __m256i*)(a + i)), zero), qa), 4);
        __m256i wv = _mm256_load_si256((const __m256i*)(w + i));
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_mulhi_epi16(v, v), wv));
    }
    __m128i s128 = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
    s128 = _mm_add_epi32(s128, _mm_shuffle_epi32(s128, 0x4E));
    s128 = _mm_add_epi32(s128, _mm_shuffle_epi32(s128, 0xB1));
    i64 total = _mm_cvtsi128_si32(s128);
    i64 out = (total + B1[bucket]) * SCALE / ((i64)QACT * QB);
    return (int)out;
}

}   // namespace NNUE
