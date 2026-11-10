#pragma once
#include "position.h"
#include "tt.h"
#include <atomic>
#include <chrono>

struct Limits {
    i64 softTime = 0, hardTime = 0;   // ms; 0 = no limit
    int depth = 0;                    // 0 = no limit
    u64 softNodes = 0, hardNodes = 0; // 0 = no limit
    bool infinite = false;
};

typedef i16 PieceToTable[12][64];

struct Stack {
    Move move;
    Move excluded;
    Move killer;
    int staticEval;
    int ply;
    int doubleExt;
    PieceToTable* cont;
    PieceToTable* contCorr;
};

constexpr int STACK_OFFSET = 6;
constexpr int CORR_SIZE = 16384;

struct Searcher {
    TT* tt = nullptr;
    std::atomic<bool> stop{false};
    Limits lim;
    std::chrono::steady_clock::time_point startTime;
    bool silent = false;

    // results
    Move bestMove = MOVE_NONE;
    int bestScore = 0;
    int completedDepth = 0;

    u64 nodes = 0;
    int selDepth = 0;
    int rootDepth = 0;
    int checkCnt = 0;
    int nmpMinPly = 0;
    Move rootBest = MOVE_NONE;
    u64 rootNodes[4096];

    u64 keyHist[1024 + MAX_PLY + 8];
    int rootIdx = 0;

    Stack stack[MAX_PLY + 16];
    Move pv[MAX_PLY + 4][MAX_PLY + 4];
    int pvLen[MAX_PLY + 4];

    i16 butterfly[2][64][64];
    i16 captHist[12][64][7];
    PieceToTable contHist[12][64];
    i16 pawnCorr[2][CORR_SIZE];
    i16 nonPawnCorr[2][2][CORR_SIZE];
    PieceToTable contCorrHist[12][64];

    Searcher() { clearHistory(); }
    void clearHistory();
    void setRootHistory(const u64* keys, int n);   // keys of positions since last irreversible move, last = root
    void run(const Position& root);

    i64 elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    }
    void checkTime();
    bool isRepetition(const Position& pos, int ply) const;
    int evaluate(const Position& pos);
    int correctEval(const Position& pos, const Stack* ss, int raw) const;
    void updateCorr(const Position& pos, const Stack* ss, int diff, int depth);
    int quietHist(const Position& pos, const Stack* ss, Move m) const;
    void updateQuietHist(const Position& pos, Stack* ss, Move m, int bonus);
    i16& captHistEntry(const Position& pos, Move m);
    int search(const Position& pos, Stack* ss, int alpha, int beta, int depth, bool cutNode);
    int qsearch(const Position& pos, Stack* ss, int alpha, int beta);
    void printInfo(int depth, int score);
};

void initSearch();
