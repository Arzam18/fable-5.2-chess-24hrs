// NNUE trainer, written from scratch (standard library only, CPU, multithreaded).
// Network: (768 -> H) x 2 perspectives, SCReLU, -> 1 output per piece-count bucket (8 buckets).
// Input features are horizontally mirrored so that the perspective's own king is on files a-d.
//
// usage: trainer --out net.bin --hidden 256 --epochs 30 --lr 0.001 --lr-final 0.00003 --lambda 0.5
//                --threads 12 --batch 16384 [--init net.f32] [--max-pos N] file1.bin file2.bin ...
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <chrono>
#include <algorithm>
#include "../src/sample.h"

typedef uint64_t u64; typedef uint32_t u32;

static int H = 256;
static const int NB = 8;
static const int NIN = 768;
static const float SCALE = 400.0f;
static const int QA = 255, QB = 64;

struct Pool {
    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable cvStart, cvDone;
    int gen = 0, done = 0, T;
    bool quit = false;
    std::function<void(int)> job;
    Pool(int t) : T(t) {
        for (int i = 0; i < T; i++) th.emplace_back([this, i]() {
            int seen = 0;
            while (true) {
                {
                    std::unique_lock<std::mutex> lk(m);
                    cvStart.wait(lk, [&] { return gen != seen || quit; });
                    if (quit) return;
                    seen = gen;
                }
                job(i);
                {
                    std::lock_guard<std::mutex> lk(m);
                    done++;
                }
                cvDone.notify_one();
            }
        });
    }
    void run(std::function<void(int)> f) {
        {
            std::lock_guard<std::mutex> lk(m);
            job = f; gen++; done = 0;
        }
        cvStart.notify_all();
        std::unique_lock<std::mutex> lk(m);
        cvDone.wait(lk, [&] { return done == T; });
    }
    ~Pool() {
        { std::lock_guard<std::mutex> lk(m); quit = true; }
        cvStart.notify_all();
        for (auto& t : th) t.join();
    }
};

static size_t oW0, ob0, oW1, ob1, NP;

static inline int extract(const Sample& s, int* fS, int* fN) {
    int n = 0;
    int mirW = (s.wksq & 7) >= 4 ? 7 : 0, mirB = (s.bksq & 7) >= 4 ? 7 : 0;
    u64 occ = s.occ;
    while (occ) {
        int sq = __builtin_ctzll(occ);
        occ &= occ - 1;
        int pc = (s.pcs[n >> 1] >> ((n & 1) * 4)) & 15;
        int c = pc >= 6, pt = pc - 6 * c;
        int fw = c * 384 + pt * 64 + (sq ^ mirW);
        int fb = (1 - c) * 384 + pt * 64 + (sq ^ 56 ^ mirB);
        if (s.stm == 0) { fS[n] = fw; fN[n] = fb; } else { fS[n] = fb; fN[n] = fw; }
        n++;
    }
    return n;
}

