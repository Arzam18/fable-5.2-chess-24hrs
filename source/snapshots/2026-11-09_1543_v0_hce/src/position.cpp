#include "position.h"
#include <sstream>

u64 zPsq[12][64], zEp[8], zCastle[16], zSide;
const int seeValue[7] = {100, 320, 330, 500, 950, 0, 0};
static u8 castleMask[64];

void initZobrist() {
    u64 s = 0x9E3779B97F4A7C15ULL;
    auto rnd = [&]() {
        s += 0x9E3779B97F4A7C15ULL;
        u64 z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    for (int p = 0; p < 12; p++) for (int q = 0; q < 64; q++) zPsq[p][q] = rnd();
    for (int i = 0; i < 8; i++) zEp[i] = rnd();
    for (int i = 0; i < 16; i++) zCastle[i] = rnd();
    zCastle[0] = 0;
    zSide = rnd();
    for (int i = 0; i < 64; i++) castleMask[i] = 15;
    castleMask[4] = 15 & ~3; castleMask[7] = 15 & ~1; castleMask[0] = 15 & ~2;
    castleMask[60] = 15 & ~12; castleMask[63] = 15 & ~4; castleMask[56] = 15 & ~8;
}

void Position::put(int pc, int sq) {
    int c = pieceColor(pc), pt = pieceType(pc);
    pieces[pt] |= bit(sq); colors[c] |= bit(sq); board[sq] = pc;
    key ^= zPsq[pc][sq];
    if (pt == PAWN) pawnKey ^= zPsq[pc][sq]; else nonPawnKey[c] ^= zPsq[pc][sq];
}

void Position::remove(int sq) {
    int pc = board[sq];
    int c = pieceColor(pc), pt = pieceType(pc);
    pieces[pt] ^= bit(sq); colors[c] ^= bit(sq); board[sq] = NO_PIECE;
    key ^= zPsq[pc][sq];
    if (pt == PAWN) pawnKey ^= zPsq[pc][sq]; else nonPawnKey[c] ^= zPsq[pc][sq];
}

void Position::setFen(const std::string& fen) {
    memset(this, 0, sizeof(*this));
    for (int i = 0; i < 64; i++) board[i] = NO_PIECE;
    std::istringstream ss(fen);
    std::string b, stm, cas, eps;
    int hm = 0, fm = 1;
    ss >> b >> stm >> cas >> eps;
    if (!(ss >> hm)) hm = 0;
    if (!(ss >> fm)) fm = 1;
    int sq = 56;
    for (char ch : b) {
        if (ch == '/') sq -= 16;
        else if (ch >= '1' && ch <= '8') sq += ch - '0';
        else {
            const char* names = "PNBRQKpnbrqk";
            const char* p = strchr(names, ch);
            if (p && sq >= 0 && sq < 64) put(int(p - names), sq);
            sq++;
        }
    }
    side = (stm == "b") ? BLACK : WHITE;
    castling = 0;
    for (char ch : cas) {
        if (ch == 'K' && board[4] == makePiece(WHITE, KING) && board[7] == makePiece(WHITE, ROOK)) castling |= 1;
        if (ch == 'Q' && board[4] == makePiece(WHITE, KING) && board[0] == makePiece(WHITE, ROOK)) castling |= 2;
        if (ch == 'k' && board[60] == makePiece(BLACK, KING) && board[63] == makePiece(BLACK, ROOK)) castling |= 4;
        if (ch == 'q' && board[60] == makePiece(BLACK, KING) && board[56] == makePiece(BLACK, ROOK)) castling |= 8;
    }
    ep = NO_SQ;
    if (eps.size() == 2 && eps[0] >= 'a' && eps[0] <= 'h' && (eps[1] == '3' || eps[1] == '6')) {
        int e = (eps[0] - 'a') + (eps[1] - '1') * 8;
        // only keep if a pawn of the side to move can capture (pseudo-legally)
        if (pawnAtt[side ^ 1][e] & bb(side, PAWN)) ep = e;
    }
    halfmove = hm > 250 ? 250 : hm;
    fullmove = fm;
    key ^= zCastle[castling];
    if (ep != NO_SQ) key ^= zEp[ep & 7];
    if (side == BLACK) key ^= zSide;
    updateCheckInfo();
}

std::string Position::fen() const {
    std::string r;
    for (int rk = 7; rk >= 0; rk--) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            int pc = board[rk * 8 + f];
            if (pc == NO_PIECE) empty++;
            else { if (empty) r += char('0' + empty); empty = 0; r += "PNBRQKpnbrqk"[pc]; }
        }
        if (empty) r += char('0' + empty);
        if (rk) r += '/';
    }
    r += side == WHITE ? " w " : " b ";
    if (!castling) r += '-';
    else { if (castling & 1) r += 'K'; if (castling & 2) r += 'Q'; if (castling & 4) r += 'k'; if (castling & 8) r += 'q'; }
    r += ' ';
    r += ep == NO_SQ ? "-" : sqStr(ep);
    r += ' ' + std::to_string(halfmove) + ' ' + std::to_string(fullmove);
    return r;
}

