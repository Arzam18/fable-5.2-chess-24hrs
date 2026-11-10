#include "search.h"
#include "eval.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <vector>

#define ENGINE_NAME "Fable 5.2 chess 24hrs"
#define ENGINE_AUTHOR "Fable 5.2"

static const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static TT tt;
static Searcher* searcher;
static std::thread searchThread;
static Position rootPos;
static std::vector<u64> gameKeys;
static int moveOverhead = 25;

int datagenMain(int argc, char** argv);

static void stopSearch() {
    searcher->stop = true;
    if (searchThread.joinable()) searchThread.join();
}

static void setPosition(std::istringstream& is) {
    std::string tok, fen;
    is >> tok;
    if (tok == "startpos") { fen = START_FEN; is >> tok; }
    else if (tok == "fen") {
        while (is >> tok && tok != "moves") fen += tok + " ";
    } else return;
    rootPos.setFen(fen);
    gameKeys.clear();
    gameKeys.push_back(rootPos.key);
    if (tok == "moves") {
        while (is >> tok) {
            Move m = rootPos.parseMove(tok);
            if (m == MOVE_NONE) break;
            rootPos.makeMove(m);
            if (rootPos.halfmove == 0) gameKeys.clear();
            gameKeys.push_back(rootPos.key);
        }
    }
}

static void go(std::istringstream& is) {
    stopSearch();
    Limits lim;
    i64 wtime = -1, btime = -1, winc = 0, binc = 0, movetime = -1;
    int mtg = 0;
    std::string tok;
    while (is >> tok) {
        if (tok == "wtime") is >> wtime;
        else if (tok == "btime") is >> btime;
        else if (tok == "winc") is >> winc;
        else if (tok == "binc") is >> binc;
        else if (tok == "movestogo") is >> mtg;
        else if (tok == "movetime") is >> movetime;
        else if (tok == "depth") is >> lim.depth;
        else if (tok == "nodes") { is >> lim.hardNodes; lim.softNodes = lim.hardNodes; }
        else if (tok == "infinite") lim.infinite = true;
    }
    i64 time = rootPos.side == WHITE ? wtime : btime;
    i64 inc = rootPos.side == WHITE ? winc : binc;
    if (movetime >= 0) {
        lim.softTime = lim.hardTime = std::max<i64>(1, movetime - moveOverhead);
        lim.softTime = lim.hardTime * 10;   // use the whole movetime
    } else if (time >= 0 && !lim.infinite) {
        i64 t = std::max<i64>(1, time - moveOverhead);
        double soft, hard;
        if (mtg > 0) {
            int n = std::min(mtg, 40);
            soft = (double)t / (n + 1) * 0.9 + inc * 0.6;
            hard = std::min((double)t / (n > 1 ? 2.5 : 1.2), soft * 4.5);
        } else {
            soft = (double)t / 26.0 + inc * 0.6;
            hard = std::min((double)t * 0.3, soft * 4.5);
        }
        lim.softTime = std::max<i64>(1, std::min<i64>((i64)soft, t));
        lim.hardTime = std::max<i64>(1, std::min<i64>((i64)hard, t));
    }
    searcher->lim = lim;
    searcher->stop = false;
    searcher->silent = false;
    searcher->setRootHistory(gameKeys.data(), (int)gameKeys.size());
    searcher->startTime = std::chrono::steady_clock::now();
    Position p = rootPos;
    searchThread = std::thread([p]() { searcher->run(p); });
}

