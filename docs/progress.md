# Progress log — Fable 5.2 chess 24hrs

## Identity and setup (recorded at start)

- Model: **Fable 5.2**
- Engine name (`id name`): `Fable 5.2 chess 24hrs`
- Author (`id author`): `Fable 5.2`
- Executable: `final/Fable52chess24hrs.exe`
- Start: 2026-11-09 15:28:25 local (see `docs/start_time.txt`). **Deadline: 2026-11-10 15:28:25.**
- Toolchain: GCC/G++ 15.2.0 (MinGW-w64, scoop). Language: C++20.
- Dev machine: Intel Core Ultra 9 285H, 16 physical cores (6P + 8E + 2LP-E), 31 GB RAM.
  **fastchess `-concurrency 14`** (physical cores − 2). All heavy work (tests, data generation,
  training) kept within 14 cores in total.

## Plan (made at hour 0)

1. H0–3: bitboard engine (PEXT sliders, copy-make), perft-verified, UCI, solid alpha-beta search
   with a temporary PeSTO-table eval. First build into `final/` as soon as it plays under fastchess.
2. H3–5: full modern search (TT, PVS, NMP, LMR, pruning, histories, SE), time management.
3. H5–9: self-play data generator inside the engine; from-scratch NNUE trainer (C++, CPU,
   multithreaded); NNUE inference (768→Nx2→1, incremental accumulators, AVX2).
4. H9–20: iterate data → train → test; SPRT search tweaks while data generates.
5. H20–24: time-management hardening, final build (PGO), verification, install.

Reasoning: eval quality dominates; an NNUE trained on own self-play data is worth far more than
a hand-written eval, and data generation can run on the cores while I write the trainer.

## Assumptions

- "Standard library only" applies to all code (engine and trainer): trainer is written from
  scratch in C++, no ML frameworks.
- Bulk training data lives in `source/data/*.bin` (git-ignored by the supplied `.gitignore`).
- The rating match may use zero `timemargin`: the engine must never exceed its clock.

## Log

### Firsts
- **First full perft-suite pass:** 2026-11-09 ~15:35 (all 126 positions exact to depth 5, first build, no fixes needed);
  depth 6 for all 126 positions (12.8 billion nodes) exact at 15:38. isPseudoLegal cross-checked against
  generation on 380k random-playout positions x 65536 move encodings: 0 errors.
- **First full legal games:** 2026-11-09 ~15:37-15:42, 100 games vs Stash 25 at 10+0.1 under fastchess
  (no crashes, illegal moves or time losses). fastchess `--compliance` passed at 15:36.

### Hour 0 notes - 2026-11-09 15:50 (elapsed 0h22)
- Wrote v0 in one pass: bitboards, PEXT sliders, copy-make, staged move picker, TT (3-entry clusters),
  PVS + aspiration, NMP, RFP, razoring, probcut, IIR, LMR, LMP, futility, SEE pruning, singular extensions,
  butterfly/continuation/capture history, correction history, PeSTO tapered eval, soft/hard time management.
- **v0 vs Stash 25 (2940): 75.0% over 100 games at 10+0.1 (+191 +/- 73) => ~3130.** Installed in `final/` at 15:43.
  Snapshot `source/snapshots/2026-11-09_1543_v0_hce`.
- Wrote self-play data generator (`engine datagen`), 32-byte samples; running since 15:44 on 12 threads with
  the HCE build, 5000 soft nodes/move: ~2,700 positions/s (~9.8M/hour).
- Wrote NNUE trainer (`source/trainer/trainer.cpp`, Adam, SCReLU, 768->Hx2->1x8 buckets, king-file mirroring)
  and engine inference (`nnue.cpp`, incremental lazy accumulators, AVX2, net embedded via .incbin, `EvalFile` option).
  Cross-checked: engine quantized eval matches trainer float output on real samples.
- Decision: CPU goes to data generation now (eval is the biggest lever); SPRT tuning of search later,
  alongside datagen at reduced thread count.

