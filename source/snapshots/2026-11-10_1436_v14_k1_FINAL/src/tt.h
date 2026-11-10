#pragma once
#include "types.h"

enum : int { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2, BOUND_EXACT = 3 };

struct TTEntry {
    u16 key;
    Move move;
    i16 score;
    i16 eval;
    u8 depth;
    u8 genBound;   // bits 0-1 bound, bit 2 pv, bits 3-7 generation
    int bound() const { return genBound & 3; }
    bool pv() const { return genBound & 4; }
};

struct TTCluster {
    TTEntry e[3];
    u16 pad;
};

struct TT {
    TTCluster* table = nullptr;
    u64 count = 0;
    u8 gen = 0;

    ~TT() { release(); }
    void release() { if (table) { _mm_free(table); table = nullptr; } }
    void resize(u64 mb) {
        release();
        count = mb * 1024 * 1024 / sizeof(TTCluster);
        if (count < 1024) count = 1024;
        table = (TTCluster*)_mm_malloc(count * sizeof(TTCluster), 64);
        clear();
    }
    void clear() { memset((void*)table, 0, count * sizeof(TTCluster)); gen = 0; }
    void newSearch() { gen = (gen + 1) & 31; }
    TTCluster* cluster(u64 key) const { return &table[(u64)(((unsigned __int128)key * count) >> 64)]; }
    void prefetch(u64 key) const { __builtin_prefetch(cluster(key)); }

    // returns entry pointer; hit set if key matched
    TTEntry* probe(u64 key, bool& hit) const {
        TTCluster* c = cluster(key);
        u16 k = (u16)key;
        for (int i = 0; i < 3; i++)
            if (c->e[i].key == k && c->e[i].bound() != BOUND_NONE) { hit = true; return &c->e[i]; }
        hit = false;
        return nullptr;
    }

    void store(u64 key, Move move, int score, int eval, int depth, int bound, bool pv) {
        TTCluster* c = cluster(key);
        u16 k = (u16)key;
        TTEntry* r = nullptr;
        int worst = 1 << 30;
        for (int i = 0; i < 3; i++) {
            TTEntry* e = &c->e[i];
            if (e->key == k && e->bound() != BOUND_NONE) { r = e; goto found; }
            int age = (gen - (e->genBound >> 3)) & 31;
            int v = e->bound() == BOUND_NONE ? -1000 : (int)e->depth - 8 * age;
            if (v < worst) { worst = v; r = e; }
        }
        // replacing a different position
        r->key = k; r->move = move; r->score = (i16)score; r->eval = (i16)eval; r->depth = (u8)depth;
        r->genBound = (u8)(bound | (pv ? 4 : 0) | (gen << 3));
        return;
    found:
        if (move != MOVE_NONE) r->move = move;
        if (bound == BOUND_EXACT || depth + 4 > r->depth || ((r->genBound >> 3) != gen)) {
            r->score = (i16)score; r->eval = (i16)eval; r->depth = (u8)depth;
            r->genBound = (u8)(bound | (pv ? 4 : 0) | (gen << 3));
        }
    }

    int hashfull() const {
        int n = 0;
        for (int i = 0; i < 1000; i++)
            for (int j = 0; j < 3; j++)
                if (table[i].e[j].bound() != BOUND_NONE && (table[i].e[j].genBound >> 3) == gen) n++;
        return n / 3;
    }
};
