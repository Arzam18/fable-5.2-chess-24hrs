// Self-play training data generator.
//   engine datagen <threads> <out-prefix> <softNodes> <totalPositions> [seed] [randomPlies]
// Each thread writes <out-prefix>_<seed>_<t>.bin, 32 bytes per position (struct Sample).
// Create a file named STOP next to the output files to stop gracefully.
#include "search.h"
#include "sample.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include <fstream>

static std::atomic<u64> totalPositions{0}, totalGames{0};
static std::atomic<bool> dgStop{false};

static Sample makeSample(const Position& pos, int scoreWhite) {
    Sample s;
    memset(&s, 0, sizeof(s));
    s.occ = pos.occ();
    u64 b = s.occ;
    int i = 0;
    while (b) {
        int sq = poplsb(b);
        s.pcs[i >> 1] |= (u8)(pos.board[sq] << ((i & 1) * 4));
        i++;
    }
    s.score = (i16)std::clamp(scoreWhite, -32000, 32000);
    s.stm = (u8)pos.side;
    s.wksq = (u8)pos.kingSq(WHITE);
    s.bksq = (u8)pos.kingSq(BLACK);
    return s;
}

static void worker(int id, std::string prefix, u64 softNodes, u64 target, u64 seed, int randomPlies) {
    TT tt;
    tt.resize(16);
    Searcher* sr = new Searcher();
    sr->tt = &tt;
    sr->silent = true;
    u64 rs = seed * 0x9E3779B97F4A7C15ULL + (u64)id * 0xD1B54A32D192ED03ULL + 0x12345;
    auto rnd = [&]() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; };
    for (int i = 0; i < 20; i++) rnd();
    std::string fname = prefix + "_" + std::to_string(seed) + "_" + std::to_string(id) + ".bin";
    FILE* out = fopen(fname.c_str(), "ab");
    if (!out) { printf("cannot open %s\n", fname.c_str()); return; }
    std::vector<Sample> samples;
    std::vector<u64> keys, allKeys;

    while (!dgStop.load() && totalPositions.load() < target) {
        Position pos;
        pos.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        keys.clear(); allKeys.clear();
        keys.push_back(pos.key); allKeys.push_back(pos.key);
        bool ok = true;
        int rp = randomPlies + (int)(rnd() & 1);
        for (int i = 0; i < rp; i++) {
            MoveList ml;
            genLegal(pos, ml);
            if (ml.n == 0) { ok = false; break; }
            pos.makeMove(ml.moves[rnd() % ml.n]);
            if (pos.halfmove == 0) keys.clear();
            keys.push_back(pos.key); allKeys.push_back(pos.key);
        }
        if (!ok) continue;
        { MoveList ml; genLegal(pos, ml); if (ml.n == 0) continue; }
        samples.clear();
        sr->clearHistory();
        tt.clear();
        int result = -1;   // white-relative: 0 loss, 1 draw, 2 win
        int winCount = 0, drawCount = 0;
        for (int ply = 0; ply < 500; ply++) {
            MoveList ml;
            genLegal(pos, ml);
            if (ml.n == 0) { result = pos.checkers ? (pos.side == WHITE ? 0 : 2) : 1; break; }
            if (pos.halfmove >= 100 || pos.insufficientMaterial()) { result = 1; break; }
            int reps = 0;
            for (u64 k : keys) if (k == pos.key) reps++;
            if (reps >= 3) { result = 1; break; }

            Limits lim;
            lim.softNodes = softNodes; lim.hardNodes = softNodes * 8;
            sr->lim = lim;
            sr->stop = false;
            sr->setRootHistory(keys.data(), (int)keys.size());
            sr->startTime = std::chrono::steady_clock::now();
            sr->run(pos);
            Move bm = sr->bestMove;
            int score = sr->bestScore;
            if (bm == MOVE_NONE) { result = 1; break; }
            if (ply == 0 && std::abs(score) > 500) { ok = false; break; }   // unbalanced random opening
            int scoreWhite = pos.side == WHITE ? score : -score;
            if (std::abs(score) >= SCORE_MATE_IN_MAX) { result = scoreWhite > 0 ? 2 : 0; break; }
            if (std::abs(score) >= 2000) { if (++winCount >= 4) { result = scoreWhite > 0 ? 2 : 0; break; } } else winCount = 0;
            if (ply >= 60 && std::abs(score) <= 6) { if (++drawCount >= 12) { result = 1; break; } } else drawCount = 0;
            bool noisy = isCapture(bm) || isPromo(bm);
            // positions whose best move is a capture/promotion are written with flag bit 0 set, so the
            // trainer can include or exclude them; positions in check are never written
            if (!pos.checkers) {
                Sample smp = makeSample(pos, scoreWhite);
                smp.pad[0] = noisy ? 1 : 0;
                samples.push_back(smp);
            }
            pos.makeMove(bm);
            if (pos.halfmove == 0) keys.clear();
            keys.push_back(pos.key);
        }
        if (!ok) continue;
        if (result < 0) result = 1;
        for (auto& s : samples) s.result = (u8)result;
        fwrite(samples.data(), sizeof(Sample), samples.size(), out);
        fflush(out);
        totalPositions += samples.size();
        totalGames++;
    }
    fclose(out);
    delete sr;
}

