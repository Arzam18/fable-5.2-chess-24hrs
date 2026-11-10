#include "attacks.h"

u64 knightAtt[64], kingAtt[64], pawnAtt[2][64];
u64 betweenBB[64][64], lineBB[64][64];
u64 rookMask[64], bishopMask[64];
u64* rookPtr[64];
u64* bishopPtr[64];

static u64 rookTable[102400];
static u64 bishopTable[5248];

static const int rookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
static const int bishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

static u64 slide(int sq, u64 occ, const int dirs[4][2]) {
    u64 a = 0;
    int r = sq >> 3, f = sq & 7;
    for (int d = 0; d < 4; d++) {
        int rr = r + dirs[d][0], ff = f + dirs[d][1];
        while (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) {
            int s = rr * 8 + ff;
            a |= bit(s);
            if (occ & bit(s)) break;
            rr += dirs[d][0]; ff += dirs[d][1];
        }
    }
    return a;
}

void initAttacks() {
    for (int s = 0; s < 64; s++) {
        int r = s >> 3, f = s & 7;
        static const int kn[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
        static const int kg[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        knightAtt[s] = kingAtt[s] = 0;
        for (int i = 0; i < 8; i++) {
            int rr = r + kn[i][0], ff = f + kn[i][1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) knightAtt[s] |= bit(rr * 8 + ff);
            rr = r + kg[i][0]; ff = f + kg[i][1];
            if (rr >= 0 && rr < 8 && ff >= 0 && ff < 8) kingAtt[s] |= bit(rr * 8 + ff);
        }
        pawnAtt[WHITE][s] = pawnAtt[BLACK][s] = 0;
        if (r < 7) { if (f > 0) pawnAtt[WHITE][s] |= bit(s + 7); if (f < 7) pawnAtt[WHITE][s] |= bit(s + 9); }
        if (r > 0) { if (f > 0) pawnAtt[BLACK][s] |= bit(s - 9); if (f < 7) pawnAtt[BLACK][s] |= bit(s - 7); }
    }
    const u64 R1 = 0xFFULL, R8 = 0xFFULL << 56, FA = 0x0101010101010101ULL, FH = FA << 7;
    u64* rp = rookTable;
    u64* bp = bishopTable;
    for (int s = 0; s < 64; s++) {
        u64 rankBB = 0xFFULL << (s & 56), fileBB = FA << (s & 7);
        u64 edges = ((R1 | R8) & ~rankBB) | ((FA | FH) & ~fileBB);
        rookMask[s] = slide(s, 0, rookDirs) & ~edges;
        bishopMask[s] = slide(s, 0, bishopDirs) & ~edges;
        rookPtr[s] = rp;
        u64 sub = 0;
        do {
            rp[_pext_u64(sub, rookMask[s])] = slide(s, sub, rookDirs);
            sub = (sub - rookMask[s]) & rookMask[s];
        } while (sub);
        rp += 1ULL << popcount(rookMask[s]);
        bishopPtr[s] = bp;
        sub = 0;
        do {
            bp[_pext_u64(sub, bishopMask[s])] = slide(s, sub, bishopDirs);
            sub = (sub - bishopMask[s]) & bishopMask[s];
        } while (sub);
        bp += 1ULL << popcount(bishopMask[s]);
    }
    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++) {
            betweenBB[a][b] = lineBB[a][b] = 0;
            if (a == b) continue;
            if (rookAttacks(a, 0) & bit(b)) {
                betweenBB[a][b] = rookAttacks(a, bit(b)) & rookAttacks(b, bit(a));
                lineBB[a][b] = (rookAttacks(a, 0) & rookAttacks(b, 0)) | bit(a) | bit(b);
            } else if (bishopAttacks(a, 0) & bit(b)) {
                betweenBB[a][b] = bishopAttacks(a, bit(b)) & bishopAttacks(b, bit(a));
                lineBB[a][b] = (bishopAttacks(a, 0) & bishopAttacks(b, 0)) | bit(a) | bit(b);
            }
        }
}