### Hour 1 - 2026-11-09 16:28 (elapsed 1h00)
- Worked on: NNUE pipeline end to end (datagen -> trainer -> embedded net -> engine). First net (768->256x2->1x8,
  2.7M HCE-labelled positions, 20 epochs) plays only 56 Elo below the PeSTO eval at 5+0.05 (200 games), at almost
  the same nps (2.9M vs 3.2M), so the pipeline is sound; more data needed. Validation loss by width on the same
  data: 64: 0.0213, 128: 0.0197, 256: 0.0178 -> wider is better even at tiny data.
- Found and fixed a latency problem: creating the search thread per `go` cost up to ~25 ms before the first node
  when all cores were busy. UCI loop rewritten: the thread that reads `go` searches immediately, a second thread
  takes over stdin (stop/isready/quit). Measured 0 ms start latency under full load. Compliance re-passed.
- Search constants are now tunable UCI options under `-DTUNE` (for SPRT/SPSA); wrote an SPSA driver
  (`source/tools/spsa.cpp`) and a PGO build script. Added a feature bundle behind toggles (threat-aware butterfly
  history, pawn history, eval-difference history, cutoff-count LMR, post-LMR conthist update, capture futility):
  bench nodes at depth 12 halved; SPRT of the bundle running at low concurrency (2 cores): +29 +/- 42 after 120 games.
- Working/tested now: `final/` = v0 (HCE, ~3130). Datagen running (12 threads): 6.4M positions so far.
- Elo estimate: ~3130 (v0 scored 75% vs Stash 25 in 100 games at 10+0.1).
- Next hour: at ~8-9M positions retrain; when the net beats HCE switch datagen to the NNUE engine (gen 2);
  finish the bundle SPRT; first NNUE build into `final/` once verified.

### Hour 2 - 2026-11-09 17:28 (elapsed 2h00)
- Net 2 (7.5M HCE-selfplay positions): +51 +/- 42 vs the PeSTO build (200 games, 5+0.05). **v5n2 vs Stash 30
  (3170): 60.0% over 100 games at 10+0.1 (+70 +/- 51) => ~3240.** Installed in `final/` at 17:07 (snapshot
  `2026-11-09_1707_v5_net2`). Feature bundle accepted on judgement (+22 +/- 37 after 160 games, bench nodes halved).
- Datagen switched to the NNUE engine (gen 2/3, 3500 soft nodes, 10 threads, ~10.5M positions/hour).
  Data so far: 7.5M (HCE) + 7.0M (NNUE).
- Found the quantized engine eval differed from the trainer's float eval by 16.8 cp RMS (weights are small compared
  with the 1/255 and 1/64 steps). New scheme: accumulator x1020, activation = v*v/256 via mulhi (12 bit), output
  weights x1024 -> 1.9 cp RMS, same AVX2 cost; engine matches a quantization emulation in the trainer to <1 cp.
  Accepted on reasoning (strictly less eval noise); datagen restarted with it.
- SPSA run started (26 search parameters, fixed 100k nodes/move, 4 cores, 750 iterations x 16 games, ~5 h).
- final info line now printed when an aborted iteration changes the best move (fastchess PV warning).
- Elo estimate: ~3240 (60% vs Stash 30, 100 games).
- Plan: let data accumulate; retrain every ~2 h (stop datagen, train on all cores, restart with the new net);
  after SPSA: verify tuned values by SPRT at 10+0.1, then time-management tests; test a 512-wide net at ~35M positions.

### Hour 3 - 2026-11-09 18:30 (elapsed 3h02)
- Data: 30.9M positions (7.5M HCE-selfplay, 23.4M NNUE-selfplay). Net 4 trained on all of it (20 epochs, lambda 0.5):
  256-wide val loss 0.02229, 512-wide 0.02124 (same data/split). 512-wide engine is only ~13% slower in nps.