u64 Position::attackersTo(int sq, u64 o) const {
    return (pawnAtt[BLACK][sq] & bb(WHITE, PAWN)) | (pawnAtt[WHITE][sq] & bb(BLACK, PAWN))
        | (knightAtt[sq] & pieces[KNIGHT]) | (kingAtt[sq] & pieces[KING])
        | (bishopAttacks(sq, o) & (pieces[BISHOP] | pieces[QUEEN]))
        | (rookAttacks(sq, o) & (pieces[ROOK] | pieces[QUEEN]));
}

bool Position::attacked(int sq, int c, u64 o) const {
    if (pawnAtt[c ^ 1][sq] & bb(c, PAWN)) return true;
    if (knightAtt[sq] & bb(c, KNIGHT)) return true;
    if (kingAtt[sq] & bb(c, KING)) return true;
    if (bishopAttacks(sq, o) & (pieces[BISHOP] | pieces[QUEEN]) & colors[c]) return true;
    if (rookAttacks(sq, o) & (pieces[ROOK] | pieces[QUEEN]) & colors[c]) return true;
    return false;
}

void Position::updateCheckInfo() {
    int us = side, them = us ^ 1;
    int ksq = kingSq(us);
    u64 o = occ();
    checkers = attackersTo(ksq, o) & colors[them];
    pinned = 0;
    u64 snipers = ((rookAttacks(ksq, 0) & (pieces[ROOK] | pieces[QUEEN])) | (bishopAttacks(ksq, 0) & (pieces[BISHOP] | pieces[QUEEN]))) & colors[them];
    while (snipers) {
        int s = poplsb(snipers);
        u64 b = betweenBB[ksq][s] & o;
        if (b && !many(b)) pinned |= b & colors[us];
    }
}

bool Position::canCastle(int c, bool kingSide) const {
    if (checkers) return false;
    int right = c == WHITE ? (kingSide ? 1 : 2) : (kingSide ? 4 : 8);
    if (!(castling & right)) return false;
    int k = c == WHITE ? 4 : 60;
    u64 o = occ();
    if (kingSide) {
        if (o & (bit(k + 1) | bit(k + 2))) return false;
        return !attacked(k + 1, c ^ 1, o) && !attacked(k + 2, c ^ 1, o);
    } else {
        if (o & (bit(k - 1) | bit(k - 2) | bit(k - 3))) return false;
        return !attacked(k - 1, c ^ 1, o) && !attacked(k - 2, c ^ 1, o);
    }
}

void Position::makeMove(Move m) {
    int f = moveFrom(m), t = moveTo(m), fl = moveFlag(m);
    int us = side, them = us ^ 1;
    int pc = board[f];
    int pt = pieceType(pc);
    key ^= zSide;
    if (ep != NO_SQ) { key ^= zEp[ep & 7]; ep = NO_SQ; }
    if (halfmove < 250) halfmove++;
    if (fl == MF_EP) { remove(us == WHITE ? t - 8 : t + 8); halfmove = 0; }
    else if (fl & 4) { remove(t); halfmove = 0; }
    if (pt == PAWN) halfmove = 0;
    remove(f);
    if (fl & 8) put(makePiece(us, promoType(m)), t); else put(pc, t);
    if (fl == MF_KCASTLE) { int rk = board[t + 1]; remove(t + 1); put(rk, t - 1); }
    else if (fl == MF_QCASTLE) { int rk = board[t - 2]; remove(t - 2); put(rk, t + 1); }
    else if (fl == MF_DOUBLE) {
        int e = (f + t) >> 1;
        if (pawnAtt[us][e] & bb(them, PAWN)) { ep = e; key ^= zEp[e & 7]; }
    }
    int nc = castling & castleMask[f] & castleMask[t];
    key ^= zCastle[castling] ^ zCastle[nc];
    castling = nc;
    side = them;
    if (us == BLACK) fullmove++;
    updateCheckInfo();
}

void Position::makeNull() {
    key ^= zSide;
    if (ep != NO_SQ) { key ^= zEp[ep & 7]; ep = NO_SQ; }
    halfmove = 0;   // no repetition detection across a null move
    side ^= 1;
    updateCheckInfo();
}