static void perftSuite(const std::string& file, int maxDepth) {
    std::ifstream in(file);
    std::string line;
    int n = 0, bad = 0;
    auto t0 = std::chrono::steady_clock::now();
    u64 total = 0;
    while (std::getline(in, line)) {
        size_t p = line.find(';');
        if (p == std::string::npos) continue;
        Position pos;
        pos.setFen(line.substr(0, p));
        std::istringstream is(line.substr(p));
        std::string tok;
        n++;
        while (is >> tok) {
            int d = atoi(tok.c_str() + 2);
            u64 expect; is >> expect;
            if (d > maxDepth) continue;
            u64 got = perft(pos, d);
            total += got;
            if (got != expect) { bad++; std::cout << "MISMATCH line " << n << " depth " << d << " got " << got << " expected " << expect << " : " << line << std::endl; }
        }
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "perft suite: " << n << " positions, depth<=" << maxDepth << ", mismatches " << bad << ", nodes " << total << ", ms " << ms << std::endl;
}

// test isPseudoLegal against generation on positions reached in random playouts
static void testPseudoLegal() {
    u64 seed = 12345, errors = 0, checked = 0;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    for (int g = 0; g < 2000; g++) {
        Position pos;
        pos.setFen(START_FEN);
        for (int ply = 0; ply < 200; ply++) {
            MoveList pl;
            genNoisy(pos, pl); genQuiet(pos, pl);
            static bool in[65536];
            memset(in, 0, sizeof(in));
            for (int i = 0; i < pl.n; i++) in[pl.moves[i]] = true;
            for (int m = 0; m < 65536; m++) {
                if (pos.isPseudoLegal((Move)m) != in[m]) {
                    errors++;
                    if (errors < 10) std::cout << "pseudo-legal mismatch: " << pos.fen() << " move " << moveStr((Move)m) << " flag " << moveFlag((Move)m) << " gen=" << in[m] << std::endl;
                }
            }
            checked++;
            MoveList ml;
            genLegal(pos, ml);
            if (ml.n == 0 || pos.halfmove >= 100) break;
            pos.makeMove(ml.moves[rnd() % ml.n]);
        }
    }
    std::cout << "pseudo-legal test: positions " << checked << " errors " << errors << std::endl;
}

static const char* benchFens[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r1bq1rk1/pp2ppbp/2np1np1/8/3NP3/2N1BP2/PPPQ2PP/R3KB1R w KQ - 3 9",
    "r2q1rk1/1b1nbppp/p2ppn2/1p6/3NPP2/1BN1B3/PPP1Q1PP/R4RK1 w - - 2 12",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "2r3k1/pp3pp1/4p2p/3pP3/3P1P2/1P2q3/P5PP/2R1Q1K1 w - - 0 25",
    "6k1/5pp1/7p/8/1p2P3/1P3P2/3r2PP/2R3K1 w - - 0 32",
    "r1bqk2r/pp1nbppp/2p1pn2/3p4/2PP4/2N1PN2/PP3PPP/R1BQKB1R w KQkq - 0 7",
    "3r1rk1/1bq1bppp/p3pn2/1p6/3BP3/P1N2Q2/1PP1B1PP/3R1R1K w - - 0 17",
    "8/8/4k3/3p1p2/3P1P2/4K3/8/8 w - - 0 1",
    "r1b2rk1/2q1bppp/p2p1n2/np2p3/3PP3/2P2N1P/PPBN1PP1/R1BQR1K1 b - - 0 13",
};

static void bench(int depth) {
    u64 total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const char* f : benchFens) {
        Position pos;
        pos.setFen(f);
        tt.clear();
        searcher->clearHistory();
        Limits lim; lim.depth = depth;
        searcher->lim = lim; searcher->stop = false; searcher->silent = true;
        searcher->setRootHistory(&pos.key, 1);
        searcher->startTime = std::chrono::steady_clock::now();
        searcher->run(pos);
        total += searcher->nodes;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << total << " nodes " << (total * 1000 / (u64)std::max<i64>(ms, 1)) << " nps" << std::endl;
}

int main(int argc, char** argv) {
    initAttacks();
    initZobrist();
    initEval();
    initSearch();
    if (argc > 1 && std::string(argv[1]) == "datagen") return datagenMain(argc, argv);
    searcher = new Searcher();
    tt.resize(16);
    searcher->tt = &tt;
    rootPos.setFen(START_FEN);
    gameKeys.push_back(rootPos.key);
    if (argc > 1 && std::string(argv[1]) == "bench") { bench(argc > 2 ? atoi(argv[2]) : 12); return 0; }

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd;
        if (!(is >> cmd)) continue;
        if (cmd == "uci") {
            std::cout << "id name " ENGINE_NAME "\nid author " ENGINE_AUTHOR "\n"
                      << "option name Hash type spin default 16 min 1 max 65536\n"
                      << "option name Threads type spin default 1 min 1 max 1\n"
                      << "option name Move Overhead type spin default 25 min 0 max 5000\n"
                      << "uciok" << std::endl;
        } else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (cmd == "ucinewgame") {
            stopSearch();
            tt.clear();
            searcher->clearHistory();
        } else if (cmd == "setoption") {
            std::string tok, name, value;
            is >> tok;
            while (is >> tok && tok != "value") name += (name.empty() ? "" : " ") + tok;
            while (is >> tok) value += (value.empty() ? "" : " ") + tok;
            for (auto& c : name) c = (char)tolower(c);
            if (name == "hash") { stopSearch(); int mb = atoi(value.c_str()); tt.resize(std::clamp(mb, 1, 65536)); }
            else if (name == "move overhead") moveOverhead = std::clamp(atoi(value.c_str()), 0, 5000);
        } else if (cmd == "position") {
            stopSearch();
            setPosition(is);
        } else if (cmd == "go") {
            go(is);
        } else if (cmd == "stop") {
            stopSearch();
        } else if (cmd == "quit") {
            break;
        } else if (cmd == "perft") {
            int d = 5; is >> d;
            auto t0 = std::chrono::steady_clock::now();
            u64 n = perft(rootPos, d);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "perft " << d << " = " << n << " (" << ms << " ms)" << std::endl;
        } else if (cmd == "perftsuite") {
            std::string f; int d = 5; is >> f >> d;
            perftSuite(f, d);
        } else if (cmd == "testpl") {
            testPseudoLegal();
        } else if (cmd == "bench") {
            int d = 12; is >> d; bench(d);
        } else if (cmd == "eval") {
            std::cout << "eval " << searcher->evaluate(rootPos) << std::endl;
        } else if (cmd == "d") {
            std::cout << rootPos.fen() << " key " << rootPos.key << std::endl;
        }
    }
    stopSearch();
    return 0;
}