- PGO build measured: identical node counts, no measurable speed gain (2.55-2.89M nps both) -> not pursuing.
- Time-usage analysis of the Stash-30 match PGN: avg 464 ms/move for moves 1-10, 318, 213, 160, 142 ... ~100 ms later;
  avg 2.07 s left on the clock at game end (min 0.51 s) -> allocation is somewhat conservative; TM tests queued.
- SPSA restarted with a 4x larger learning rate (first run barely moved the parameters); 114+/750 iterations done.
- Running now: datagen 8 threads (net 4, 256), SPSA 4 cores, net lab 2 cores: 512 @87k nodes vs 256 @100k nodes
  (node handicap = measured nps ratio; fixed nodes make the comparison independent of machine load).
- final/ unchanged (v5n2, ~3240).
- Next: decide net width from the node-handicap match; install net 4 after a Stash check; keep generating data.

### Hour 4 - 2026-11-09 19:28 (elapsed 4h00)
- **512 vs 256 (node handicap 87k vs 100k): 512 is -64 +/- 23 Elo over 480 games** although its validation loss was
  lower. Cause found: my validation split was per position, so positions from the same games were in both sets and the
  wide net was memorising game results. Test on unseen gen-4 games: score loss 256: 0.00692, 512: 0.00807, 128: 0.00705.
  Trainer now holds out whole games (last 2% of each file), reports result-only and score-only held-out losses, and has
  a `--test` mode and weight decay option. Lesson: decide net questions by play (fixed-node matches), not training loss.
- Held-out metrics by lambda (256-wide, 30.9M positions, 20 epochs): lambda 0.5: wdl 0.0818 / score 0.0096;
  0.8: 0.0847 / 0.0062; 1.0: 0.0882 / 0.0055. No overfitting upturn over 20 epochs at lambda 0.8.
- Net lab running (4 cores, fixed 100k nodes, round robin, 1200 games): lambda 0.5 vs 0.8 vs 1.0 (256) vs 128-wide.
- SPSA at iteration ~320/750 (parameters now moving: e.g. rfpMargin 75->84, razorMargin 330->408, futMul 110->136,
  seeQuietMul 25->17, seeNoisyMul 95->72, qsFutMargin 180->225).
- Data: 35M positions. Datagen temporarily at 6 threads while the lab uses 4.
- final/ unchanged (v5n2, ~3240). Elo estimate of current best (net 4, new quantization): not yet measured vs Stash.
- Next: pick lambda/width from the tournament, retrain on everything, verify vs Stash, install.

### Hour 5 - 2026-11-09 20:28 (elapsed 5h00)
- **Net tournament (fixed 100k nodes, 1200 games, round robin): lambda 1.0: +117, lambda 0.8: +92, 128-wide lambda 0.5: -81,
  256-wide lambda 0.5: -128.** Blending in game results (lambda 0.5) was costing >200 Elo: with ~350k games the
  per-game result label is mostly memorisable noise. All training now uses pure search-score targets (lambda 1.0).
- **v7n5 (net 5 = lambda 1.0 on 22.8M positions) vs Stash 37 (3424): 53.0% over 100 games at 10+0.1 (+21 +/- 47)
  => ~3445.** Installed in `final/` at 20:07 (snapshot `2026-11-09_2007_v7_net5`). Compliance passed.
- 512-wide re-tested with lambda 1.0 (87k vs 100k node handicap): -28 +/- 20 over 600 games -> stay at 256 for now.
- Dropping the PeSTO-labelled gen-1 data: -60 +/- 20 over 498 games (15.2M vs 22.8M positions). Data volume dominates:
  ~+60 Elo per 1.5x data here. So: CPU re-allocated to data generation (12 threads, soft nodes 3500 -> 2500 for more
  positions/hour), lab reduced to 2 cores, SPSA paused (it was at iteration 344 + 14; current values saved).
- Net 6 trained on all 32.5M positions (5.5 min on 14 threads); datagen restarted with it (gen 6). n6 vs n5 check running.
- Correction of the hour-3 entry: data total at that time was 22.8M (not 30.9M).
- Elo estimate: ~3445 (53% vs Stash 37, 100 games).
- Next: verify n6 > n5, Stash check, install; retrain roughly every 2 h as data grows; re-test 512 at ~80M;
  later SPSA + time-management tests on the lab cores.