static void processRange(const Sample* data, const u32* idx, size_t lo, size_t hi, const float* P, float* G, double& lossSum, float lambda, bool train) {
    const int MAXH = 2048;
    alignas(32) static thread_local float accS[MAXH], accN[MAXH], aS[MAXH], aN[MAXH], dS[MAXH], dN[MAXH];
    int fS[32], fN[32];
    const float* W0 = P + oW0; const float* b0 = P + ob0; const float* W1 = P + oW1; const float* b1 = P + ob1;
    float* gW0 = G + oW0; float* gb0 = G + ob0; float* gW1 = G + oW1; float* gb1 = G + ob1;
    double loss = 0;
    for (size_t i = lo; i < hi; i++) {
        const Sample& s = data[idx[i]];
        int n = extract(s, fS, fN);
        int bucket = (n - 2) / 4;
        if (bucket > 7) bucket = 7;
        memcpy(accS, b0, H * sizeof(float));
        memcpy(accN, b0, H * sizeof(float));
        for (int j = 0; j < n; j++) {
            const float* r1 = W0 + (size_t)fS[j] * H;
            const float* r2 = W0 + (size_t)fN[j] * H;
            for (int h = 0; h < H; h++) { accS[h] += r1[h]; accN[h] += r2[h]; }
        }
        const float* w = W1 + (size_t)bucket * 2 * H;
        float out = b1[bucket];
        for (int h = 0; h < H; h++) {
            float c1 = std::min(std::max(accS[h], 0.0f), 1.0f);
            float c2 = std::min(std::max(accN[h], 0.0f), 1.0f);
            aS[h] = c1 * c1; aN[h] = c2 * c2;
            out += aS[h] * w[h] + aN[h] * w[H + h];
            dS[h] = (accS[h] > 0.0f && accS[h] < 1.0f) ? 2.0f * c1 : 0.0f;
            dN[h] = (accN[h] > 0.0f && accN[h] < 1.0f) ? 2.0f * c2 : 0.0f;
        }
        float p = 1.0f / (1.0f + std::exp(-out));
        float sc = s.stm == 0 ? (float)s.score : -(float)s.score;
        float res = s.stm == 0 ? s.result * 0.5f : 1.0f - s.result * 0.5f;
        float t = lambda / (1.0f + std::exp(-sc / SCALE)) + (1.0f - lambda) * res;
        float err = p - t;
        loss += (double)err * err;
        if (!train) continue;
        float dOut = 2.0f * err * p * (1.0f - p);
        gb1[bucket] += dOut;
        float* gw = gW1 + (size_t)bucket * 2 * H;
        for (int h = 0; h < H; h++) {
            gw[h] += dOut * aS[h];
            gw[H + h] += dOut * aN[h];
            dS[h] *= dOut * w[h];
            dN[h] *= dOut * w[H + h];
            gb0[h] += dS[h] + dN[h];
        }
        for (int j = 0; j < n; j++) {
            float* r1 = gW0 + (size_t)fS[j] * H;
            float* r2 = gW0 + (size_t)fN[j] * H;
            for (int h = 0; h < H; h++) { r1[h] += dS[h]; r2[h] += dN[h]; }
        }
    }
    lossSum += loss;
}

static void saveQuantized(const std::string& path, const std::vector<float>& P) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", path.c_str()); return; }
    int32_t hdr[4] = {0x4E4E5546, H, NB, 0};
    fwrite(hdr, 4, 4, f);
    std::vector<int16_t> q;
    int clipped = 0;
    auto q16 = [&](float v, float s) { float r = std::round(v * s); if (r > 32767) { r = 32767; clipped++; } if (r < -32768) { r = -32768; clipped++; } return (int16_t)r; };
    for (size_t i = 0; i < (size_t)NIN * H; i++) q.push_back(q16(P[oW0 + i], QA));
    for (int i = 0; i < H; i++) q.push_back(q16(P[ob0 + i], QA));
    for (int i = 0; i < NB * 2 * H; i++) q.push_back(q16(P[oW1 + i], QB));
    fwrite(q.data(), 2, q.size(), f);
    int32_t b[NB];
    for (int i = 0; i < NB; i++) b[i] = (int32_t)std::round(P[ob1 + i] * QA * QB);
    fwrite(b, 4, NB, f);
    fclose(f);
    if (clipped) printf("warning: %d weights clipped in quantization\n", clipped);
}

