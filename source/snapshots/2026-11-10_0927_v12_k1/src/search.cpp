#include "search.h"
#include "eval.h"
#include "nnue.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>
#include <string>

static int lmrTable[2][64][64];

// Tunable search parameters. With -DTUNE they are UCI-settable variables (for SPRT/SPSA runs);
// otherwise compile-time constants.
#ifdef TUNE
struct ParamReg { const char* name; int* value; int mn, mx; };
static std::vector<ParamReg>& paramList() { static std::vector<ParamReg> v; return v; }
struct ParamAdder { ParamAdder(const char* n, int* v, int mn, int mx) { paramList().push_back({n, v, mn, mx}); } };
#define PARAM(name, def, mn, mx) static int name = def; static ParamAdder adder_##name(#name, &name, mn, mx);
bool setParam(const std::string& name, int value) {
    for (auto& p : paramList()) {
        std::string n = p.name;
        for (auto& ch : n) ch = (char)tolower(ch);
        if (n == name) { *p.value = std::clamp(value, p.mn, p.mx); initSearch(); return true; }
    }
    return false;
}
void printParams() {
    for (auto& p : paramList())
        printf("option name %s type spin default %d min %d max %d\n", p.name, *p.value, p.mn, p.mx);
}
#else
#define PARAM(name, def, mn, mx) constexpr int name = def;
bool setParam(const std::string&, int) { return false; }
void printParams() {}
#endif

PARAM(rfpMargin, 73, 30, 150)
PARAM(razorMargin, 510, 150, 600)
PARAM(nmpEvalDiv, 194, 80, 400)
PARAM(probcutMargin, 212, 80, 400)
PARAM(futBaseMargin, 76, 20, 250)
PARAM(futMul, 162, 40, 220)
PARAM(histPruneMul, 2651, 1000, 8000)
PARAM(seeQuietMul, 12, 5, 80)
PARAM(seeNoisyMul, 72, 30, 200)
PARAM(seBetaMul, 20, 8, 48)
PARAM(seDoubleMargin, 18, 0, 80)
PARAM(lmrBase, 71, 20, 160)
PARAM(lmrDiv, 259, 140, 400)
PARAM(lmrNoisyBase, 32, -40, 120)
PARAM(lmrNoisyDiv, 340, 180, 600)
PARAM(lmrHistDiv, 9542, 3000, 20000)
PARAM(lmrCapHistDiv, 5815, 2000, 20000)
PARAM(deeperMargin, 47, 10, 150)
PARAM(histBonusMul, 210, 60, 400)
PARAM(histBonusSub, 97, -100, 300)
PARAM(histBonusMax, 2408, 600, 4000)
PARAM(aspDelta, 17, 4, 40)
PARAM(qsFutMargin, 344, 60, 400)
PARAM(useThreats, 1, 0, 1)
PARAM(usePawnHist, 1, 0, 1)
PARAM(evalHistMul, 6, 0, 40)
PARAM(useCutoffCnt, 1, 0, 1)
PARAM(lmrResearchHist, 1, 0, 1)
PARAM(useCapFut, 1, 0, 1)
PARAM(capFutMargin, 242, 50, 600)
PARAM(capFutMul, 250, 50, 500)
PARAM(nmpA, 284, 64, 512)
PARAM(nmpB, 25, 4, 48)
PARAM(nmpSeMul, 21, 0, 60)
PARAM(nmpSeBase, 156, 0, 400)
PARAM(lmpA, 233, 32, 512)
PARAM(lmpB, 60, 16, 160)
PARAM(rfpImp, 93, 0, 200)
PARAM(lmrNotImp, 1687, -1024, 3072)
PARAM(lmrCut, 903, -1024, 3072)
PARAM(lmrCutQuiet, 926, -1024, 3072)
PARAM(lmrTtPv, 1100, -1024, 3072)
PARAM(lmrPv, 1234, -1024, 3072)
PARAM(lmrCheck, 940, -1024, 3072)
PARAM(lmrKiller, 819, -1024, 3072)
PARAM(lmrTtNoisy, 951, -1024, 3072)
PARAM(lmrCutoff, 1115, -1024, 3072)
PARAM(corrPawnW, 11, 0, 64)
PARAM(corrNonPawnW, 21, 0, 64)
PARAM(corrContW, 16, 0, 64)
PARAM(cont4W, 585, 0, 2048)
PARAM(pawnHistW, 1083, 0, 3072)
PARAM(histMalusMul, 201, 60, 400)
PARAM(histMalusMax, 2116, 600, 4000)
PARAM(mvvMul, 12, 4, 48)
#ifdef TUNE
static ParamAdder adder_seeP("seePawn", &seeValue[PAWN], 50, 200);
static ParamAdder adder_seeN("seeKnight", &seeValue[KNIGHT], 200, 500);
static ParamAdder adder_seeB("seeBishop", &seeValue[BISHOP], 200, 500);
static ParamAdder adder_seeR("seeRook", &seeValue[ROOK], 300, 800);
static ParamAdder adder_seeQ("seeQueen", &seeValue[QUEEN], 600, 1500);
#endif
PARAM(tmSoftDiv, 26, 8, 80)
PARAM(tmIncPct, 60, 10, 100)
PARAM(tmHardMul10, 45, 15, 120)
PARAM(tmHardPct, 30, 5, 70)
PARAM(tmNodeBase, 210, 100, 300)
PARAM(tmNodeMul, 170, 0, 300)
PARAM(tmStabBase, 130, 80, 200)
PARAM(tmStabStep, 7, 0, 20)
PARAM(tmFallMul, 10, 0, 30)

