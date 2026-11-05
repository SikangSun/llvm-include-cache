| redis, recompilation, front end | all: 187 compiles, plain clang 15.61 s | redis-server (src/): 125 compiles, plain clang 13.52 s | Header-fragment time reused | Unsafe reuses (charged as parses) |
|---|---|---|---|---|
| Header cache | 5.80 s: **2.69x** (2.30x) | 4.46 s: **3.03x** (2.80x) |  |  |
| Option 2 (as he built it), our repeat rule R1, macro lists | 8.04 s: **1.94x** (1.74x) | 6.56 s: **2.06x** (1.93x) | 76.2% | 193 |
| Option 3 (his preferred design), R1, macro lists | 7.60 s: **2.05x** (1.86x) | 6.18 s: **2.19x** (2.08x) | 76.6% | 178 |
| the same, its #define lines' time not counted | 8.06 s: **1.94x** (1.74x) | 6.58 s: **2.06x** (1.93x) |  |  |
| Option 2, his repeat rule as written (R0), macro lists | 9.38 s: **1.66x** (1.54x) | 7.88 s: **1.72x** (1.62x) | 62.1% | 185 |
| Option 2, R1, no macro checks (as his 2003 prototype) | 8.04 s: **1.94x** (1.75x) | 6.56 s: **2.06x** (1.93x) | 76.2% | 2,469 |
| Option 2, R1, p.30's two bits for macros | 8.55 s: **1.83x** (1.69x) | 7.04 s: **1.92x** (1.86x) | 70.8% | 2,422 |
| Option 2, the repeat rule most favourable to him (R2), macro lists | 6.82 s: **2.29x** (1.99x) | 5.58 s: **2.42x** (2.17x) | 89.0% | 193 |
| Option 3, R2, macro lists | 6.33 s: **2.46x** (2.15x) | 5.16 s: **2.62x** (2.38x) | 89.1% | 178 |
| Ideal fragment cache, option 2: any earlier identical parse | 6.40 s: **2.44x** (2.09x) | 5.20 s: **2.60x** (2.32x) | 93.3% | 0 |
| Ideal fragment cache, option 3 | 5.81 s: **2.69x** (2.31x) | 4.67 s: **2.90x** (2.60x) | 94.4% | 0 |
| Ceiling: every fragment reused, option 2 | 5.76 s: **2.71x** (2.31x) | 4.62 s: **2.93x** (2.62x) |  |  |
| Ceiling: every option-3 fragment reused, #define lines included | 5.24 s: **2.98x** (2.55x) | 4.17 s: **3.25x** (2.94x) |  |  |

Times are totals over the compiles; bold, the speed-up of the total; in parentheses, the geometric mean of the per-compile speed-ups. Bothner rows: restoring a reused fragment costs nothing, 1.65 ms of compiler start-up is credited per compile, and the time each reuse saves is scaled by 1.007. Option 3 also counts its #define lines' time as saved (replayed for free; not checked by blank.py). The ceilings reuse what every rule rejects: nested fragments and repeats.

Speed-up of the total under each credit for what a persistent server skips (all compiles / redis-server):

| | none | start-up | start-up + header files | whole process |
|---|---|---|---|---|
| Header cache | 2.69x / 3.03x | 2.69x / 3.03x | 2.69x / 3.03x | 2.69x / 3.03x |
| Option 2 (as he built it), our repeat rule R1, macro lists | 1.87x / 2.00x | 1.94x / 2.06x | 2.07x / 2.19x | 2.26x / 2.31x |
| Option 3 (his preferred design), R1, macro lists | 1.97x / 2.12x | 2.05x / 2.19x | 2.19x / 2.34x | 2.42x / 2.48x |
| the same, its #define lines' time not counted | 1.86x / 1.99x | 1.94x / 2.06x | 2.06x / 2.19x | 2.26x / 2.31x |
| Option 2, his repeat rule as written (R0), macro lists | 1.61x / 1.67x | 1.66x / 1.72x | 1.75x / 1.81x | 1.90x / 1.89x |
| Option 2, R1, no macro checks (as his 2003 prototype) | 1.87x / 2.00x | 1.94x / 2.06x | 2.07x / 2.19x | 2.26x / 2.31x |
| Option 2, R1, p.30's two bits for macros | 1.76x / 1.87x | 1.83x / 1.92x | 1.93x / 2.03x | 2.11x / 2.14x |
| Option 2, the repeat rule most favourable to him (R2), macro lists | 2.19x / 2.34x | 2.29x / 2.42x | 2.46x / 2.61x | 2.75x / 2.78x |
| Option 3, R2, macro lists | 2.35x / 2.52x | 2.46x / 2.62x | 2.67x / 2.84x | 3.01x / 3.04x |
| Ideal fragment cache, option 2: any earlier identical parse | 2.33x / 2.50x | 2.44x / 2.60x | 2.64x / 2.82x | 2.97x / 3.02x |
| Ideal fragment cache, option 3 | 2.55x / 2.77x | 2.69x / 2.90x | 2.93x / 3.16x | 3.34x / 3.42x |
| Ceiling: every fragment reused, option 2 | 2.57x / 2.80x | 2.71x / 2.93x | 2.95x / 3.20x | 3.38x / 3.46x |
| Ceiling: every option-3 fragment reused, #define lines included | 2.81x / 3.09x | 2.98x / 3.25x | 3.28x / 3.59x | 3.81x / 3.92x |

Credits per compile: none; compiler start-up, 1.65 ms; that and a server's cache of header text (p.24), at most the calibrated cost of each #include and file entry; or the whole process start outside the compile's own work, which his design did not save (his driver still ran).