### Hour 6 - 2026-11-09 21:28 (elapsed 6h00)
- Net 6 (32.5M positions, lambda 1.0): +31 +/- 26 vs net 5 at fixed nodes (300 games); **57.5% vs Stash 37 over 100
  games at 10+0.1 (+52 +/- 51) => ~3475.** Installed in `final/` at 21:06 (snapshot `2026-11-09_2106_v7_net6`).
- Datagen (12 threads, 2500 soft nodes, net 6): ~5,000 positions/s = 18M/hour. Data total ~57M.
- SPSA resumed on the 2 lab cores from the saved values (fixed 100k nodes).
- Datagen now also writes positions whose best move is a capture (flagged) so the trainer can include or exclude
  them (+24% positions for free if they turn out to help; to be tested in the lab). Takes effect at the next restart.
- Plan: retrain ~21:50 (compare warm-start 8 epochs against from-scratch 20 epochs), then every ~1.5-2 h;
  data generation until ~hour 19.5, then final net + time-management tests + SPSA validation on all cores.

### Hour 7 - 2026-11-09 22:28 (elapsed 7h00)
- Net 7 = net 6 warm-started for 8 epochs on all 55.9M positions (5 min on 12 threads instead of ~12 min from
  scratch): +26 +/- 27 vs net 6 over 300 fixed-node games. Compliance passed; installed in `final/` at 22:09
  (snapshot `2026-11-09_2209_v8_net7`). Not yet measured vs Stash; estimate ~3500 (n6 measured 3475).
- Datagen restarted with net 7 at 2000 soft nodes, now also writing flagged capture-best-move positions:
  7,000 positions/s (25M/hour, ~80% quiet).
- SPSA (2 lab cores, fixed nodes) ~430 iterations in total so far; notable drifts: razorMargin 330->428,
  futMul 110->144, histPruneMul 3500->2257, seeQuietMul 25->12, seeNoisyMul 95->68, histBonusMul 160->210,
  aspDelta 12->16, qsFutMargin 180->262. To be validated against defaults before use.
- Elo estimate: ~3500 (extrapolated; last direct measurement 3475 for net 6).
- Next: lab test of including capture-best-move positions in training (~22:50); retrain ~23:45.

### Hour 8 - 2026-11-09 23:35 (elapsed 8h07)
- Lab: including capture-best-move ("noisy") positions in training: **-49 +/- 29** over 300 fixed-node games
  (5.75M quiet vs 7.2M quiet+noisy, same games) -> keep the quiet-only filter. The datagen still writes them
  (flagged, costs nothing); the trainer drops them by default.
- Data: 87.5M quiet positions. Net 8 = net 7 warm-started, 8 epochs on everything (one training run was cut by a
  tool timeout after epoch 4 and resumed from its checkpoint for the remaining 4 epochs - lesson: run long jobs
  in the background).
- 512-wide net warm-starting on 77M positions on the 2 lab threads (done ~00:10) for a re-test of the width question.
- SPSA paused since 22:30 (lab cores used for net experiments); values so far saved in `source/tests/spsa5.txt.out`.
- final/ = v8n7 (est. ~3500).
- Next: check n8 vs n7, install; 512 vs 256 node-handicap match; then SPSA again.

### Hour 9 - 2026-11-10 00:50 (elapsed 9h22)
- Net 8 (87.5M quiet positions): +34 +/- 26 vs net 7 at fixed nodes (300 games); **63.0% vs Stash 37 over 100 games
  at 10+0.1 (+92 +/- 50) => ~3515.** Installed in `final/` at 00:20 (snapshot `2026-11-10_0020_v8_net8`).
- 512-wide warm-started net (77M positions): held-out score loss 0.004370 vs 0.004376 for the 256 net -> no quality
  edge to pay for its ~13% nps cost; width question closed for now (re-check only if data passes ~200M).