void setTimeLimits(Limits& lim, i64 time, i64 inc, int mtg, int overhead) {
    i64 t = std::max<i64>(1, time - overhead);
    double soft, hard;
    if (mtg > 0) {
        int n = std::min(mtg, 40);
        soft = (double)t / (n + 1) * 0.9 + inc * tmIncPct / 100.0;
        hard = std::min((double)t / (n > 1 ? 2.5 : 1.2), soft * tmHardMul10 / 10.0);
    } else {
        soft = (double)t / tmSoftDiv + inc * tmIncPct / 100.0;
        hard = std::min((double)t * tmHardPct / 100.0, soft * tmHardMul10 / 10.0);
    }
    lim.softTime = std::max<i64>(1, std::min<i64>((i64)soft, t));
    lim.hardTime = std::max<i64>(1, std::min<i64>((i64)hard, t));
}

void initSearch() {
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++) {
            lmrTable[0][d][m] = int(1024.0 * (lmrBase / 100.0 + std::log(d) * std::log(m) / (lmrDiv / 100.0)));
            lmrTable[1][d][m] = int(1024.0 * (lmrNoisyBase / 100.0 + std::log(d) * std::log(m) / (lmrNoisyDiv / 100.0)));
        }
}

static inline int scoreToTT(int s, int ply) { return s >= SCORE_MATE_IN_MAX ? s + ply : s <= -SCORE_MATE_IN_MAX ? s - ply : s; }
static inline int scoreFromTT(int s, int ply) { return s >= SCORE_MATE_IN_MAX ? s - ply : s <= -SCORE_MATE_IN_MAX ? s + ply : s; }
static inline bool isNoisy(Move m) { return isCapture(m) || moveFlag(m) == MF_PROMO + 3; }
static inline void updHist(i16& h, int bonus) { h += bonus - h * std::abs(bonus) / 16384; }

void Searcher::clearHistory() {
    memset(butterfly, 0, sizeof(butterfly));
    memset(pawnHist, 0, sizeof(pawnHist));
    memset(captHist, 0, sizeof(captHist));
    memset(contHist, 0, sizeof(contHist));
    memset(pawnCorr, 0, sizeof(pawnCorr));
    memset(nonPawnCorr, 0, sizeof(nonPawnCorr));
    memset(contCorrHist, 0, sizeof(contCorrHist));
}

void Searcher::setRootHistory(const u64* keys, int n) {
    if (n > 1024) { keys += n - 1024; n = 1024; }
    for (int i = 0; i < n; i++) keyHist[i] = keys[i];
    rootIdx = n - 1;
}

void Searcher::checkTime() {
    checkCnt = 1024;
    if (rootDepth <= 1) return;
    if (lim.hardNodes && nodes >= lim.hardNodes) { stop = true; return; }
    if (lim.hardTime > 0 && elapsedMs() >= lim.hardTime) stop = true;
}

bool Searcher::isRepetition(const Position& pos, int ply) const {
    int idx = rootIdx + ply;
    int last = std::max(0, idx - pos.halfmove);
    for (int i = idx - 4; i >= last; i -= 2)
        if (keyHist[i] == pos.key) return true;
    return false;
}

void Searcher::pushAcc(const Position& pos, Move m, int ply) {
    Accumulator& a = accStack[ply + 1];
    a.computed[0] = a.computed[1] = false;
    a.refresh[0] = a.refresh[1] = false;
    a.nAdd = a.nSub = 0;
    if (m == MOVE_NONE) return;
    int f = moveFrom(m), t = moveTo(m), fl = moveFlag(m), us = pos.side;
    int pc = pos.board[f];
    if (pieceType(pc) == KING && NNUE::kingZone(us, f) != NNUE::kingZone(us, t)) a.refresh[us] = true;
    a.subPc[0] = (u8)pc; a.subSq[0] = (u8)f; a.nSub = 1;
    if (fl == MF_EP) { a.subPc[1] = (u8)makePiece(us ^ 1, PAWN); a.subSq[1] = (u8)(us == WHITE ? t - 8 : t + 8); a.nSub = 2; }
    else if (fl & 4) { a.subPc[1] = pos.board[t]; a.subSq[1] = (u8)t; a.nSub = 2; }
    a.addPc[0] = (u8)((fl & 8) ? makePiece(us, promoType(m)) : pc); a.addSq[0] = (u8)t; a.nAdd = 1;
    if (fl == MF_KCASTLE) {
        a.subPc[1] = (u8)makePiece(us, ROOK); a.subSq[1] = (u8)(t + 1); a.nSub = 2;
        a.addPc[1] = (u8)makePiece(us, ROOK); a.addSq[1] = (u8)(t - 1); a.nAdd = 2;
    } else if (fl == MF_QCASTLE) {
        a.subPc[1] = (u8)makePiece(us, ROOK); a.subSq[1] = (u8)(t - 2); a.nSub = 2;
        a.addPc[1] = (u8)makePiece(us, ROOK); a.addSq[1] = (u8)(t + 1); a.nAdd = 2;
    }
}