bool Position::isLegal(Move m) const {
    int f = moveFrom(m), t = moveTo(m), fl = moveFlag(m);
    int us = side, them = us ^ 1;
    int ksq = kingSq(us);
    u64 o = occ();
    if (fl == MF_EP) {
        int cap = us == WHITE ? t - 8 : t + 8;
        u64 o2 = (o ^ bit(f) ^ bit(cap)) | bit(t);
        if (rookAttacks(ksq, o2) & (pieces[ROOK] | pieces[QUEEN]) & colors[them]) return false;
        if (bishopAttacks(ksq, o2) & (pieces[BISHOP] | pieces[QUEEN]) & colors[them]) return false;
        // knight / other pawn checks cannot coexist with an available ep capture unless the checker is the pushed pawn
        if (checkers & ~bit(cap)) {
            // checker is a slider (discovered); handled above. Anything else: illegal.
            if (checkers & (pieces[KNIGHT] | pieces[PAWN])) return false;
        }
        return true;
    }
    if (f == ksq) {
        if (fl == MF_KCASTLE || fl == MF_QCASTLE) return true;   // verified at generation
        return !attacked(t, them, o ^ bit(f));
    }
    if (checkers) {
        if (many(checkers)) return false;
        if (!((betweenBB[ksq][lsb(checkers)] | checkers) & bit(t))) return false;
    }
    if (pinned & bit(f)) return (lineBB[ksq][f] & bit(t)) != 0;
    return true;
}

bool Position::isPseudoLegal(Move m) const {
    if (m == MOVE_NONE) return false;
    int f = moveFrom(m), t = moveTo(m), fl = moveFlag(m);
    if (fl == 6 || fl == 7) return false;
    int pc = board[f];
    if (pc == NO_PIECE || pieceColor(pc) != side) return false;
    int target = board[t];
    if (target != NO_PIECE && (pieceColor(target) == side || pieceType(target) == KING)) return false;
    int pt = pieceType(pc);
    if (fl == MF_EP) return pt == PAWN && ep != NO_SQ && t == ep && (pawnAtt[side][f] & bit(t));
    bool cap = (fl & 4) != 0;
    if (cap != (target != NO_PIECE)) return false;
    if (pt == PAWN) {
        bool promo = (fl & 8) != 0;
        if ((relRank(side, t) == 7) != promo) return false;
        if (cap) return (pawnAtt[side][f] & bit(t)) != 0;
        int dir = side == WHITE ? 8 : -8;
        if (fl == MF_DOUBLE) return relRank(side, f) == 1 && t == f + 2 * dir && board[f + dir] == NO_PIECE;
        if (fl == MF_KCASTLE || fl == MF_QCASTLE) return false;
        return t == f + dir;
    }
    if (fl & 8) return false;
    if (fl == MF_DOUBLE) return false;
    if (fl == MF_KCASTLE || fl == MF_QCASTLE) {
        if (pt != KING) return false;
        int k = side == WHITE ? 4 : 60;
        if (f != k) return false;
        if (fl == MF_KCASTLE) return t == k + 2 && canCastle(side, true);
        return t == k - 2 && canCastle(side, false);
    }
    return (pieceAttacks(pt, f, occ()) & bit(t)) != 0;
}

bool Position::see(Move m, int threshold) const {
    int f = moveFrom(m), t = moveTo(m), fl = moveFlag(m);
    if (fl == MF_KCASTLE || fl == MF_QCASTLE) return threshold <= 0;
    int gain = fl == MF_EP ? seeValue[PAWN] : (board[t] != NO_PIECE ? seeValue[pieceType(board[t])] : 0);
    int victim = pieceType(board[f]);
    if (fl & 8) { gain += seeValue[promoType(m)] - seeValue[PAWN]; victim = promoType(m); }
    int balance = gain - threshold;
    if (balance < 0) return false;
    balance -= seeValue[victim];
    if (balance >= 0) return true;
    u64 o = (occ() ^ bit(f)) | bit(t);
    if (fl == MF_EP) o ^= bit(side == WHITE ? t - 8 : t + 8);
    u64 attackers = attackersTo(t, o) & o;
    u64 diag = pieces[BISHOP] | pieces[QUEEN], orth = pieces[ROOK] | pieces[QUEEN];
    int stm = side ^ 1;
    while (true) {
        u64 my = attackers & colors[stm];
        if (!my) break;
        int pt = PAWN;
        while (!(my & pieces[pt])) pt++;
        stm ^= 1;
        balance = -balance - 1 - seeValue[pt];
        if (balance >= 0) {
            if (pt == KING && (attackers & colors[stm])) stm ^= 1;
            break;
        }
        o ^= bit(lsb(my & pieces[pt]));
        if (pt == PAWN || pt == BISHOP || pt == QUEEN) attackers |= bishopAttacks(t, o) & diag;
        if (pt == ROOK || pt == QUEEN) attackers |= rookAttacks(t, o) & orth;
        attackers &= o;
    }
    return stm != side;
}

