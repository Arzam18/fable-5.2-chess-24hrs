#pragma once
#include "types.h"

extern u64 knightAtt[64], kingAtt[64], pawnAtt[2][64];
extern u64 betweenBB[64][64], lineBB[64][64];
extern u64 rookMask[64], bishopMask[64];
extern u64* rookPtr[64];
extern u64* bishopPtr[64];

void initAttacks();

inline u64 rookAttacks(int s, u64 occ) { return rookPtr[s][_pext_u64(occ, rookMask[s])]; }
inline u64 bishopAttacks(int s, u64 occ) { return bishopPtr[s][_pext_u64(occ, bishopMask[s])]; }
inline u64 queenAttacks(int s, u64 occ) { return rookAttacks(s, occ) | bishopAttacks(s, occ); }
inline u64 pieceAttacks(int pt, int s, u64 occ) {
    switch (pt) {
    case KNIGHT: return knightAtt[s];
    case BISHOP: return bishopAttacks(s, occ);
    case ROOK: return rookAttacks(s, occ);
    case QUEEN: return queenAttacks(s, occ);
    case KING: return kingAtt[s];
    }
    return 0;
}