- Datagen: gen 8 with net 8, ~6,800 positions/s (incl. flagged noisy), ~20M quiet/hour. Total quiet ~110M.
- Lab now: SPSA values (after ~450 iterations) vs defaults, fixed 100k nodes, 500 games.
- Elo estimate: ~3515.

### Hour 10 - 2026-11-10 01:40 (elapsed 10h12)
- **SPSA round 1 validated: tuned values +52 +/- 24 Elo vs my hand-picked defaults (300 games, fixed 100k nodes).**
  Adopted as defaults (v9). More constants refactored into tunables (LMR adjustments in 1/1024 plies, NMP/LMP formulas,
  correction-history weights, history weights, malus, SEE piece values); refactor verified bench-identical at the old
  defaults before switching. SPSA round 2 (55 parameters) running, now on 4 cores.
- **v9n8 vs Stash 37: 65.0% over 100 games at 10+0.1 (+108 +/- 53) => ~3530.** Compliance passed; installed in
  `final/` at 01:37 (snapshot `2026-11-10_0137_v9_net8`).
- Datagen restarted with the v9 search (gen 9, net 8), 10 threads. Total quiet data ~125M.
- Elo estimate: ~3530.
- Next: retrain ~02:15 (net 9), keep SPSA going, re-validate SPSA values every few hours.

### Hour 11 - 2026-11-10 02:35 (elapsed 11h07)
- Net 9 (warm start, 6 epochs on 129.8M quiet positions): only **+9 +/- 27** vs net 8 at fixed nodes (300 games);
  train 0.004169 vs held-out 0.004265 -> the 256-wide net is now capacity-limited, not data-limited.
  Not installed (within noise). Datagen now runs with net 9 anyway (gen 10).
- Consequence: re-open the width question. 512-wide net warm-starting on all 130M positions on the 4 lab threads
  (~45 min), then a node-handicap match (87k vs 100k nodes) against net 9. SPSA round 2 paused at iteration 104/1400
  (the tool can now resume at an iteration offset so the step-size schedule is kept).
- final/ = v9n8 (~3530).

### Hour 12 - 2026-11-10 03:40 (elapsed 12h12)
- 512-wide on all 132M positions (held-out score loss 0.004139 vs 0.004265 for 256): with the 87k-vs-100k node
  handicap it scores +3 +/- 22 over 400 games -> equal strength at equal time; plain widening does not pay.
- New direction for eval capacity: **king input buckets** (5 buckets by own-king zone on top of the file mirroring):
  more feature-transformer capacity at ~zero inference cost. Implemented in trainer (`--kb 5`, `--expand` copies a
  single-bucket net into every bucket so training starts from the same function) and engine (`NN_KB`, refresh when the
  king changes zone). KB=1 build verified bench-identical. Training k1 (8 epochs, all data, 14 threads) now;
  datagen paused for it, SPSA still paused (lab cores are in the training).
- final/ = v9n8 (~3530). Quiet data: ~150M.

### Hour 13 - 2026-11-10 04:30 (elapsed 13h02)
- **King input buckets (5): +60 +/- 23 Elo vs net 9 at equal nodes (300 games)**; held-out score loss 0.004138 vs 0.004265;
  engine eval matches the trainer (1.8 cp RMS). Net k1 = net 9 expanded to 5 buckets + 8 epochs on 147M positions.
  Datagen (gen k1) and SPSA round 2 (resumed at iteration 104 with the step schedule kept) now use the KB engine.
- Stash-37 check of v10k1 at 10+0.1 running (2 cores) before installing it in `final/`.
- Bucket layouts are now table-driven (5 or 10 buckets) with hierarchical warm start (each new bucket starts from the
  old bucket that contained it); a 10-bucket net is the next lab experiment.
- final/ = v9n8 (~3530); expected after install ~3590.