bool Position::insufficientMaterial() const {
    if (pieces[PAWN] | pieces[ROOK] | pieces[QUEEN]) return false;
    return popcount(pieces[KNIGHT] | pieces[BISHOP]) <= 1;
}

Move Position::parseMove(const std::string& s) const {
    MoveList ml;
    genLegal(*this, ml);
    for (int i = 0; i < ml.n; i++) if (moveStr(ml.moves[i]) == s) return ml.moves[i];
    return MOVE_NONE;
}

// ---------------------------------------------------------------- move generation

static inline void addPromos(MoveList& ml, int f, int t, bool cap, bool noisy) {
    if (cap) {
        for (int p = 3; p >= 0; p--) ml.add(makeMove(f, t, MF_PROMO_CAP + p));
    } else if (noisy) ml.add(makeMove(f, t, MF_PROMO + 3));
    else for (int p = 2; p >= 0; p--) ml.add(makeMove(f, t, MF_PROMO + p));
}

void genNoisy(const Position& pos, MoveList& ml) {
    int us = pos.side, them = us ^ 1;
    u64 o = pos.occ(), enemy = pos.colors[them];
    u64 pawns = pos.bb(us, PAWN);
    int up = us == WHITE ? 8 : -8;
    u64 r7 = us == WHITE ? 0x00FF000000000000ULL : 0x000000000000FF00ULL;
    u64 b = pawns;
    while (b) {
        int f = poplsb(b);
        u64 att = pawnAtt[us][f] & enemy;
        if (bit(f) & r7) {
            while (att) addPromos(ml, f, poplsb(att), true, true);
            if (!(o & bit(f + up))) addPromos(ml, f, f + up, false, true);
        } else {
            while (att) ml.add(makeMove(f, poplsb(att), MF_CAPTURE));
        }
    }
    if (pos.ep != NO_SQ) {
        u64 a = pawnAtt[them][pos.ep] & pawns;
        while (a) ml.add(makeMove(poplsb(a), pos.ep, MF_EP));
    }
    for (int pt = KNIGHT; pt <= KING; pt++) {
        u64 p = pos.bb(us, pt);
        while (p) {
            int f = poplsb(p);
            u64 att = pieceAttacks(pt, f, o) & enemy;
            while (att) ml.add(makeMove(f, poplsb(att), MF_CAPTURE));
        }
    }
}

void genQuiet(const Position& pos, MoveList& ml) {
    int us = pos.side;
    u64 o = pos.occ(), empty = ~o;
    u64 pawns = pos.bb(us, PAWN);
    int up = us == WHITE ? 8 : -8;
    u64 r7 = us == WHITE ? 0x00FF000000000000ULL : 0x000000000000FF00ULL;
    u64 r2 = us == WHITE ? 0x000000000000FF00ULL : 0x00FF000000000000ULL;
    u64 b = pawns;
    while (b) {
        int f = poplsb(b);
        int t = f + up;
        if (o & bit(t)) continue;
        if (bit(f) & r7) addPromos(ml, f, t, false, false);
        else {
            ml.add(makeMove(f, t, MF_QUIET));
            if ((bit(f) & r2) && !(o & bit(t + up))) ml.add(makeMove(f, t + up, MF_DOUBLE));
        }
    }
    for (int pt = KNIGHT; pt <= KING; pt++) {
        u64 p = pos.bb(us, pt);
        while (p) {
            int f = poplsb(p);
            u64 att = pieceAttacks(pt, f, o) & empty;
            while (att) ml.add(makeMove(f, poplsb(att), MF_QUIET));
        }
    }
    int k = us == WHITE ? 4 : 60;
    if (pos.canCastle(us, true)) ml.add(makeMove(k, k + 2, MF_KCASTLE));
    if (pos.canCastle(us, false)) ml.add(makeMove(k, k - 2, MF_QCASTLE));
}

void genLegal(const Position& pos, MoveList& ml) {
    MoveList tmp;
    genNoisy(pos, tmp);
    genQuiet(pos, tmp);
    for (int i = 0; i < tmp.n; i++) if (pos.isLegal(tmp.moves[i])) ml.add(tmp.moves[i]);
}

u64 perft(const Position& pos, int depth) {
    MoveList ml;
    genLegal(pos, ml);
    if (depth <= 1) return depth == 1 ? ml.n : 1;
    u64 n = 0;
    for (int i = 0; i < ml.n; i++) {
        Position next = pos;
        next.makeMove(ml.moves[i]);
        n += perft(next, depth - 1);
    }
    return n;
}
