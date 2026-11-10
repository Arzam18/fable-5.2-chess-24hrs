#include "search.h"
#include "eval.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <thread>

static int lmrTable[2][64][64];

void initSearch() {
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++) {
            lmrTable[0][d][m] = int(0.85 + std::log(d) * std::log(m) / 2.3);
            lmrTable[1][d][m] = int(0.25 + std::log(d) * std::log(m) / 3.3);
        }
}

static inline int scoreToTT(int s, int ply) { return s >= SCORE_MATE_IN_MAX ? s + ply : s <= -SCORE_MATE_IN_MAX ? s - ply : s; }
static inline int scoreFromTT(int s, int ply) { return s >= SCORE_MATE_IN_MAX ? s - ply : s <= -SCORE_MATE_IN_MAX ? s + ply : s; }
static inline bool isNoisy(Move m) { return isCapture(m) || moveFlag(m) == MF_PROMO + 3; }
static inline void updHist(i16& h, int bonus) { h += bonus - h * std::abs(bonus) / 16384; }

void Searcher::clearHistory() {
    memset(butterfly, 0, sizeof(butterfly));
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

int Searcher::evaluate(const Position& pos) {
    return evaluateHCE(pos);
}

int Searcher::correctEval(const Position& pos, const Stack* ss, int raw) const {
    int us = pos.side;
    int c = pawnCorr[us][pos.pawnKey & (CORR_SIZE - 1)] * 2
        + nonPawnCorr[us][WHITE][pos.nonPawnKey[WHITE] & (CORR_SIZE - 1)] * 2
        + nonPawnCorr[us][BLACK][pos.nonPawnKey[BLACK] & (CORR_SIZE - 1)] * 2;
    if ((ss - 1)->move != MOVE_NONE && (ss - 2)->move != MOVE_NONE) {
        Move pm = (ss - 1)->move;
        c += (*(ss - 2)->contCorr)[pos.board[moveTo(pm)]][moveTo(pm)] * 2;
    }
    int v = raw * (220 - pos.halfmove) / 220 + c / 512;
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
    int pc = pos.board[moveFrom(m)], to = moveTo(m);
    return butterfly[pos.side][moveFrom(m)][to] + (*(ss - 1)->cont)[pc][to] + (*(ss - 2)->cont)[pc][to] + (*(ss - 4)->cont)[pc][to] / 2;
}

void Searcher::updateQuietHist(const Position& pos, Stack* ss, Move m, int bonus) {
    int pc = pos.board[moveFrom(m)], to = moveTo(m);
    updHist(butterfly[pos.side][moveFrom(m)][to], bonus);
    updHist((*(ss - 1)->cont)[pc][to], bonus);
    updHist((*(ss - 2)->cont)[pc][to], bonus);
    updHist((*(ss - 4)->cont)[pc][to], bonus);
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
                int sc = seeValue[cap] * 16 + s.captHist[pos.board[moveFrom(m)]][to][cap];
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
    if (ply >= MAX_PLY - 2) return inCheck ? 0 : correctEval(pos, ss, evaluate(pos));

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
        rawEval = ttEval != SCORE_NONE ? ttEval : evaluate(pos);
        ss->staticEval = bestScore = correctEval(pos, ss, rawEval);
        if (ttHit && (ttBound & (ttScore > bestScore ? BOUND_LOWER : BOUND_UPPER))) bestScore = ttScore;
        if (bestScore >= beta) {
            if (!ttHit) tt->store(pos.key, MOVE_NONE, scoreToTT(bestScore, ply), rawEval, 0, BOUND_LOWER, false);
            return bestScore;
        }
        if (bestScore > alpha) alpha = bestScore;
        futBase = ss->staticEval + 180;
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
        Position next = pos;
        next.makeMove(m);
        tt->prefetch(next.key);
        ss->move = m;
        ss->cont = &contHist[pos.board[moveFrom(m)]][moveTo(m)];
        ss->contCorr = &contCorrHist[pos.board[moveFrom(m)]][moveTo(m)];
        keyHist[rootIdx + ply + 1] = next.key;
        nodes++;
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
        if (ply >= MAX_PLY - 2) return inCheck ? 0 : correctEval(pos, ss, evaluate(pos));
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
        rawEval = ttEval != SCORE_NONE ? ttEval : evaluate(pos);
        ss->staticEval = eval = correctEval(pos, ss, rawEval);
        if (ttHit && (ttBound & (ttScore > eval ? BOUND_LOWER : BOUND_UPPER))) eval = ttScore;
    }
    if (!inCheck) {
        if ((ss - 2)->staticEval != SCORE_NONE) improving = ss->staticEval > (ss - 2)->staticEval;
        else if ((ss - 4)->staticEval != SCORE_NONE) improving = ss->staticEval > (ss - 4)->staticEval;
        else improving = true;
    }
    (ss + 2)->killer = MOVE_NONE;
    (ss + 1)->excluded = MOVE_NONE;

    if (!pvNode && !inCheck && !excluded) {
        // reverse futility pruning
        if (depth <= 8 && eval - 75 * (depth - improving) >= beta && eval < SCORE_MATE_IN_MAX && beta > -SCORE_MATE_IN_MAX)
            return (eval + beta) / 2;
        // razoring
        if (depth <= 4 && eval + 330 * depth <= alpha) {
            int v = qsearch(pos, ss, alpha, alpha + 1);
            if (v <= alpha) return v;
        }
        // null move pruning
        if (depth >= 3 && eval >= beta && ss->staticEval >= beta - 20 * depth + 180 && (ss - 1)->move != MOVE_NONE
            && pos.hasNonPawn(pos.side) && beta > -SCORE_MATE_IN_MAX && ply >= nmpMinPly) {
            int R = 4 + depth / 3 + std::min((eval - beta) / 200, 3);
            Position next = pos;
            next.makeNull();
            tt->prefetch(next.key);
            ss->move = MOVE_NONE;
            ss->cont = &contHist[0][0];
            ss->contCorr = &contCorrHist[0][0];
            keyHist[rootIdx + ply + 1] = next.key;
            nodes++;
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
        int probBeta = beta + 200;
        if (depth >= 5 && std::abs(beta) < SCORE_MATE_IN_MAX && !(ttHit && ttDepth >= depth - 3 && ttScore < probBeta)) {
            MovePicker mp(pos, *this, ss, ttMove, MOVE_NONE, true);
            Move m;
            while ((m = mp.next()) != MOVE_NONE) {
                if (!pos.isLegal(m)) continue;
                if (!pos.see(m, probBeta - ss->staticEval)) continue;
                Position next = pos;
                next.makeMove(m);
                tt->prefetch(next.key);
                ss->move = m;
                ss->cont = &contHist[pos.board[moveFrom(m)]][moveTo(m)];
                ss->contCorr = &contCorrHist[pos.board[moveFrom(m)]][moveTo(m)];
                keyHist[rootIdx + ply + 1] = next.key;
                nodes++;
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
            int lmrDepth = std::max(0, depth - 1 - lmrTable[quiet ? 0 : 1][std::min(depth, 63)][std::min(moveCount, 63)]);
            if (quiet) {
                if (moveCount >= (3 + depth * depth) / (2 - improving)) { mp.skipQuiets = true; continue; }
                if (!inCheck && lmrDepth <= 8 && ss->staticEval + 90 + 110 * lmrDepth <= alpha) { mp.skipQuiets = true; continue; }
                if (lmrDepth <= 4 && hist < -3500 * depth) continue;
                if (!pos.see(m, -25 * lmrDepth * lmrDepth)) continue;
            } else {
                if (!pos.see(m, -95 * depth)) continue;
            }
        }

        int ext = 0;
        if (!root && depth >= 7 && m == ttMove && !excluded && ttDepth >= depth - 3 && (ttBound & BOUND_LOWER)
            && std::abs(ttScore) < SCORE_MATE_IN_MAX && ply < 2 * rootDepth) {
            int sBeta = ttScore - depth * 5 / 4;
            int sDepth = (depth - 1) / 2;
            ss->excluded = m;
            int v = search(pos, ss, sBeta - 1, sBeta, sDepth, cutNode);
            ss->excluded = MOVE_NONE;
            if (stop.load(std::memory_order_relaxed)) return 0;
            if (v < sBeta) {
                ext = 1;
                if (!pvNode && v < sBeta - 20 && ss->doubleExt <= 6) ext = 2;
            } else if (sBeta >= beta) return sBeta;
            else if (ttScore >= beta) ext = -2;
            else if (cutNode) ext = -1;
        }

        Position next = pos;
        next.makeMove(m);
        tt->prefetch(next.key);
        ss->move = m;
        ss->cont = &contHist[pc][to];
        ss->contCorr = &contCorrHist[pc][to];
        (ss + 1)->doubleExt = ss->doubleExt + (ext >= 2);
        keyHist[rootIdx + ply + 1] = next.key;
        nodes++;
        const u64 nodesBefore = nodes;
        int newDepth = depth - 1 + ext;
        int score = 0;

        if (depth >= 2 && moveCount > 1 + root) {
            int r = lmrTable[quiet ? 0 : 1][std::min(depth, 63)][std::min(moveCount, 63)];
            r += !improving;
            if (cutNode) r += 1 + quiet;
            r -= ttPv;
            r -= pvNode;
            if (next.checkers) r -= 1;
            if (m == killer) r -= 1;
            if (quiet && ttNoisy) r += 1;
            r -= hist / (quiet ? 8192 : 6000);
            int d = std::clamp(newDepth - r, 1, newDepth);
            score = -search(next, ss + 1, -alpha - 1, -alpha, d, true);
            if (score > alpha && d < newDepth) {
                newDepth += (score > bestScore + 50 + 4 * newDepth) - (score < bestScore + newDepth);
                if (d < newDepth) score = -search(next, ss + 1, -alpha - 1, -alpha, newDepth, !cutNode);
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
                if (root) rootBest = m;
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
        int bonus = std::min(160 * depth - 60, 1900);
        if (!isNoisy(bestMove)) {
            ss->killer = bestMove;
            updateQuietHist(pos, ss, bestMove, bonus);
            for (int i = 0; i < nQuiets; i++) updateQuietHist(pos, ss, quietsTried[i], -bonus);
        } else {
            updHist(captHistEntry(pos, bestMove), bonus);
        }
        for (int i = 0; i < nCaps; i++) updHist(captHistEntry(pos, capsTried[i]), -bonus);
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
        stack[i].staticEval = SCORE_NONE; stack[i].ply = i - STACK_OFFSET; stack[i].doubleExt = 0;
        stack[i].cont = &contHist[0][0]; stack[i].contCorr = &contCorrHist[0][0];
    }
    memset(rootNodes, 0, sizeof(rootNodes));
    Stack* ss = &stack[STACK_OFFSET];
    keyHist[rootIdx] = root.key;
    pvLen[0] = 0;

    MoveList legal;
    genLegal(root, legal);
    if (legal.n == 0) {
        bestMove = MOVE_NONE;
        if (!silent) { fputs("bestmove 0000\n", stdout); fflush(stdout); }
        return;
    }
    rootBest = legal.moves[0];
    int maxDepth = lim.depth > 0 ? std::min(lim.depth, MAX_PLY - 10) : MAX_PLY - 10;
    int prevScore = 0, stability = 0;
    Move prevBest = MOVE_NONE;

    for (rootDepth = 1; rootDepth <= maxDepth; rootDepth++) {
        selDepth = 0;
        int alpha = -SCORE_INF, beta = SCORE_INF, delta = 12;
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
        prevScore = score;
        bestScore = score;
        completedDepth = rootDepth;
        bestMove = rootBest;
        if (!silent) printInfo(rootDepth, score);

        if (rootBest == prevBest) stability++; else stability = 0;
        prevBest = rootBest;
        if (lim.softNodes && nodes >= lim.softNodes) break;
        if (lim.softTime > 0) {
            if (legal.n == 1 && rootDepth >= 6) break;
            double frac = (double)rootNodes[rootBest & 4095] / (double)std::max<u64>(nodes, 1);
            double nodeScale = std::clamp(2.1 - 1.7 * frac, 0.55, 1.7);
            double stabScale = 1.3 - 0.07 * std::min(stability, 7);
            if ((double)elapsedMs() >= (double)lim.softTime * nodeScale * stabScale) break;
        }
    }
    bestMove = rootBest;
    if (lim.infinite)
        while (!stop.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!silent) {
        std::string s = "bestmove " + moveStr(bestMove) + "\n";
        fputs(s.c_str(), stdout);
        fflush(stdout);
    }
}