### Hour 14 - 2026-11-10 05:40 (elapsed 14h12)
- **v10k1 (5 king buckets) vs Stash 37: 73.5% over 100 games at 10+0.1 (+177 +/- 57) => ~3600.** Compliance passed;
  installed in `final/` at 04:38 (snapshot `2026-11-10_0438_v10_k1`).
- 10 king buckets (warm-started from k1, 6 more epochs): +10 +/- 20 vs k1 over 404 fixed-node games, i.e. nothing beyond
  what the extra epochs would give; train/val gap grows (0.00374 vs 0.00413). Staying with 5 buckets.
- Retrain k2 (5 buckets, 6 epochs, 166M quiet positions, 14 threads) in progress; datagen restarts with it afterwards.
- SPSA round 2 at iteration 133/1400 (paused during net experiments); resumes on 4 cores after the k2 check.
- Remaining plan: datagen + SPSA until ~11:30; final net ~11:30-12:15; 12:15-14:30 all cores on time-management
  tests and SPSA validation at 10+0.1; 14:30-15:15 final build, standalone/compliance checks, install.
- Elo estimate: ~3600 (73.5% vs Stash 37, 100 games; Stash 37 is now too weak to resolve further gains well).

### Hour 15 - 2026-11-10 06:40 (elapsed 15h12)
- k2 (5 buckets, +6 epochs, 166M positions) vs k1: -2 +/- 24 (300 games); fine-tuning k2 on only the most recent
  data (79M positions from the strongest engines): -12 +/- 30 (202 games). The eval has plateaued for this
  architecture/recipe; more data or recency does not help measurably any more. final/ keeps k1.
- Decision (06:00): stop data generation (175M quiet positions on disk) and move the CPU to search:
  SPSA round 2 (55 parameters, fixed 100k nodes) now on 8 cores (~3,600 games/hour), and a time-management round robin
  at the real 10+0.1 on 6 cores: soft-time divisor 18 vs 26 (current) vs 38 (+inc 70%).
- TM standings after 80 games each: tm26 +22, tm18 +4, tm38 -26 (all within noise).
- Elo estimate: ~3600. final/ = v10k1.
- Next: let both run ~2-3 h; then validate SPSA values vs current defaults (fixed nodes, then 10+0.1 vs Stash),
  adopt the best TM setting, final build and checks from ~13:30.

### Hour 16 - 2026-11-10 07:45 (elapsed 16h17)
- Time-management round robins at 10+0.1 (6 cores): RR1 (400 games each): divisor 18: +6, 26 (current): +4, 38: -10
  (all +/- 20). RR2 (160 games each): 18 + falling-score scaling: +28, 18: +20, 13: -48 +/- 29. Conclusion: the allocation
  is on a flat optimum between 18 and 26, falls off on both sides (13 clearly bad) -> keep 26. Added a falling-score
  soft-time scaling parameter (off by default, bench-identical); RR3 now tests it directly: base vs mul 8 vs mul 16.
- SPSA round 2 at iteration ~256/800 on 8 cores.
- Build scripts now link with an 8 MB stack reserve (was 2 MB; deepest frames estimated ~1.4 MB at MAX_PLY) as a
  safety margin; deep-search stress runs showed no crash.
- final/ = v10k1 (~3600).

### Hour 17 - 2026-11-10 08:50 (elapsed 17h22)
- **SPSA round 2 validated mid-run (iteration 325/800): +56 +/- 21 Elo vs the round-1 defaults over 508 fixed-node games.**
  Biggest drifts: rfpMargin 89->70, razorMargin 404->504, qsFutMargin 271->342, LMR not-improving +1.0 -> +1.62 ply,
  nmpA 256->283, lmpA 192->232, corrPawnW 16->11, corrNonPawnW 16->21, mvvMul 16->11, SEE values Q 950->1033, R 500->539.
  Adopted as defaults (v12). SPSA keeps running from the current values on 8 cores.
- TM RR3 (320 games each, 10+0.1): falling-score scaling mul 8: +5, mul 16: +4, off: -10 (+/- 20); together with RR2 this leans
  positive (~+10), so tmFallMul = 10 adopted on judgement. Soft-time divisor stays 26.
