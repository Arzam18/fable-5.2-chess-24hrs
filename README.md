# Fable 5.2 chess 24hrs

A UCI chess engine designed, written, trained, tuned and tested from scratch by one AI model
(**Fable 5.2**, Anthropic) in a single autonomous 24-hour session, with nobody answering questions.
This file is the model's own write-up of what it built and how the time was spent.

## What this is

The *24-Hour Chess Engine Benchmark* gives a model an empty project, a C/C++ toolchain and 24 hours of
wall-clock time. The only thing scored is the playing strength (Elo) of the executable left in `final/`,
rated at **10 s + 0.1 s per move**, single-threaded, 256 MB hash, against a pool of other engines.
The full rules are in [CLAUDE.md](CLAUDE.md). In short:

- everything written in the session; no source code from existing engines, no pre-existing network,
  book or tablebase files; published documentation, formulas and tables are allowed;
- C or C++ with GCC (MinGW-w64), standard library and compiler intrinsics only;
- target: x86-64-v3 (AVX2, BMI2/PEXT), static standalone Windows executable;
- up to (physical cores − 2) = 14 cores for testing on the development machine (Core Ultra 9 285H);
- a loss on time, a crash or an illegal move is a lost game.

The run started 2026-11-09 15:28 and the last change to `final/` was made at 14:36 the next day.
The hour-by-hour log is in [docs/progress.md](docs/progress.md). No internet resource was consulted
([docs/resources.md](docs/resources.md)).

## The engine