int Searcher::evaluate(const Position& pos, int ply) {
    if (!NNUE::available()) return evaluateHCE(pos);
    Accumulator& cur = accStack[ply];
    for (int p = 0; p < 2; p++) {
        if (cur.computed[p]) continue;
        int j = ply;
        while (!accStack[j].computed[p] && !accStack[j].refresh[p]) j--;
        if (!accStack[j].computed[p]) { NNUE::refresh(cur, pos, p); continue; }
        int ksq = pos.kingSq(p);
        for (int k = j + 1; k <= ply; k++) NNUE::update(accStack[k], accStack[k - 1], p, ksq);
    }
    return std::clamp(NNUE::evaluate(cur, pos), -10000, 10000);
}

inline void Searcher::doMove(const Position& pos, Position& next, Stack* ss, Move m) {
    pushAcc(pos, m, ss->ply);
    next = pos;
    if (m == MOVE_NONE) {
        next.makeNull();
        ss->cont = &contHist[0][0];
        ss->contCorr = &contCorrHist[0][0];
    } else {
        int pc = pos.board[moveFrom(m)], to = moveTo(m);
        next.makeMove(m);
        ss->cont = &contHist[pc][to];
        ss->contCorr = &contCorrHist[pc][to];
    }
    tt->prefetch(next.key);
    ss->move = m;
    keyHist[rootIdx + ss->ply + 1] = next.key;
    nodes++;
}

int Searcher::correctEval(const Position& pos, const Stack* ss, int raw) const {
    int us = pos.side;
    int c = pawnCorr[us][pos.pawnKey & (CORR_SIZE - 1)] * corrPawnW
        + nonPawnCorr[us][WHITE][pos.nonPawnKey[WHITE] & (CORR_SIZE - 1)] * corrNonPawnW
        + nonPawnCorr[us][BLACK][pos.nonPawnKey[BLACK] & (CORR_SIZE - 1)] * corrNonPawnW;
    if ((ss - 1)->move != MOVE_NONE && (ss - 2)->move != MOVE_NONE) {
        Move pm = (ss - 1)->move;
        c += (*(ss - 2)->contCorr)[pos.board[moveTo(pm)]][moveTo(pm)] * corrContW;
    }
    int v = raw * (220 - pos.halfmove) / 220 + c / 4096;
    return std::clamp(v, -SCORE_MATE_IN_MAX + 1, SCORE_MATE_IN_MAX - 1);
}

static inline void updCorr(i16& e, int diff, int w) {
    int v = (e * (256 - w) + diff * 256 * w) / 256;
    e = (i16)std::clamp(v, -12000, 12000);
}

void Searcher::updateCorr(const Position& pos, const Stack* ss, int diff, int depth) {
    int us = pos.side;
    int w = std::min(depth + 1, 16);
    diff = std::clamp(diff, -300, 300);
    updCorr(pawnCorr[us][pos.pawnKey & (CORR_SIZE - 1)], diff, w);
    updCorr(nonPawnCorr[us][WHITE][pos.nonPawnKey[WHITE] & (CORR_SIZE - 1)], diff, w);
    updCorr(nonPawnCorr[us][BLACK][pos.nonPawnKey[BLACK] & (CORR_SIZE - 1)], diff, w);
    if ((ss - 1)->move != MOVE_NONE && (ss - 2)->move != MOVE_NONE) {
        Move pm = (ss - 1)->move;
        updCorr((*(ss - 2)->contCorr)[pos.board[moveTo(pm)]][moveTo(pm)], diff, w);
    }
}

int Searcher::quietHist(const Position& pos, const Stack* ss, Move m) const {
    int f = moveFrom(m), to = moveTo(m), pc = pos.board[f];
    return butterfly[pos.side][(ss->threats >> f) & 1][(ss->threats >> to) & 1][f][to]
        + (*(ss - 1)->cont)[pc][to] + (*(ss - 2)->cont)[pc][to] + (*(ss - 4)->cont)[pc][to] * cont4W / 1024
        + pawnHist[pos.pawnKey & (PAWN_HIST_SIZE - 1)][pc][to] * pawnHistW / 1024;
}

void Searcher::updateContHist(const Position& pos, Stack* ss, Move m, int bonus) {
    int pc = pos.board[moveFrom(m)], to = moveTo(m);
    updHist((*(ss - 1)->cont)[pc][to], bonus);
    updHist((*(ss - 2)->cont)[pc][to], bonus);
    updHist((*(ss - 4)->cont)[pc][to], bonus);
}

void Searcher::updateQuietHist(const Position& pos, Stack* ss, Move m, int bonus) {
    int f = moveFrom(m), to = moveTo(m), pc = pos.board[f];
    updHist(butterfly[pos.side][(ss->threats >> f) & 1][(ss->threats >> to) & 1][f][to], bonus);
    if (usePawnHist) updHist(pawnHist[pos.pawnKey & (PAWN_HIST_SIZE - 1)][pc][to], bonus);
    updateContHist(pos, ss, m, bonus);
}

i16& Searcher::captHistEntry(const Position& pos, Move m) {
    int to = moveTo(m);
    int cap = moveFlag(m) == MF_EP ? PAWN : (pos.board[to] == NO_PIECE ? NO_PT : pieceType(pos.board[to]));
    return captHist[pos.board[moveFrom(m)]][to][cap];
}

// ---------------------------------------------------------------- move picker