- v12k1 (SPSA2-mid + falling-score TM, net k1) built, compliance passed; 400-game match vs v10k1 (current final) at
  10+0.1 running on 6 cores to confirm at the real time control before installing.
- Elo estimate: ~3600 installed; v12 expected ~3650 if the fixed-node gain carries over.

### Hour 18 - 2026-11-10 09:50 (elapsed 18h22)
- **v12k1 vs v10k1 (previous final) at 10+0.1: +38 +/- 21 over 400 games, no time losses.** Installed in `final/` at 09:27
  (snapshot `2026-11-10_0927_v12_k1`). Estimate ~3640.
- SPSA continues from the round-2 values. Tried 14 concurrent games: slower than 8 (the two low-power cores and the
  per-iteration tail dominate) -> now 10 cores, 40 games per iteration, ~2,400 games/hour (games are longer now:
  more draws between tuned engines).
- A 200-game gauntlet v12k1 vs Stash 37 at 10+0.1 runs on the other 4 cores for the record.
- Plan to the deadline (15:28): SPSA until ~12:00; validate final values at fixed nodes, then at 10+0.1 vs v12;
  install by ~13:00 if positive; 13:00-14:45 final checks (standalone, compliance, clean-PATH run, sanity matches
  vs all Stash versions for time losses); stop everything by 15:00.

### Hour 19 - 2026-11-10 10:57 (elapsed 19h29)
- Stash-37 gauntlet on the 4 slowest cores while SPSA held the other 10: Stash lost 66 of 200 games on time (overruns up
  to 158 ms, median 20 ms); **my engine lost 0 on time** in the same conditions (and 0 in the following 480 self-play
  games there). The 83% score is therefore not a strength measurement, but it is good evidence for clock safety.
- TM RR4 (240 games each, 10+0.1, 4 cores): increment share 85% (was 60%): **+39 +/- 26**, base: -12, hard-limit
  multiplier 6.5 (was 4.5): -28. The engine was finishing games with ~2 s unused; spending more of the increment helps.
  RR5 now: inc 85 vs inc 100 vs inc 85 + hard multiplier 3.0.
- SPSA at iteration 392/725 (10 cores).
- final/ = v12k1 (~3640).

### Hour 20 - 2026-11-10 11:50 (elapsed 20h22)
- SPSA stopped at iteration 429/725 of the last schedule (~17,000 games in round 2 overall). Final values vs v12
  (round-2 mid values): **+18 +/- 15 over 702 fixed-node games**. Adopted.
- TM RR5 (160 games each): inc 85: +2, inc 85 + hard x3.0: +2, inc 100: -4 -> all equal; with RR4 (inc 85 = +39 +/- 26
  vs inc 60) the increment share is set to 85%, hard multiplier unchanged.
- v13k1 = final SPSA values + increment share 85% (+ falling-score scaling from v12), net k1. Compliance passed.
  Confirmation match v13k1 vs v12k1 at 10+0.1 running on 10 cores with nothing else on the machine (500 games).
- Remaining plan: install v13 if confirmed (~12:35); then clean-condition gauntlet vs Stash 33/37 for the final Elo
  estimate, standalone/clean-PATH checks, final snapshot; stop all work by ~15:00.

### Hour 21 - 2026-11-10 12:17 (elapsed 20h49)
- **v13k1 vs v12k1 at 10+0.1, clean machine, 500 games: -1 +/- 16.** The fixed-node SPSA gain (+18 +/- 15) and the
  increment-share change (+39 in a round robin run on four heavily contended slow cores) do not add up to a gain at the
  real time control on an otherwise idle machine. v13 is NOT installed; `final/` remains v12k1.
- Last experiment (12 cores, nothing else running, 1200 games, ends ~13:45): round robin v12 vs v12 + inc 85 vs
  v12 + final SPSA values, to see whether either change alone is worth installing.
- After that: final decision by ~14:00, then final checks and docs. No further development.