| | |
|---|---|
| Name (`id name`) | `Fable 5.2 chess 24hrs` |
| Author (`id author`) | `Fable 5.2` |
| Executable | `final/Fable52chess24hrs.exe` (5.3 MB, imports only `KERNEL32.dll` and `msvcrt.dll`) |
| Language | C++20, about 2,200 lines for the engine, 370 for the network trainer, 80 for the SPSA tuner |
| Official rating | **3542 ± 15** on the CCRL Blitz scale at 10+0.1 (2,220 games; see [Official results](#official-results)) |

C++ was chosen over C for `std::thread`, `std::atomic` and `std::string` at no cost in generated code;
the engine uses nothing beyond the standard library and `<immintrin.h>`.

### Build

Compiler: `g++ (GCC) 15.2.0`, MinGW-w64. From a `cmd` prompt in the project root:

```
set NNKB=5
source\build.bat Fable52chess24hrs nets\k1_256.nnue
copy /y source\build\Fable52chess24hrs.exe final\
```

`build.bat` copies the network to `source\src\net.bin` (embedded with `.incbin`) and runs, inside `source\src`:

```
g++ -std=c++20 -O3 -march=x86-64-v3 -flto -static -Wl,--stack,8388608 -DNDEBUG -Wall
    -Wno-unused-but-set-variable -DNN_KB=5
    -o ..\build\Fable52chess24hrs.exe attacks.cpp position.cpp eval.cpp nnue.cpp search.cpp uci.cpp datagen.cpp
```

A rebuild gives the same search signature as the delivered binary: `Fable52chess24hrs.exe bench 12`
prints `465614 nodes`. Adding `-DTUNE` exposes every search parameter as a UCI spin option.

The other tools:

```
g++ -std=c++20 -O3 -march=x86-64-v3 -static -o source\build\trainer.exe source\trainer\trainer.cpp
g++ -std=c++20 -O2 -static -o source\build\spsa.exe source\tools\spsa.cpp
```

### Run

It is a normal UCI engine: point a GUI or fastchess at the executable. Extra console commands:
`bench [depth]`, `perft <depth>`, `perftsuite <file> <depth>`, `eval`, `d`, and the command-line mode
`Fable52chess24hrs.exe datagen <threads> <out-prefix> <softNodes> <positions> <seed>` used to generate
training data.

### UCI options

| Option | Default | Meaning |
|---|---|---|
| `Hash` | 16 | transposition table size in MB (1–65536) |
| `Threads` | 1 | fixed at 1; search is single-threaded |
| `Move Overhead` | 25 | milliseconds held back from the clock on every move |
| `EvalFile` | `<embedded>` | load a different network file of the same architecture |

## Architecture and features

**Board and move generation.** Bitboards (six piece boards, two colour boards) plus a 64-byte mailbox;
copy-make; 16-bit moves; PEXT slider attacks; Zobrist keys for the position, the pawns and each side's
non-pawn material. Generation is pseudo-legal and staged (captures/queen promotions, then quiets), with
legality decided from pinned pieces and checkers before a move is made. A separate `isPseudoLegal`
validates hash and killer moves; it was checked against the generator for all 65,536 move encodings on
380,000 random-playout positions. The generator matches all 126 positions of the supplied perft suite to
depth 6 (12.8 billion nodes).

**Search.** Iterative deepening with aspiration windows; principal variation search; transposition table
with three 10-byte entries per 32-byte cluster and ageing; quiescence search with hash cut-offs, SEE and
futility pruning; null-move pruning with verification at high depth; reverse futility pruning; razoring;
ProbCut; internal iterative reductions; late move reductions from a logarithmic table with fractional
adjustments (improving, cut node, PV, hash-PV, check, killer, noisy hash move, cut-off count, history);
late move pruning; futility, history and SEE pruning of quiets; futility and SEE pruning of captures;
singular extensions with double extension, multi-cut and negative extension; mate distance pruning;
repetition, 50-move and insufficient-material detection.

**Move ordering and learning tables.** Hash move, captures by victim value plus capture history with SEE
split into good and bad, one killer, quiets by threat-indexed butterfly history plus continuation history
(1, 2 and 4 plies back) plus pawn-structure history. Static-evaluation correction history keyed by pawn
structure, each side's non-pawn material and the previous two moves. The static-evaluation difference
across a move also feeds the quiet history.

**Evaluation.** An NNUE trained in this session on the engine's own games:
(5 king buckets × 768 inputs → 256) × 2 perspectives → 1 output in each of 8 piece-count buckets, SCReLU
activation. Inputs are mirrored horizontally so the perspective's king is on files a–d. Accumulators are
`int16` (scale 1020), updated lazily and incrementally, refreshed when the king changes bucket or half;
the output layer is AVX2 with a 12-bit squared activation and weights scaled by 1024. A small tapered
PeSTO-table evaluation remains in the source only as the bootstrap used for the first training data.

**Training pipeline.** `datagen` plays self-play games from 8–9 random opening plies at a soft node
limit, adjudicates decided and dead-drawn games, and writes 32-byte records (position, search score,
result). `trainer.cpp` is a from-scratch multithreaded CPU trainer: sparse forward/backward pass, Adam,
exponential learning-rate decay, optional warm start (including expanding a net into king buckets),
validation on held-out whole games, and an emulation of the engine's integer arithmetic to check
quantization. About 175 million quiet positions were generated (5.9 GB).

**Parameter tuning.** `spsa.cpp` drives fastchess: each iteration plays a mini-match between two
perturbed parameter sets at a fixed node count and nudges all parameters at once. 55 search parameters
were tuned over roughly 24,000 games.

**Time management and UCI.** Soft limit = clock/26 + 85% of the increment, scaled by the share of nodes
spent on the best move, by best-move stability and by a falling score; hard limit = min(30% of the
clock, 4.5 × soft). 25 ms is held back per move. The thread that reads `go` runs the search itself while
a second thread takes over stdin, so no thread creation or wake-up sits between `go` and the first node;
`stop`, `isready` and `quit` are answered during the search.

**Deliberately left out.** Multi-threaded search, pondering, MultiPV, `searchmoves`, `go mate`,
Chess960, opening book and tablebases (not required or not allowed). A wider network was tried three
times and never paid for its speed at this data size. Profile-guided optimisation was measured and gave
nothing.

## How the time was spent

| Hours | Work |
|---|---|
| 0–1 | Whole engine written in one pass and perft-exact on the first build; plays under fastchess; first build in `final/` after 15 minutes. Data generator, trainer and NNUE inference written. |
| 1–2 | First nets from PeSTO-labelled games; UCI loop rewritten after measuring up to 25 ms of thread start latency under load; tunable parameters, SPSA tool, PGO script. |
| 2–4 | Quantization error found and fixed (16.8 → 1.9 cp RMS). SPSA round 1 started. Width experiments exposed a leaking validation split. |
| 4–5 | Net tournament showed that blending game results into the target cost over 200 Elo; switched to pure search-score targets. |
| 5–9 | Data generation at ~18–25 M positions/hour with periodic warm-start retraining (nets 5–8). |
| 9–11 | SPSA round 1 validated (+52) and adopted; 55 parameters made tunable. |
| 11–14 | The 256-wide net stopped improving with data; 512-wide did not help; king input buckets did (+60). |
| 14–20 | Eval plateaued, so data generation was stopped and the CPU moved to SPSA round 2 (+56) and time-management round robins. |
| 20–23 | Confirmation matches at the real time control, final selection, standalone and compliance checks. |

**Measured by matches:** every network generation, network width (three times), the result/score blend,
removing the PeSTO-labelled data, including capture positions in training, king buckets (5 and 10),
fine-tuning on recent data, both SPSA rounds (at fixed nodes and at 10+0.1), time-management settings
(five round robins), and every build that went into `final/`.

**Accepted on reasoning:** the finer quantization (verified numerically against the trainer), the UCI
threading model (latency measured directly), the 8 MB stack, the first bundle of search features
(+22 ± 37 only), falling-score time scaling (about +10 ± 20 in two round robins), warm-start retraining.

**What went wrong.**

- The first training recipe gave half the weight to the game result. With only a few hundred thousand
  games that label is mostly memorisable noise; a four-net tournament showed pure score targets were
  ~245 Elo better. Roughly three hours of data generation ran with the weaker nets before this was found.
- My first validation split was per position, so positions from the same game sat on both sides and a
  wider net looked better while playing 64 Elo worse. Validation now holds out whole games, and network
  questions were decided by fixed-node matches from then on.
- The standard 255/64 quantization was too coarse for the small weights of a lightly trained net.
- A training run was killed by a tool timeout and resumed from its checkpoint; long jobs were run in the
  background afterwards.
- Running SPSA on 14 cores was slower than on 8 because of the laptop's two low-power cores.
- A gauntlet run on the slowest cores while other jobs held the fast ones was useless as a strength
  measurement because the opponent lost a third of its games on time (my engine lost none).
- The script that copied SPSA values into the source skipped two parameters whose names matched a filter
  meant for something else. It was found while compiling the final parameter list, fixed, and the
  corrected build was matched against the previous one before it replaced it.
- Combined changes that looked worth +18 and +39 separately measured −1 ± 16 at the real time control in
  the first 500 games; it took 1,300 games to settle on +7 ± 10.

**What was cut.** Further data generation after hour 14, wider networks, more king buckets, a second
hidden layer, PGO, any search feature that could not be expressed as a tunable parameter.

## Elo by hour

Estimate of the build in `final/` at each hour, on the ladder given by the supplied Stash versions
(CCRL Blitz ratings), from the entries in `docs/progress.md`.

| Hour | Time | Estimated Elo | Basis |
|---|---|---|---|
| 0 | 11-09 15:28 | not yet measurable | start |
| 1 | 16:28 | ~3130 | PeSTO eval: 75% vs Stash 25, 100 games |
| 2 | 17:28 | ~3240 | first NNUE: 60% vs Stash 30, 100 games |
| 3 | 18:28 | ~3240 | unchanged |
| 4 | 19:28 | ~3240 | unchanged |
| 5 | 20:28 | ~3445 | score-only training: 53% vs Stash 37, 100 games |
| 6 | 21:28 | ~3475 | net 6: 57.5% vs Stash 37, 100 games |
| 7 | 22:28 | ~3500 | net 7: +26 vs net 6 at fixed nodes |
| 8 | 23:28 | ~3500 | unchanged |
| 9 | 11-10 00:28 | ~3515 | net 8: 63% vs Stash 37, 100 games |
| 10 | 01:28 | ~3530 | SPSA round 1: 65% vs Stash 37, 100 games |
| 11 | 02:28 | ~3530 | unchanged |
| 12 | 03:28 | ~3530 | unchanged |
| 13 | 04:28 | ~3530 | unchanged (king-bucket build installed 04:38) |
| 14 | 05:28 | ~3600 | king buckets: 73.5% vs Stash 37, 100 games on a loaded machine |
| 15 | 06:28 | ~3600 | unchanged |
| 16 | 07:28 | ~3600 | unchanged |
| 17 | 08:28 | ~3600 | unchanged |
| 18 | 09:28 | ~3640 | SPSA round 2: +38 vs previous build, 400 games (self-play) |
| 19 | 10:28 | ~3640 | unchanged |
| 20 | 11:28 | ~3640 | unchanged |
| 21 | 12:28 | ~3640 | unchanged |
| 22 | 13:28 | ~3640 | v13 installed 13:31 (+7 ± 10 vs v12, 1,300 games) |
| 23 | 14:28 | ~3585 | clean re-measurement: 71.7% vs Stash 37 over 500 games |
| 24 | 15:28 | ~3590 | final entry (installed 14:36): 72.5% vs Stash 37 pooled over 700 games |

The drop at hour 23 is a better measurement, not a weaker engine: the 3640 figures added self-play gains
to an earlier 100-game result taken on a loaded machine.

## Assumptions

- "Standard library only" was applied to everything, including the trainer and the tuner: no ML
  framework, no GPU code.
- The 14-core limit was applied to all heavy work together (matches, data generation, training, tuning),
  not only to fastchess concurrency.
- Published tables may be reproduced: the bootstrap evaluation uses the PeSTO piece-square tables, typed
  from memory. Everything else was derived in the session; the final evaluation contains no PeSTO values.
- The rating harness may use no time margin at all, so the engine keeps 25 ms in hand and never plans to
  use more than 30% of its clock on one move. It lost no game on time in several thousand games at
  10+0.1, including runs on a heavily oversubscribed machine.
- Training data is kept under `source/data/` and is excluded from version control by the supplied
  `.gitignore`; the final network is kept in `source/nets/` so the build can be reproduced.
- The README may be written once the final build is installed and no further change is intended; it was
  written in the last 40 minutes, after all testing had stopped.
- Matches at a fixed 100,000 nodes per move (fewer than a typical 10+0.1 move gets) are an acceptable
  proxy for comparing networks and search parameters, and are immune to machine load; results that
  mattered were re-checked at 10+0.1.
- The Stash versions' CCRL Blitz ratings transfer to 10+0.1 on this machine well enough for a ladder
  estimate; no other calibration was available.

## Estimated strength

**About 3590 on the CCRL-Blitz scale implied by the Stash ladder, plausibly 3550–3630.**

Evidence, all at 10+0.1 with 256 MB hash, the UHO book and both colours per opening:

| Match | Games | Score | Elo difference |
|---|---|---|---|
| final entry (v14) vs Stash 37 (3424) | 200 | 74.5% | +186 ± 37 |
| previous build (v13, differs in two pruning margins) vs Stash 37 | 500 | 71.7% | +162 ± 25 |
| both pooled | 700 | 72.5% | +168 ± 21 |
| v13 gauntlet vs Stash 20, 21, 25, 30, 33, 37 | 240 | 91.0% | 40–0, 40–0, 97.5%, 87.5%, 92.5%, 68.8% |
| v14 vs v13 | 600 | 50.5% | +3.5 ± 15 |

The statistical error is about ±21 on the pooled figure. On top of that: every opponent in these matches
is a version of one engine family, most of the difference from the anchor is extrapolated from a 72%
score, CCRL ratings are measured at a longer time control on different hardware, and gains measured in
self-play (which is how most changes here were selected) usually shrink against unrelated engines. The
official result could reasonably land 50 Elo or more away from this estimate in either direction.

## Official results

**3542 ± 15 Elo (95% CI) on the CCRL Blitz scale**, from 2,220 games played on 10 November 2026, after
the run. That is 81 Elo above the next engine built under the same benchmark and the highest of the
nine rated so far.

The engine had no time loss, disconnect, illegal move or fastchess warning in any of the 2,220 games.

### Conditions

fastchess 1.8.2, 10 s + 0.1 s, `timemargin=200`, one thread, 64 MB hash, UHO openings (8 plies, both
colours), 12 concurrent games on an Intel Core Ultra 9 285H. (The benchmark rules announced 256 MB
hash; the rating run used 64 MB for every engine.) Games were adjudicated: resign after 3
moves at ±600 cp from both engines, draw after move 40 with 8 moves inside ±20 cp, 250 moves maximum.
Ratings come from an anchored maximum-likelihood fit in which the reference engines are fixed at their
CCRL Blitz ratings and every game involves at least one of the AI-written engines.

### All engines from this benchmark

| Engine | Elo | 95% CI | Games |
|---|---:|:---:|---:|
| **Fable 5.2 chess 24hrs** | **3542** | ±15 | 2220 |
| Opus 5.5 chess 24hrs | 3461 | ±15 | 2340 |
| Sonnet 5.5 chess 24hrs | 3415 | ±15 | 2340 |
| Fable 5.1 chess 24hrs | 3254 | ±19 | 1680 |
| Opus 5 chess 24hrs | 3228 | ±19 | 1580 |
| Astra 6 chess 24hrs | 3143 | ±19 | 1520 |
| Fable 5 chess 24hrs | 3045 | ±21 | 1300 |
| Haiku 5.5 chess 24hrs | 2765 | ±24 | 1060 |
| Sonnet 5 chess 24hrs | 2703 | ±23 | 1360 |

### Head-to-head

| Opponent | CCRL Blitz | +W | =D | −L | Score | Implied Elo |
|---|---:|---:|---:|---:|---:|---:|
| Coda 0.9.4 | 3638 | 1 | 41 | 118 | 13.4% | 3314 |
| Clover 9.0 | 3620 | 2 | 50 | 108 | 16.9% | 3343 |
| Elixir | 3567 | 24 | 85 | 51 | 41.6% | 3508 |
| Patricia 5.0 | 3539 | 65 | 69 | 66 | 49.8% | 3537 |
| Sirius 9.0 | 3528 | 75 | 72 | 53 | 55.5% | 3566 |
| Carp 3.0.1 | 3525 | 57 | 71 | 32 | 57.8% | 3580 |
| Tucano 12.00 | 3491 | 71 | 101 | 28 | 60.8% | 3567 |
| Leorik 3.2.1 | 3490 | 55 | 70 | 35 | 56.2% | 3534 |
| Marvin 6.3.0 | 3458 | 95 | 78 | 27 | 67.0% | 3581 |
| Stash 37 | 3424 | 127 | 62 | 11 | 79.0% | 3654 |
| Opus 5.5 chess 24hrs | (3461) | 59 | 89 | 12 | 64.7% | – |
| Sonnet 5.5 chess 24hrs | (3415) | 91 | 59 | 10 | 75.3% | – |
| Fable 5.1 chess 24hrs | (3254) | 87 | 10 | 3 | 92.0% | – |

The single win against Coda is a game in which Coda stopped responding; it is worth under 1 Elo in the fit.

### How firm the number is

The figure depends on which reference engines are trusted. Clover and Coda (and Motor, used for an
earlier engine) play roughly 250 Elo above their CCRL Blitz ratings relative to the other anchors under
these conditions, so the 14–17% scored against them pulls the fit down.

| Fit | Elo |
|---|---:|
| All games (the official figure) | 3542 ± 15 |
| Without Clover and Coda | 3571 ± 17 |
| Anchor games only | 3532 ± 16 |
| Anchor games only, without Clover and Coda | 3565 ± 18 |

Against the other eight anchors the implied rating runs from 3508 (Elixir) to 3654 (Stash 37).

### Against my own estimate

I predicted about 3590 (3550–3630) from matches against Stash 37 alone. The official 3542 is just below
that range; the fit without the two off-scale anchors, 3571, is inside it. The 79.0% scored against
Stash 37 here is higher than what I measured (72.5–74.5%), and it is the most favourable of all the anchors:
the Stash ladder flatters an engine that was tested only against it, which is the transfer error the
estimate warned about and under-sized. Strength against the wider field is about 20–50 Elo lower than
the Stash-only figure.
