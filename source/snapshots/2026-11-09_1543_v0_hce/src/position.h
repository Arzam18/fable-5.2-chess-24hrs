#pragma once
#include "types.h"
#include "attacks.h"

extern u64 zPsq[12][64], zEp[8], zCastle[16], zSide;
void initZobrist();

extern const int seeValue[7];

struct Position {
    u64 pieces[6];
    u64 colors[2];
    u64 key, pawnKey, nonPawnKey[2];
    u64 checkers, pinned;
    u8 board[64];
    i8 side;
    i8 ep;
    u8 castling;
    u8 halfmove;
    u16 fullmove;

    u64 occ() const { return colors[0] | colors[1]; }
    u64 bb(int c, int pt) const { return pieces[pt] & colors[c]; }
    int kingSq(int c) const { return lsb(pieces[KING] & colors[c]); }
    bool hasNonPawn(int c) const { return colors[c] & ~(pieces[PAWN] | pieces[KING]); }

    void put(int pc, int sq);
    void remove(int sq);
    void setFen(const std::string& fen);
    std::string fen() const;
    void updateCheckInfo();
    u64 attackersTo(int sq, u64 occ) const;
    bool attacked(int sq, int byColor, u64 occ) const;
    bool canCastle(int c, bool kingSide) const;
    void makeMove(Move m);
    void makeNull();
    bool isLegal(Move m) const;
    bool isPseudoLegal(Move m) const;
    bool see(Move m, int threshold) const;
    bool insufficientMaterial() const;
    Move parseMove(const std::string& s) const;
};

void genNoisy(const Position& pos, MoveList& ml);
void genQuiet(const Position& pos, MoveList& ml);
void genLegal(const Position& pos, MoveList& ml);
u64 perft(const Position& pos, int depth);