enum { ST_TT, ST_GEN_NOISY, ST_GOOD_NOISY, ST_KILLER, ST_GEN_QUIET, ST_QUIET, ST_BAD, ST_DONE };

struct MovePicker {
    const Position& pos;
    Searcher& s;
    Stack* ss;
    Move ttMove, killer;
    bool qs;
    bool skipQuiets = false;
    int stage = ST_TT;
    int n = 0, idx = 0, nBad = 0, badIdx = 0;
    Move moves[MAX_MOVES];
    int scores[MAX_MOVES];
    Move bad[MAX_MOVES];

    MovePicker(const Position& p, Searcher& sr, Stack* st, Move tt, Move k, bool q)
        : pos(p), s(sr), ss(st), ttMove(tt), killer(k), qs(q) {}

    Move pickBest() {
        int b = idx;
        for (int i = idx + 1; i < n; i++) if (scores[i] > scores[b]) b = i;
        std::swap(moves[b], moves[idx]); std::swap(scores[b], scores[idx]);
        return moves[idx++];
    }

    Move next() {
        switch (stage) {
        case ST_TT:
            stage = ST_GEN_NOISY;
            if (ttMove != MOVE_NONE && (!qs || isNoisy(ttMove)) && pos.isPseudoLegal(ttMove)) return ttMove;
            ttMove = MOVE_NONE;
            [[fallthrough]];
        case ST_GEN_NOISY: {
            MoveList ml;
            genNoisy(pos, ml);
            for (int i = 0; i < ml.n; i++) {
                Move m = ml.moves[i];
                if (m == ttMove) continue;
                int to = moveTo(m);
                int cap = moveFlag(m) == MF_EP ? PAWN : (pos.board[to] == NO_PIECE ? NO_PT : pieceType(pos.board[to]));
                int sc = seeValue[cap] * mvvMul + s.captHist[pos.board[moveFrom(m)]][to][cap];
                if (isPromo(m)) sc += promoType(m) == QUEEN ? 16000 : -20000;
                moves[n] = m; scores[n] = sc; n++;
            }
            stage = ST_GOOD_NOISY;
        }
            [[fallthrough]];
        case ST_GOOD_NOISY:
            while (idx < n) {
                int sc = scores[idx];   // overwritten by pickBest swap; re-read below
                Move m = pickBest();
                sc = scores[idx - 1];
                if (!qs && !pos.see(m, -sc / 64)) { bad[nBad++] = m; continue; }
                return m;
            }
            if (qs) { stage = ST_DONE; return MOVE_NONE; }
            stage = ST_KILLER;
            [[fallthrough]];
        case ST_KILLER:
            stage = ST_GEN_QUIET;
            if (!skipQuiets && killer != MOVE_NONE && killer != ttMove && !isNoisy(killer) && pos.isPseudoLegal(killer)) return killer;
            killer = MOVE_NONE;
            [[fallthrough]];
        case ST_GEN_QUIET:
            n = idx = 0;
            if (!skipQuiets) {
                MoveList ml;
                genQuiet(pos, ml);
                for (int i = 0; i < ml.n; i++) {
                    Move m = ml.moves[i];
                    if (m == ttMove || m == killer) continue;
                    moves[n] = m; scores[n] = s.quietHist(pos, ss, m); n++;
                }
            }
            stage = ST_QUIET;
            [[fallthrough]];
        case ST_QUIET:
            if (!skipQuiets && idx < n) return pickBest();
            stage = ST_BAD;
            [[fallthrough]];
        case ST_BAD:
            if (badIdx < nBad) return bad[badIdx++];
            stage = ST_DONE;
            [[fallthrough]];
        default:
            return MOVE_NONE;
        }
    }
};

// ---------------------------------------------------------------- search

