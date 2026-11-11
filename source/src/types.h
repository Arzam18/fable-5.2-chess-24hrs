#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#if defined(_WIN32)
    #include <malloc.h>
#endif
// Native SIMD headers are selected for the target architecture.
#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
    #include <arm_neon.h>
    #define FABLE_ARM_NEON 1
#else
    #include <immintrin.h>
    #define FABLE_ARM_NEON 0
#endif

typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;
typedef int64_t i64; typedef int32_t i32; typedef int16_t i16; typedef int8_t i8;

enum : int { WHITE, BLACK };
enum : int { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PT };
enum : int { NO_PIECE = 12 };
enum : int { NO_SQ = 64 };

constexpr int MAX_PLY = 220;
constexpr int MAX_MOVES = 256;
constexpr int SCORE_MATE = 32000;
constexpr int SCORE_INF = 32001;
constexpr int SCORE_NONE = 32002;
constexpr int SCORE_MATE_IN_MAX = SCORE_MATE - MAX_PLY - 1;

inline int lsb(u64 b) { return __builtin_ctzll(b); }
inline int popcount(u64 b) { return __builtin_popcountll(b); }
inline int poplsb(u64& b) { int s = lsb(b); b &= b - 1; return s; }
inline u64 bit(int s) { return 1ULL << s; }
inline bool many(u64 b) { return b & (b - 1); }

inline int pieceColor(int pc) { return pc >= 6; }
inline int pieceType(int pc) { return pc >= 6 ? pc - 6 : pc; }
inline int makePiece(int c, int pt) { return c * 6 + pt; }
inline int fileOf(int s) { return s & 7; }
inline int rankOf(int s) { return s >> 3; }
inline int relRank(int c, int s) { return c == WHITE ? (s >> 3) : 7 - (s >> 3); }

// Move: from(6) | to(6) | flag(4)
typedef u16 Move;
constexpr Move MOVE_NONE = 0;
enum : int {
    MF_QUIET = 0, MF_DOUBLE = 1, MF_KCASTLE = 2, MF_QCASTLE = 3, MF_CAPTURE = 4, MF_EP = 5,
    MF_PROMO = 8, MF_PROMO_CAP = 12
};
inline Move makeMove(int from, int to, int flag) { return Move(from | (to << 6) | (flag << 12)); }
inline int moveFrom(Move m) { return m & 63; }
inline int moveTo(Move m) { return (m >> 6) & 63; }
inline int moveFlag(Move m) { return m >> 12; }
inline bool isCapture(Move m) { return (m >> 12) & 4; }
inline bool isPromo(Move m) { return (m >> 12) & 8; }
inline int promoType(Move m) { return KNIGHT + ((m >> 12) & 3); }
inline bool isCastle(Move m) { int f = m >> 12; return f == MF_KCASTLE || f == MF_QCASTLE; }

inline std::string sqStr(int s) { std::string r; r += char('a' + (s & 7)); r += char('1' + (s >> 3)); return r; }
inline std::string moveStr(Move m) {
    if (m == MOVE_NONE) return "0000";
    std::string r = sqStr(moveFrom(m)) + sqStr(moveTo(m));
    if (isPromo(m)) r += "nbrq"[promoType(m) - KNIGHT];
    return r;
}

struct MoveList {
    Move moves[MAX_MOVES];
    int n = 0;
    void add(Move m) { moves[n++] = m; }
};