int datagenMain(int argc, char** argv) {
    if (argc < 6) { printf("usage: datagen <threads> <out-prefix> <softNodes> <totalPositions> [seed] [randomPlies]\n"); return 1; }
    int threads = atoi(argv[2]);
    std::string prefix = argv[3];
    u64 softNodes = strtoull(argv[4], nullptr, 10);
    u64 target = strtoull(argv[5], nullptr, 10);
    u64 seed = argc > 6 ? strtoull(argv[6], nullptr, 10) : 1;
    int randomPlies = argc > 7 ? atoi(argv[7]) : 8;
    std::string stopFile = prefix.substr(0, prefix.find_last_of("/\\") + 1) + "STOP";
    std::vector<std::thread> th;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < threads; i++) th.emplace_back(worker, i, prefix, softNodes, target, seed, randomPlies);
    while (totalPositions.load() < target && !dgStop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("positions %llu games %llu pos/s %.0f\n", (unsigned long long)totalPositions.load(), (unsigned long long)totalGames.load(), totalPositions.load() / sec);
        fflush(stdout);
        std::ifstream f(stopFile);
        if (f.good()) dgStop = true;
    }
    for (auto& t : th) t.join();
    printf("done: positions %llu games %llu\n", (unsigned long long)totalPositions.load(), (unsigned long long)totalGames.load());
    return 0;
}

// debug: print the engine's NNUE evaluation of the first n samples of a data file
void evalSamples(const std::string& file, int n) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) { printf("cannot open %s\n", file.c_str()); return; }
    Searcher* sr = new Searcher();
    for (int i = 0; i < n; i++) {
        Sample s;
        if (fread(&s, sizeof(s), 1, f) != 1) break;
        Position pos;
        memset(&pos, 0, sizeof(pos));
        for (int q = 0; q < 64; q++) pos.board[q] = NO_PIECE;
        u64 b = s.occ; int k = 0;
        while (b) { int sq = poplsb(b); pos.put((s.pcs[k >> 1] >> ((k & 1) * 4)) & 15, sq); k++; }
        pos.side = s.stm; pos.ep = NO_SQ;
        pos.updateCheckInfo();
        sr->accStack[0].computed[0] = sr->accStack[0].computed[1] = false;
        sr->accStack[0].refresh[0] = sr->accStack[0].refresh[1] = true;
        printf("%d: stm %d score(white) %d result %d eval(stm) %d  %s\n", i, s.stm, s.score, s.result, sr->evaluate(pos, 0), pos.fen().c_str());
    }
    fclose(f);
    delete sr;
}