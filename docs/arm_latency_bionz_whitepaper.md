# DriveMode-Change (ARM) Latency Across Four Sony Alpha Bodies

**Prepared for:** Alex
**Project:** TotalControl — TSE 2026-08-12 (Burgos/Lerma) autonomous multi-camera control
**Date:** 2026-07-21
**Author:** Maciej / TotalControl project

## Summary

Four Sony Alpha bodies (ILCE-7SM3, ILCE-7C, ILCE-7RM4A, ILCE-7M4) were calibrated for
bracket-ARM latency — the time Sony's Camera Remote SDK takes to accept a DriveMode
change (bracket EV/count) between captures. Two cameras are consistently fast
(~300-550ms); two are consistently slow and highly variable (up to ~5.4s). The split
lines up exactly with **image-processor generation** (BIONZ X vs BIONZ XR), not with
camera age or resolution — a same-generation "A" refresh (ILCE-7RM4A, announced 2021)
is slower than an older BIONZ XR body (ILCE-7SM3, announced 2020) because it inherited
its processor unchanged from the original 2019 body.

## Background

TotalControl fires synchronized exposure brackets across up to 4 cameras simultaneously
during the TSE 2026 totality window. Between brackets with different EV/count settings,
the camera's DriveMode property must be changed; Sony's SDK rejects this change
(`err=0x8402`) while the camera is still clearing its internal capture buffer to the
memory card. This "ARM latency" directly constrains how tightly consecutive bracket
blocks can be scheduled — underestimating it causes failed ARM attempts during a live
sequence; overestimating it wastes irreplaceable seconds during the ~104s of totality.

## Method