int Searcher::qsearch(const Position& pos, Stack* ss, int alpha, int beta) {
    const int ply = ss->ply;
    const bool pvNode = beta - alpha > 1;
    pvLen[ply] = 0;
    if (--checkCnt <= 0) checkTime();
    if (stop.load(std::memory_order_relaxed)) return 0;
    if (ply + 1 > selDepth) selDepth = ply + 1;
    const bool inCheck = pos.checkers != 0;
    if (pos.halfmove >= 100 || pos.insufficientMaterial() || isRepetition(pos, ply)) return 0;
    if (ply >= MAX_PLY - 2) return inCheck ? 0 : correctEval(pos, ss, evaluate(pos, ply));
    ss->threats = (useThreats && inCheck) ? pos.attacksBy(pos.side ^ 1) : 0;

    bool ttHit = false;
    TTEntry* tte = tt->probe(pos.key, ttHit);
    Move ttMove = MOVE_NONE;
    int ttScore = SCORE_NONE, ttBound = BOUND_NONE, ttEval = SCORE_NONE;
    bool ttPv = pvNode;
    if (ttHit) {
        ttMove = tte->move; ttScore = scoreFromTT(tte->score, ply); ttBound = tte->bound(); ttEval = tte->eval;
        ttPv = ttPv || tte->pv();
        if (!pvNode && (ttBound == BOUND_EXACT || (ttBound == BOUND_LOWER && ttScore >= beta) || (ttBound == BOUND_UPPER && ttScore <= alpha)))
            return ttScore;
    }

    int rawEval = SCORE_NONE, bestScore, futBase = -SCORE_INF;
    if (inCheck) {
        ss->staticEval = SCORE_NONE;
        bestScore = -SCORE_INF;
    } else {
        rawEval = ttEval != SCORE_NONE ? ttEval : evaluate(pos, ply);
        ss->staticEval = bestScore = correctEval(pos, ss, rawEval);
        if (ttHit && (ttBound & (ttScore > bestScore ? BOUND_LOWER : BOUND_UPPER))) bestScore = ttScore;
        if (bestScore >= beta) {
            if (!ttHit) tt->store(pos.key, MOVE_NONE, scoreToTT(bestScore, ply), rawEval, 0, BOUND_LOWER, false);
            return bestScore;
        }
        if (bestScore > alpha) alpha = bestScore;
        futBase = ss->staticEval + qsFutMargin;
    }

    MovePicker mp(pos, *this, ss, ttMove, MOVE_NONE, !inCheck);
    Move bestMove = MOVE_NONE, m;
    int moveCount = 0;
    while ((m = mp.next()) != MOVE_NONE) {
        if (!pos.isLegal(m)) continue;
        moveCount++;
        if (!inCheck) {
            if (!isPromo(m) && moveFlag(m) != MF_EP) {
                int fv = futBase + seeValue[pieceType(pos.board[moveTo(m)])];
                if (fv <= alpha) { if (fv > bestScore) bestScore = fv; continue; }
            }
            if (!pos.see(m, 0)) continue;
        } else if (bestScore > -SCORE_MATE_IN_MAX && !isNoisy(m)) {
            continue;   // found a non-losing evasion: only captures after that
        }
        Position next;
        doMove(pos, next, ss, m);
        int score = -qsearch(next, ss + 1, -beta, -alpha);
        if (stop.load(std::memory_order_relaxed)) return 0;
        if (score > bestScore) {
            bestScore = score;
            if (score > alpha) {
                bestMove = m;
                alpha = score;
                if (score >= beta) break;
            }
        }
    }
    if (inCheck && moveCount == 0) return -SCORE_MATE + ply;
    int bound = bestScore >= beta ? BOUND_LOWER : BOUND_UPPER;
    tt->store(pos.key, bestMove, scoreToTT(bestScore, ply), rawEval, 0, bound, ttPv);
    return bestScore;
}

