#pragma once
#include <cstdint>

// One training position, 32 bytes.
struct Sample {
    uint64_t occ;        // occupancy
    uint8_t pcs[16];     // 4 bits per occupied square, in lsb-first order of occ; piece = color*6 + type
    int16_t score;       // search score in centipawns, white-relative
    uint8_t result;      // game result, white-relative: 0 = black win, 1 = draw, 2 = white win
    uint8_t stm;         // side to move: 0 white, 1 black
    uint8_t wksq, bksq;  // king squares
    uint8_t pad[2];      // pad[0] bit 0: best move was a capture/promotion ("noisy" position)
};
static_assert(sizeof(Sample) == 32, "Sample must be 32 bytes");