A dedicated calibration preset ("Bracket ARM Calibration") fires all 16 supported
bracket EV/count combinations (0.3/0.5/0.7/1.0/2.0/3.0 EV x 3/5/9 shots, where
supported), 5 repetitions each, **interleaved** (one full pass through all 16 variants,
repeated 5x) rather than grouped by variant — this spreads any time-of-run drift
(thermal, battery, accumulated buffer state) evenly across all combinations instead of
biasing one variant's measurements. All 4 cameras ran this preset concurrently, each on
its own thread and pipe connection (TotalControl's per-camera sequencer). One
camera (ILCE-7C) was independently re-run alone to check for cross-camera interference
from the concurrent test harness.

Raw per-shot ARM latencies (n=320, plus n=80 for the solo re-run) are available as CSV:
`calibration/arm_timing_2026-07-21_per_model.csv` and
`calibration/arm_timing_2026-07-21_ILCE-7C_solo.csv`.

**Known limitation:** n=5 per (model, count, EV) combination is a first-pass sample,
not a high-confidence calibration (compare to the 10x-repeated single-camera sweep done
earlier for ILCE-7RM4A alone, `calibration/bracket_calibration_10x.csv`). The very first
ARM in each run is a cold start (no preceding capture to flush) and is not comparable to
the other reps in its group — this shows up as a low outlier in the count=3 groups
below.

## Results

### Overall per camera (n=80 each: 16 variants x 5 reps)

| Model | min (ms) | median (ms) | mean (ms) | max (ms) | stdev (ms) |
|---|---:|---:|---:|---:|---:|
| ILCE-7SM3 | 125 | 328 | 323 | 437 | 44 |
| ILCE-7M4  | 234 | 468 | 456 | 546 | 45 |
| ILCE-7C   | 312 | 3422 | 3439 | 4484 | 649 |
| ILCE-7RM4A| 297 | 4063 | 4149 | 5438 | 881 |

### By bracket count (across all EV steps)

| Model | count | n | min (ms) | mean (ms) | max (ms) |
|---|---:|---:|---:|---:|---:|
| ILCE-7SM3 | 3 | 30 | 125 | 319 | 437 |
| ILCE-7SM3 | 5 | 30 | 265 | 323 | 437 |
| ILCE-7SM3 | 9 | 20 | 266 | 329 | 391 |
| ILCE-7M4  | 3 | 30 | 234 | 450 | 532 |
| ILCE-7M4  | 5 | 30 | 390 | 465 | 546 |
| ILCE-7M4  | 9 | 20 | 375 | 450 | 500 |
| ILCE-7C   | 3 | 30 | 312 | 3817 | 4484 |
| ILCE-7C   | 5 | 30 | 1984 | 3017 | 3438 |
| ILCE-7C   | 9 | 20 | 3422 | 3504 | 3609 |
| ILCE-7RM4A| 3 | 30 | 297 | 4711 | 5438 |
| ILCE-7RM4A| 5 | 30 | 1500 | 3581 | 3828 |
| ILCE-7RM4A| 9 | 20 | 4094 | 4157 | 4266 |

Note the non-monotonic pattern on the slow pair: count=3 is *slower* on average than
count=5 for both ILCE-7C and ILCE-7RM4A. Buffer-clear time on these bodies does not
scale simply with shots-per-bracket; it likely depends on buffer state carried over
from preceding blocks in the run (consistent with an earlier, unrelated single-camera
sweep — `calibration/buffer_depletion_sweep.csv` — that found a single 9-shot burst
clears slower than three 3-shot bursts totalling the same 9 shots).

### Solo re-run: is the slow pair an artifact of 4-camera concurrency?

ILCE-7C was re-run alone (no other cameras connected) to rule out cross-camera
interference from TotalControl's concurrent per-camera sequencer threads.

| count | 4-camera concurrent run (mean) | solo run (mean) | delta |
|---|---:|---:|---:|
| 3 | 3817 ms | 4025 ms | +5% |
| 5 | 3017 ms | 2985 ms | -1% |
| 9 | 3504 ms | 3508 ms | ~0% |

Results are within measurement noise of each other. **The slow, variable ARM latency
on ILCE-7C is a genuine property of the camera/firmware, not a side effect of running
four cameras concurrently.**

## Key finding: correlates with processor generation, not camera age

| Model | Announced | Image processor | ARM latency |
|---|---|---|---|
| ILCE-7RM4A (a7R IVA) | 2021-04-07 (sensor/AF refresh of the 2019-07-16 a7R IV) | BIONZ X | slow, high variance |
| ILCE-7C (a7C) | 2020-09-14 | BIONZ X | slow, high variance |
| ILCE-7SM3 (a7S III) | 2020-07-28 (first Sony body with BIONZ XR) | BIONZ XR | fast, stable |
| ILCE-7M4 (a7 IV) | 2021-10-21 | BIONZ XR | fast, stable |

The split is clean and matches the processor, not the announcement date: ILCE-7RM4A
was announced *after* ILCE-7SM3 yet is the slowest camera in the set, because the "A"
refresh changed only the sensor coating and AF system — it kept the same BIONZ X
processor as the original 2019 body. BIONZ XR is Sony's claimed ~8x processing-power
generation, first shipped in the a7S III; the correlation with buffer-clear speed here
is consistent with that being a real processing-throughput effect (encoding/flushing
the capture buffer to card), not incidental.

## Implications for TotalControl

- `ArmEstMs()` and `kDriveModeVerifyMs` currently apply a single, ILCE-7RM4A-derived
  ARM-timing budget uniformly to all cameras. For ILCE-7SM3/ILCE-7M4 this over-estimates
  by roughly 8-12x, unnecessarily slowing down per-camera scheduling on the fast pair.
- Any future per-camera-model calibration should key off this fast/slow split at
  minimum; it may be usable as a coarse default (BIONZ XR vs BIONZ X) for camera models
  not yet directly measured.
- Code changes are pending the author's own statistical review of the raw CSVs; this
  note summarizes the data collected so far, not a finalized model.

## Data files

- `calibration/arm_timing_2026-07-21_per_model.csv` — 320 raw ARM samples, all 4 cameras
- `calibration/arm_timing_2026-07-21_ILCE-7C_solo.csv` — 80 raw ARM samples, ILCE-7C alone
- `calibration/bracket_calibration_10x.csv` — earlier 10x single-camera sweep (ILCE-7RM4A)
- `calibration/buffer_depletion_sweep.csv` — earlier sustained-shooting buffer-depletion sweep