int Searcher::search(const Position& pos, Stack* ss, int alpha, int beta, int depth, bool cutNode) {
    if (depth <= 0) return qsearch(pos, ss, alpha, beta);
    const int ply = ss->ply;
    const bool root = ply == 0;
    const bool pvNode = beta - alpha > 1;
    pvLen[ply] = 0;
    if (--checkCnt <= 0) checkTime();
    if (stop.load(std::memory_order_relaxed)) return 0;
    if (ply + 1 > selDepth) selDepth = ply + 1;
    const bool inCheck = pos.checkers != 0;
    if (!root) {
        if (pos.halfmove >= 100 || pos.insufficientMaterial() || isRepetition(pos, ply)) return 0;
        if (ply >= MAX_PLY - 2) return inCheck ? 0 : correctEval(pos, ss, evaluate(pos, ply));
        alpha = std::max(alpha, -SCORE_MATE + ply);
        beta = std::min(beta, SCORE_MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    const Move excluded = ss->excluded;
    bool ttHit = false;
    TTEntry* tte = excluded ? nullptr : tt->probe(pos.key, ttHit);
    Move ttMove = MOVE_NONE;
    int ttScore = SCORE_NONE, ttBound = BOUND_NONE, ttEval = SCORE_NONE, ttDepth = -1;
    bool ttPv = pvNode;
    if (ttHit) {
        ttMove = tte->move; ttScore = scoreFromTT(tte->score, ply); ttBound = tte->bound(); ttEval = tte->eval; ttDepth = tte->depth;
        ttPv = ttPv || tte->pv();
        if (!pvNode && ttDepth >= depth && pos.halfmove < 90
            && (ttBound == BOUND_EXACT || (ttBound == BOUND_LOWER && ttScore >= beta) || (ttBound == BOUND_UPPER && ttScore <= alpha)))
            return ttScore;
    }

    int rawEval = SCORE_NONE, eval = SCORE_NONE;
    bool improving = false;
    if (inCheck) {
        ss->staticEval = SCORE_NONE;
    } else if (excluded) {
        eval = ss->staticEval;
    } else {
        rawEval = ttEval != SCORE_NONE ? ttEval : evaluate(pos, ply);
        ss->staticEval = eval = correctEval(pos, ss, rawEval);
        if (ttHit && (ttBound & (ttScore > eval ? BOUND_LOWER : BOUND_UPPER))) eval = ttScore;
    }
    if (!inCheck) {
        if ((ss - 2)->staticEval != SCORE_NONE) improving = ss->staticEval > (ss - 2)->staticEval;
        else if ((ss - 4)->staticEval != SCORE_NONE) improving = ss->staticEval > (ss - 4)->staticEval;
        else improving = true;
    }
    (ss + 2)->killer = MOVE_NONE;
    (ss + 2)->cutoffCnt = 0;
    (ss + 1)->excluded = MOVE_NONE;
    ss->threats = useThreats ? pos.attacksBy(pos.side ^ 1) : 0;

    // static-eval difference feeds the history of the opponent's last quiet move
    if (evalHistMul && !inCheck && !excluded && (ss - 1)->move != MOVE_NONE && !isNoisy((ss - 1)->move) && (ss - 1)->staticEval != SCORE_NONE) {
        int bonus = std::clamp(-evalHistMul * ((ss - 1)->staticEval + ss->staticEval), -1200, 1200);
        Move pm = (ss - 1)->move;
        int pf = moveFrom(pm), pt = moveTo(pm);
        updHist(butterfly[pos.side ^ 1][((ss - 1)->threats >> pf) & 1][((ss - 1)->threats >> pt) & 1][pf][pt], bonus);
    }

    if (!pvNode && !inCheck && !excluded) {
        // reverse futility pruning
        if (depth <= 8 && eval - rfpMargin * depth + rfpImp * improving >= beta && eval < SCORE_MATE_IN_MAX && beta > -SCORE_MATE_IN_MAX)
            return (eval + beta) / 2;
        // razoring
        if (depth <= 4 && eval + razorMargin * depth <= alpha) {
            int v = qsearch(pos, ss, alpha, alpha + 1);
            if (v <= alpha) return v;
        }
        // null move pruning
        if (depth >= 3 && eval >= beta && ss->staticEval >= beta - nmpSeMul * depth + nmpSeBase && (ss - 1)->move != MOVE_NONE
            && pos.hasNonPawn(pos.side) && beta > -SCORE_MATE_IN_MAX && ply >= nmpMinPly) {
            int R = (nmpA + nmpB * depth) / 64 + std::min((eval - beta) / nmpEvalDiv, 3);
            Position next;
            doMove(pos, next, ss, MOVE_NONE);
            int v = -search(next, ss + 1, -beta, -beta + 1, depth - R, !cutNode);
            if (stop.load(std::memory_order_relaxed)) return 0;
            if (v >= beta) {
                if (v >= SCORE_MATE_IN_MAX) v = beta;
                if (depth < 14 || nmpMinPly > 0) return v;
                nmpMinPly = ply + 3 * (depth - R) / 4;
                int v2 = search(pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly = 0;
                if (stop.load(std::memory_order_relaxed)) return 0;
                if (v2 >= beta) return v;
            }
        }
        // probcut
        int probBeta = beta + probcutMargin;
        if (depth >= 5 && std::abs(beta) < SCORE_MATE_IN_MAX && !(ttHit && ttDepth >= depth - 3 && ttScore < probBeta)) {
            MovePicker mp(pos, *this, ss, ttMove, MOVE_NONE, true);
            Move m;
            while ((m = mp.next()) != MOVE_NONE) {
                if (!pos.isLegal(m)) continue;
                if (!pos.see(m, probBeta - ss->staticEval)) continue;
                Position next;
                doMove(pos, next, ss, m);
                int v = -qsearch(next, ss + 1, -probBeta, -probBeta + 1);
                if (v >= probBeta) v = -search(next, ss + 1, -probBeta, -probBeta + 1, depth - 4, !cutNode);
                if (stop.load(std::memory_order_relaxed)) return 0;
                if (v >= probBeta) {
                    tt->store(pos.key, m, scoreToTT(v, ply), rawEval, depth - 3, BOUND_LOWER, ttPv);
                    return v;
                }
            }
        }
    }

    // internal iterative reduction
    if (depth >= 4 && ttMove == MOVE_NONE && (pvNode || cutNode) && !excluded) depth--;

    const Move killer = ss->killer;
    MovePicker mp(pos, *this, ss, ttMove, killer, false);
    Move quietsTried[64], capsTried[32];
    int nQuiets = 0, nCaps = 0;
    int bestScore = -SCORE_INF, moveCount = 0;
    Move bestMove = MOVE_NONE, m;
    const bool ttNoisy = ttMove != MOVE_NONE && isNoisy(ttMove);

    while ((m = mp.next()) != MOVE_NONE) {
        if (m == excluded) continue;
        if (!pos.isLegal(m)) continue;
        moveCount++;
        const bool quiet = !isNoisy(m);
        const int pc = pos.board[moveFrom(m)], to = moveTo(m);
        const int hist = quiet ? quietHist(pos, ss, m) : (int)captHistEntry(pos, m);

        if (!root && bestScore > -SCORE_MATE_IN_MAX && pos.hasNonPawn(pos.side)) {
            int lmrDepth = std::max(0, depth - 1 - lmrTable[quiet ? 0 : 1][std::min(depth, 63)][std::min(moveCount, 63)] / 1024);
            if (quiet) {
                if (moveCount >= (lmpA + lmpB * depth * depth) / (64 * (2 - improving))) { mp.skipQuiets = true; continue; }
                if (!inCheck && lmrDepth <= 8 && ss->staticEval + futBaseMargin + futMul * lmrDepth <= alpha) { mp.skipQuiets = true; continue; }
                if (lmrDepth <= 4 && hist < -histPruneMul * depth) continue;
                if (!pos.see(m, -seeQuietMul * lmrDepth * lmrDepth)) continue;
            } else {
                if (useCapFut && !inCheck && lmrDepth < 6 && !isPromo(m)) {
                    int capVal = moveFlag(m) == MF_EP ? seeValue[PAWN] : seeValue[pieceType(pos.board[to])];
                    if (ss->staticEval + capFutMargin + capFutMul * lmrDepth + capVal <= alpha) continue;
                }
                if (!pos.see(m, -seeNoisyMul * depth)) continue;
            }
        }

        int ext = 0;
        if (!root && depth >= 7 && m == ttMove && !excluded && ttDepth >= depth - 3 && (ttBound & BOUND_LOWER)
            && std::abs(ttScore) < SCORE_MATE_IN_MAX && ply < 2 * rootDepth) {
            int sBeta = ttScore - depth * seBetaMul / 16;
            int sDepth = (depth - 1) / 2;
            ss->excluded = m;
            int v = search(pos, ss, sBeta - 1, sBeta, sDepth, cutNode);
            ss->excluded = MOVE_NONE;
            if (stop.load(std::memory_order_relaxed)) return 0;
            if (v < sBeta) {
                ext = 1;
                if (!pvNode && v < sBeta - seDoubleMargin && ss->doubleExt <= 6) ext = 2;
            } else if (sBeta >= beta) return sBeta;
            else if (ttScore >= beta) ext = -2;
            else if (cutNode) ext = -1;
        }

        Position next;
        doMove(pos, next, ss, m);
        (ss + 1)->doubleExt = ss->doubleExt + (ext >= 2);
        const u64 nodesBefore = nodes;
        int newDepth = depth - 1 + ext;
        int score = 0;

        if (depth >= 2 && moveCount > 1 + root) {
            int r = lmrTable[quiet ? 0 : 1][std::min(depth, 63)][std::min(moveCount, 63)];
            if (!improving) r += lmrNotImp;
            if (cutNode) r += lmrCut + (quiet ? lmrCutQuiet : 0);
            if (ttPv) r -= lmrTtPv;
            if (pvNode) r -= lmrPv;
            if (next.checkers) r -= lmrCheck;
            if (m == killer) r -= lmrKiller;
            if (quiet && ttNoisy) r += lmrTtNoisy;
            if (useCutoffCnt && (ss + 1)->cutoffCnt > 3) r += lmrCutoff;
            r = (r >> 10) - hist / (quiet ? lmrHistDiv : lmrCapHistDiv);
            int d = std::clamp(newDepth - r, 1, newDepth);
            score = -search(next, ss + 1, -alpha - 1, -alpha, d, true);
            if (score > alpha && d < newDepth) {
                newDepth += (score > bestScore + deeperMargin + 4 * newDepth) - (score < bestScore + newDepth);
                if (d < newDepth) score = -search(next, ss + 1, -alpha - 1, -alpha, newDepth, !cutNode);
                if (lmrResearchHist && quiet && !stop.load(std::memory_order_relaxed) && (score <= alpha || score >= beta)) {
                    int b = std::min(histBonusMul * depth - histBonusSub, histBonusMax);
                    updateContHist(pos, ss, m, score >= beta ? b : -b);
                }
            }
        } else if (!pvNode || moveCount > 1) {
            score = -search(next, ss + 1, -alpha - 1, -alpha, newDepth, !cutNode);
        }
        if (pvNode && (moveCount == 1 || score > alpha))
            score = -search(next, ss + 1, -beta, -alpha, newDepth, false);
        if (stop.load(std::memory_order_relaxed)) return 0;
        if (root) rootNodes[m & 4095] += nodes - nodesBefore + 1;

        if (score > bestScore) {
            bestScore = score;
            if (score > alpha) {
                bestMove = m;
                alpha = score;
                if (pvNode) {
                    pv[ply][0] = m;
                    int cl = pvLen[ply + 1];
                    memcpy(&pv[ply][1], &pv[ply + 1][0], cl * sizeof(Move));
                    pvLen[ply] = cl + 1;
                }
                if (root) { rootBest = m; rootScore = score; }
                if (score >= beta) break;
            }
        }
        if (m != bestMove) {
            if (quiet) { if (nQuiets < 64) quietsTried[nQuiets++] = m; }
            else if (nCaps < 32) capsTried[nCaps++] = m;
        }
    }

    if (moveCount == 0) return excluded ? alpha : (inCheck ? -SCORE_MATE + ply : 0);

    if (bestScore >= beta) {
        ss->cutoffCnt++;
        int bonus = std::min(histBonusMul * depth - histBonusSub, histBonusMax);
        int malus = std::min(histMalusMul * depth - histBonusSub, histMalusMax);
        if (!isNoisy(bestMove)) {
            ss->killer = bestMove;
            updateQuietHist(pos, ss, bestMove, bonus);
            for (int i = 0; i < nQuiets; i++) updateQuietHist(pos, ss, quietsTried[i], -malus);
        } else {
            updHist(captHistEntry(pos, bestMove), bonus);
        }
        for (int i = 0; i < nCaps; i++) updHist(captHistEntry(pos, capsTried[i]), -malus);
    }

    if (!excluded) {
        int bound = bestScore >= beta ? BOUND_LOWER : (pvNode && bestMove != MOVE_NONE ? BOUND_EXACT : BOUND_UPPER);
        tt->store(pos.key, bestMove, scoreToTT(bestScore, ply), rawEval, depth, bound, ttPv);
        if (!inCheck && (bestMove == MOVE_NONE || !isNoisy(bestMove))
            && !(bound == BOUND_LOWER && bestScore <= ss->staticEval)
            && !(bound == BOUND_UPPER && bestScore >= ss->staticEval))
            updateCorr(pos, ss, bestScore - ss->staticEval, depth);
    }
    return bestScore;
}

void Searcher::printInfo(int depth, int score) {
    i64 ms = elapsedMs();
    std::string s = "info depth " + std::to_string(depth) + " seldepth " + std::to_string(selDepth) + " score ";
    if (score >= SCORE_MATE_IN_MAX) s += "mate " + std::to_string((SCORE_MATE - score + 1) / 2);
    else if (score <= -SCORE_MATE_IN_MAX) s += "mate " + std::to_string(-(SCORE_MATE + score) / 2);
    else s += "cp " + std::to_string(score);
    s += " nodes " + std::to_string(nodes) + " nps " + std::to_string(nodes * 1000 / (u64)std::max<i64>(ms, 1))
        + " time " + std::to_string(ms) + " hashfull " + std::to_string(tt->hashfull()) + " pv";
    if (pvLen[0] == 0) s += " " + moveStr(rootBest);
    for (int i = 0; i < pvLen[0]; i++) s += " " + moveStr(pv[0][i]);
    s += "\n";
    fputs(s.c_str(), stdout);
    fflush(stdout);
}

void Searcher::run(const Position& root) {
    nodes = 0; checkCnt = 1024; nmpMinPly = 0; completedDepth = 0; bestScore = 0;
    tt->newSearch();
    for (int i = 0; i < MAX_PLY + 16; i++) {
        stack[i].move = MOVE_NONE; stack[i].excluded = MOVE_NONE; stack[i].killer = MOVE_NONE;
        stack[i].staticEval = SCORE_NONE; stack[i].ply = i - STACK_OFFSET; stack[i].doubleExt = 0; stack[i].threats = 0; stack[i].cutoffCnt = 0;
        stack[i].cont = &contHist[0][0]; stack[i].contCorr = &contCorrHist[0][0];
    }
    memset(rootNodes, 0, sizeof(rootNodes));
    Stack* ss = &stack[STACK_OFFSET];
    keyHist[rootIdx] = root.key;
    accStack[0].computed[0] = accStack[0].computed[1] = false;
    accStack[0].refresh[0] = accStack[0].refresh[1] = true;
    if (NNUE::available()) { NNUE::refresh(accStack[0], root, WHITE); NNUE::refresh(accStack[0], root, BLACK); }
    pvLen[0] = 0;

    MoveList legal;
    genLegal(root, legal);
    if (legal.n == 0) {
        bestMove = MOVE_NONE;
        if (!silent) { fputs("bestmove 0000\n", stdout); fflush(stdout); }
        return;
    }
    rootBest = legal.moves[0];
    lastInfoMove = MOVE_NONE;
    rootScore = 0;
    int maxDepth = lim.depth > 0 ? std::min(lim.depth, MAX_PLY - 10) : MAX_PLY - 10;
    int prevScore = 0, stability = 0;
    Move prevBest = MOVE_NONE;

    for (rootDepth = 1; rootDepth <= maxDepth; rootDepth++) {
        selDepth = 0;
        int alpha = -SCORE_INF, beta = SCORE_INF, delta = aspDelta;
        if (rootDepth >= 4) { alpha = std::max(-SCORE_INF, prevScore - delta); beta = std::min<int>(SCORE_INF, prevScore + delta); }
        int score = 0, failHigh = 0;
        while (true) {
            score = search(root, ss, alpha, beta, std::max(1, rootDepth - failHigh), false);
            if (stop.load(std::memory_order_relaxed)) break;
            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-SCORE_INF, score - delta);
                failHigh = 0;
            } else if (score >= beta) {
                beta = std::min<int>(SCORE_INF, score + delta);
                if (failHigh < 3) failHigh++;
            } else break;
            delta += delta / 2;
            if (delta > 1500) { alpha = -SCORE_INF; beta = SCORE_INF; }
        }
        if (stop.load(std::memory_order_relaxed)) break;
        const int scoreDrop = rootDepth > 1 ? prevScore - score : 0;
        prevScore = score;
        bestScore = score;
        completedDepth = rootDepth;
        bestMove = rootBest;
        lastInfoMove = rootBest;
        if (!silent) printInfo(rootDepth, score);

        if (rootBest == prevBest) stability++; else stability = 0;
        prevBest = rootBest;
        if (lim.softNodes && nodes >= lim.softNodes) break;
        if (lim.softTime > 0) {
            if (legal.n == 1 && rootDepth >= 6) break;
            double frac = (double)rootNodes[rootBest & 4095] / (double)std::max<u64>(nodes, 1);
            double nodeScale = std::clamp(tmNodeBase / 100.0 - tmNodeMul / 100.0 * frac, 0.55, 1.7);
            double stabScale = tmStabBase / 100.0 - tmStabStep / 100.0 * std::min(stability, 7);
            // falling score: spend more; rising score: spend a little less (tmFallMul = 0 disables)
            double fallScale = std::clamp(1.0 + tmFallMul * scoreDrop / 1000.0, 0.75, 1.5);
            if ((double)elapsedMs() >= (double)lim.softTime * nodeScale * stabScale * fallScale) break;
        }
    }
    bestMove = rootBest;
    // the aborted iteration found a different best move: report it so the last PV matches bestmove
    if (!silent && bestMove != lastInfoMove && completedDepth > 0) printInfo(rootDepth, rootScore);
    if (lim.infinite)
        while (!stop.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!silent) {
        std::string s = "bestmove " + moveStr(bestMove) + "\n";
        fputs(s.c_str(), stdout);
        fflush(stdout);
    }
}