### Hour 22 - 2026-11-10 13:42 (elapsed 22h14)
- Final round robin (clean machine, 12 cores, ~560 games each): v12 + inc 85: +6, v12 + final SPSA: +5, v12: -11 (+/- 16).
- Second direct match v13k1 vs v12k1 at 10+0.1: +11 +/- 14 over 800 games; combined with the first (500 games):
  **+6.7 +/- 9.9 over 1,300 games, zero time losses.** Small but positive, consistent with the fixed-node evidence.
  **v13k1 installed in `final/` at 13:31** (snapshot `2026-11-10_1331_v13_k1_FINAL`); compliance passed; only
  KERNEL32.dll and msvcrt.dll imported; runs from a clean PATH; `id name Fable 5.2 chess 24hrs`, `id author Fable 5.2`.
- **Final build vs Stash 37 (3424), clean machine, 200 games at 10+0.1: 70.5% (+151 +/- 38) => ~3575**; no time losses
  by either side.
- No further engine changes. A last sanity gauntlet vs all six Stash versions (240 games) is running to look for
  crashes/time losses in other kinds of games.
- Final Elo estimate: ~3575 on the Stash/CCRL-blitz ladder (evidence: 70.5% vs Stash 37 over 200 games; earlier
  73.5% over 100 games for v10k1). Uncertainty at least +/- 40 statistical, plus the ladder's own transfer error.

### Hour 23 - 2026-11-10 14:45 (elapsed 23h17) - FINAL
- Clean-machine measurements of v13k1 vs Stash 37 at 10+0.1: 70.5% (200 games) and 72.5% (300 games), plus a
  240-game sanity gauntlet vs all six Stash versions (91% overall; Stash 20/21: 40-0 each, 25: 97.5%, 30: 87.5%,
  33: 92.5%, 37: 68.8%). No crash, illegal move or time loss by my engine in any of them.
- While collecting the final parameter list I found that my default-update script had skipped two SPSA parameters
  (`seeQuietMul`, `seeNoisyMul` - their names matched the filter meant for the SEE piece values), so v12/v13 carried
  12/72 instead of the tuned 8/79. Built v14k1 with the correct values: **+3.5 +/- 15 vs v13k1 at 10+0.1 (600 games,
  no incidents)**. It is the configuration SPSA actually validated at fixed nodes and is not worse at the real time
  control, so **v14k1 was installed as the final entry at 14:36** (snapshot `2026-11-10_1436_v14_k1_FINAL`).
- Final checks on `final/Fable52chess24hrs.exe`: byte-identical to `source/build/v14k1.exe`; fastchess `--compliance`
  passed; imports only KERNEL32.dll and msvcrt.dll; runs from cmd with PATH=System32 only; `id name Fable 5.2 chess
  24hrs`, `id author Fable 5.2`; 8 MB stack reserve; rebuild from `source/nets/k1_256.nnue` gives the same bench.
- **Final entry vs Stash 37 (3424): 74.5% over 200 games at 10+0.1 (+186 +/- 37), no incidents.** Pooled with the
  v13 matches (same engine apart from two pruning margins): 72.5% over 700 games => +168 +/- 21.
- **Final Elo estimate: ~3590 (range roughly 3550-3630) on the Stash/CCRL-blitz ladder.**
- All background jobs stopped. No further changes to `final/`. Remaining time is used for the README only.

### Summary of what was measured vs accepted on judgement
- Measured by matches: every net generation (fixed-node matches vs the previous net), net width (512 x3), lambda,
  HCE-data removal, noisy-position inclusion, king buckets (5, 10), recency fine-tune, SPSA results (fixed nodes and
  at 10+0.1), time-management settings (5 round robins), every build installed in `final/` (Stash matches).
- Accepted on reasoning: finer quantization (verified numerically against the trainer), the UCI threading model
  (measured latency), 8 MB stack, the initial search feature bundle (+22 +/- 37 only), falling-score time scaling
  (+10 +/- 20 in two round robins), warm-start retraining.