int main(int argc, char** argv) {
    std::string out = "net.bin", init;
    int epochs = 30, threads = 12, batch = 16384;
    float lr = 0.001f, lrFinal = 0.00003f, lambda = 0.5f;
    size_t maxPos = 0;
    int dump = 0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto nxt = [&]() { return std::string(argv[++i]); };
        if (a == "--out") out = nxt();
        else if (a == "--hidden") H = atoi(nxt().c_str());
        else if (a == "--epochs") epochs = atoi(nxt().c_str());
        else if (a == "--lr") lr = (float)atof(nxt().c_str());
        else if (a == "--lr-final") lrFinal = (float)atof(nxt().c_str());
        else if (a == "--lambda") lambda = (float)atof(nxt().c_str());
        else if (a == "--threads") threads = atoi(nxt().c_str());
        else if (a == "--batch") batch = atoi(nxt().c_str());
        else if (a == "--init") init = nxt();
        else if (a == "--max-pos") maxPos = strtoull(nxt().c_str(), nullptr, 10);
        else if (a == "--dump") dump = atoi(nxt().c_str());
        else files.push_back(a);
    }
    // load data
    std::vector<Sample> data;
    for (auto& fn : files) {
        FILE* f = fopen(fn.c_str(), "rb");
        if (!f) { printf("cannot open %s\n", fn.c_str()); continue; }
        _fseeki64(f, 0, SEEK_END);
        long long sz = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        size_t n = (size_t)(sz / sizeof(Sample));
        size_t old = data.size();
        data.resize(old + n);
        size_t got = fread(data.data() + old, sizeof(Sample), n, f);
        data.resize(old + got);
        fclose(f);
    }
    if (maxPos && data.size() > maxPos) data.resize(maxPos);
    size_t N = data.size();
    printf("loaded %zu positions from %zu files\n", N, files.size());
    if (dump) {
        double wdl[3] = {0, 0, 0}; double absSum = 0;
        for (size_t i = 0; i < N; i++) { wdl[data[i].result]++; absSum += std::abs(data[i].score); }
        printf("results: black %.3f draw %.3f white %.3f, mean |score| %.1f\n", wdl[0] / N, wdl[1] / N, wdl[2] / N, absSum / N);
        oW0 = 0; ob0 = (size_t)NIN * H; oW1 = ob0 + H; ob1 = oW1 + (size_t)NB * 2 * H; NP = ob1 + NB;
        std::vector<float> P(NP, 0.0f);
        if (!init.empty()) { FILE* f = fopen(init.c_str(), "rb"); if (f) { fread(P.data(), sizeof(float), NP, f); fclose(f); } }
        for (int i = 0; i < dump && (size_t)i < N; i++) {
            const Sample& s = data[i];
            int fS[32], fN[32];
            int n = extract(s, fS, fN);
            int bucket = std::min((n - 2) / 4, 7);
            float out = P[ob1 + bucket];
            for (int h = 0; h < H; h++) {
                float a1 = P[ob0 + h], a2 = P[ob0 + h];
                for (int j = 0; j < n; j++) { a1 += P[oW0 + (size_t)fS[j] * H + h]; a2 += P[oW0 + (size_t)fN[j] * H + h]; }
                a1 = std::min(std::max(a1, 0.0f), 1.0f); a2 = std::min(std::max(a2, 0.0f), 1.0f);
                out += a1 * a1 * P[oW1 + (size_t)bucket * 2 * H + h] + a2 * a2 * P[oW1 + (size_t)bucket * 2 * H + H + h];
            }
            printf("%d: pieces %d stm %d score %d result %d net(stm) %.1f\n", i, n, s.stm, s.score, s.result, out * SCALE);
        }
        return 0;
    }
    if (N < 1000) { printf("not enough data\n"); return 1; }

    oW0 = 0; ob0 = (size_t)NIN * H; oW1 = ob0 + H; ob1 = oW1 + (size_t)NB * 2 * H; NP = ob1 + NB;
    std::vector<float> P(NP, 0.0f), M(NP, 0.0f), V(NP, 0.0f), Gt(NP, 0.0f);
    u64 rs = 0x1234567ULL;
    auto rnd = [&]() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; };
    auto uni = [&](float a) { return ((rnd() >> 11) * (1.0f / 9007199254740992.0f) * 2.0f - 1.0f) * a; };
    for (size_t i = 0; i < (size_t)NIN * H; i++) P[oW0 + i] = uni(0.1f);
    for (int i = 0; i < NB * 2 * H; i++) P[oW1 + i] = uni(1.0f / std::sqrt((float)(2 * H)));
    if (!init.empty()) {
        FILE* f = fopen(init.c_str(), "rb");
        if (f) { size_t g = fread(P.data(), sizeof(float), NP, f); fclose(f); printf("init from %s (%zu floats)\n", init.c_str(), g); }
    }

    size_t nVal = std::min<size_t>(N / 50, 500000);
    size_t nTrain = N - nVal;
    std::vector<u32> idx(N);
    for (size_t i = 0; i < N; i++) idx[i] = (u32)i;
    // shuffle everything once so validation is a random subset
    for (size_t i = N - 1; i > 0; i--) { size_t j = rnd() % (i + 1); std::swap(idx[i], idx[j]); }

    Pool pool(threads);
    std::vector<std::vector<float>> G(threads, std::vector<float>(NP, 0.0f));
    std::vector<double> losses(threads, 0.0);
    const float b1c = 0.9f, b2c = 0.999f, eps = 1e-8f;
    u64 step = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int ep = 0; ep < epochs; ep++) {
        float curLr = epochs > 1 ? lr * std::pow(lrFinal / lr, (float)ep / (float)(epochs - 1)) : lr;
        for (size_t i = nTrain - 1; i > 0; i--) { size_t j = rnd() % (i + 1); std::swap(idx[i], idx[j]); }
        double epLoss = 0; size_t epCount = 0;
        for (size_t start = 0; start + batch <= nTrain; start += batch) {
            pool.run([&](int t) {
                size_t lo = start + (size_t)batch * t / threads, hi = start + (size_t)batch * (t + 1) / threads;
                processRange(data.data(), idx.data(), lo, hi, P.data(), G[t].data(), losses[t], lambda, true);
            });
            // reduce gradients (parallel over parameter ranges)
            pool.run([&](int t) {
                size_t lo = NP * t / threads, hi = NP * (t + 1) / threads;
                float* gt = Gt.data();
                for (size_t k = lo; k < hi; k++) gt[k] = 0.0f;
                for (int u = 0; u < threads; u++) {
                    float* g = G[u].data();
                    for (size_t k = lo; k < hi; k++) { gt[k] += g[k]; g[k] = 0.0f; }
                }
                {
                    const float inv = 1.0f / batch;
                    const float c1 = 1.0f - std::pow(b1c, (float)(step + 1)), c2 = 1.0f - std::pow(b2c, (float)(step + 1));
                    float* p = P.data(); float* m = M.data(); float* v = V.data();
                    for (size_t k = lo; k < hi; k++) {
                        float g = gt[k] * inv;
                        m[k] = b1c * m[k] + (1.0f - b1c) * g;
                        v[k] = b2c * v[k] + (1.0f - b2c) * g * g;
                        p[k] -= curLr * (m[k] / c1) / (std::sqrt(v[k] / c2) + eps);
                    }
                    // clip output weights so they fit the quantized range
                    size_t a = std::max(lo, oW1), b = std::min(hi, ob1);
                    for (size_t k = a; k < b; k++) p[k] = std::min(std::max(p[k], -1.98f), 1.98f);
                }
            });
            step++;
            for (int t = 0; t < threads; t++) { epLoss += losses[t]; losses[t] = 0; }
            epCount += batch;
        }
        // validation
        double vloss = 0;
        pool.run([&](int t) {
            size_t lo = nTrain + nVal * t / threads, hi = nTrain + nVal * (t + 1) / threads;
            processRange(data.data(), idx.data(), lo, hi, P.data(), G[t].data(), losses[t], lambda, false);
        });
        for (int t = 0; t < threads; t++) { vloss += losses[t]; losses[t] = 0; }
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("epoch %d lr %.6f train loss %.6f val loss %.6f  (%.0f s, %.0f pos/s)\n", ep + 1, curLr, epLoss / epCount, vloss / nVal, sec, (double)epCount * (ep + 1) / sec);
        fflush(stdout);
        saveQuantized(out, P);
        FILE* f = fopen((out + ".f32").c_str(), "wb");
        if (f) { fwrite(P.data(), sizeof(float), NP, f); fclose(f); }
    }
    return 0;
}
