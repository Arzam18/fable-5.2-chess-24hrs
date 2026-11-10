#pragma once
#include "position.h"

#ifndef NN_H
#define NN_H 256
#endif
constexpr int NN_BUCKETS = 8;

struct Accumulator {
    alignas(32) i16 v[2][NN_H];
    bool computed[2];
    bool refresh[2];   // the move leading here needs a full refresh for that perspective
    u8 nAdd, nSub;
    u8 addPc[2], addSq[2], subPc[2], subSq[2];
};

namespace NNUE {
    void init();                               // load the embedded network, if any
    bool loadFile(const std::string& path);
    bool available();
    void refresh(Accumulator& acc, const Position& pos, int persp);
    void update(Accumulator& dst, const Accumulator& src, int persp, int ksq);
    int evaluate(const Accumulator& acc, const Position& pos);
}
