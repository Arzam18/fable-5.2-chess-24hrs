// Minimal SPSA tuner driving fastchess (written for this project).
// usage: spsa <fastchess.exe> <engine.exe> <book.pgn> <params.txt> <iterations> <pairsPerIter> <tc> <concurrency> [extra "-each" options]
// params.txt: one parameter per line:  name value min max c_end r_end
// State is rewritten to <params.txt>.out after every iteration (same format, current values).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <random>
#include <algorithm>

struct P { std::string name; double v, mn, mx, cEnd, rEnd; };

int main(int argc, char** argv) {
    if (argc < 9) { printf("usage: spsa fastchess engine book params iterations pairs tc concurrency [each-opts]\n"); return 1; }
    std::string fc = argv[1], eng = argv[2], book = argv[3], pfile = argv[4];
    int iters = atoi(argv[5]), pairs = atoi(argv[6]);
    std::string tc = argv[7];
    int conc = atoi(argv[8]);
    std::string eachExtra = argc > 9 ? argv[9] : "";
    int startIter = argc > 10 ? atoi(argv[10]) : 0;   // resume: keeps the a_k / c_k schedule position
    std::vector<P> ps;
    {
        std::ifstream in(pfile);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream is(line);
            P p;
            if (is >> p.name >> p.v >> p.mn >> p.mx >> p.cEnd >> p.rEnd) ps.push_back(p);
        }
    }
    printf("%zu parameters, %d iterations x %d pairs\n", ps.size(), iters, pairs);
    const double alpha = 0.602, gamma = 0.101;
    const double N = (double)iters * pairs, A = 0.1 * N;
    std::mt19937_64 rng(std::random_device{}());
    long long totalW = 0, totalL = 0, totalD = 0;
    for (int it = startIter; it < iters; it++) {
        double k = (double)(it + 1) * pairs;
        std::vector<int> delta(ps.size());
        std::string optPlus, optMinus;
        std::vector<double> ck(ps.size());
        for (size_t i = 0; i < ps.size(); i++) {
            delta[i] = (rng() & 1) ? 1 : -1;
            ck[i] = ps[i].cEnd * std::pow(N, gamma) / std::pow(k, gamma);
            double vp = std::clamp(ps[i].v + ck[i] * delta[i], ps[i].mn, ps[i].mx);
            double vm = std::clamp(ps[i].v - ck[i] * delta[i], ps[i].mn, ps[i].mx);
            optPlus += " option." + ps[i].name + "=" + std::to_string((long long)std::llround(vp));
            optMinus += " option." + ps[i].name + "=" + std::to_string((long long)std::llround(vm));
        }
        std::string cmd = "\"\"" + fc + "\" -engine cmd=\"" + eng + "\" name=plus" + optPlus + " -engine cmd=\"" + eng + "\" name=minus" + optMinus
            + " -each tc=" + tc + " " + eachExtra + " -openings file=\"" + book + "\" format=pgn order=random plies=16 -srand " + std::to_string(rng() % 1000000000ULL)
            + " -rounds " + std::to_string(pairs) + " -repeat -concurrency " + std::to_string(conc) + " -recover 2>&1\"";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) { printf("cannot run fastchess\n"); return 1; }
        char buf[4096];
        int W = 0, L = 0, D = 0;
        while (fgets(buf, sizeof(buf), pipe)) {
            int g, w, l, d;
            if (sscanf(buf, "Games: %d, Wins: %d, Losses: %d, Draws: %d", &g, &w, &l, &d) == 4) { W = w; L = l; D = d; }
        }
        _pclose(pipe);
        totalW += W; totalL += L; totalD += D;
        int result = W - L;
        for (size_t i = 0; i < ps.size(); i++) {
            double aEnd = ps[i].rEnd * ps[i].cEnd * ps[i].cEnd;
            double ak = aEnd * std::pow(A + N, alpha) / std::pow(A + k, alpha);
            ps[i].v = std::clamp(ps[i].v + ak / ck[i] * result * delta[i], ps[i].mn, ps[i].mx);
        }
        std::ofstream out(pfile + ".out");
        for (auto& p : ps) out << p.name << " " << p.v << " " << p.mn << " " << p.mx << " " << p.cEnd << " " << p.rEnd << "\n";
        out.close();
        printf("iter %d/%d: W %d L %d D %d (total %lld/%lld/%lld)\n", it + 1, iters, W, L, D, totalW, totalL, totalD);
        if ((it + 1) % 10 == 0) { for (auto& p : ps) printf("  %s=%.1f", p.name.c_str(), p.v); printf("\n"); }
        fflush(stdout);
        std::ifstream stopf(pfile + ".stop");
        if (stopf.good()) { printf("stop file found\n"); break; }
    }
    return 0;
}
