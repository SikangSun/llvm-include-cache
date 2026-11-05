| redis, recompilation, front end | all: 187 compiles, plain clang 15.61 s | redis-server (src/): 125 compiles, plain clang 13.52 s | Header-fragment time reused | Unsafe reuses (charged as parses) |
|---|---|---|---|---|
| Header cache | 5.80 s: **2.69x** (2.30x) | 4.46 s: **3.03x** (2.80x) |  |  |
| Option 2 (as he built it), our repeat rule R1, macro lists | 8.09 s: **1.93x** (1.73x) | 6.61 s: **2.05x** (1.92x) | 76.2% | 193 |
| Option 3 (his preferred design), R1, macro lists | 7.65 s: **2.04x** (1.84x) | 6.23 s: **2.17x** (2.06x) | 76.6% | 178 |
| the same, its #define lines' time not counted | 8.11 s: **1.92x** (1.73x) | 6.63 s: **2.04x** (1.92x) |  |  |
| Option 2, his repeat rule as written (R0), macro lists | 9.42 s: **1.66x** (1.54x) | 7.92 s: **1.71x** (1.61x) | 62.1% | 185 |
| Option 2, R1, no macro checks (as his 2003 prototype) | 8.09 s: **1.93x** (1.73x) | 6.61 s: **2.05x** (1.92x) | 76.2% | 2,469 |
| Option 2, R1, p.30's two bits for macros | 8.60 s: **1.82x** (1.68x) | 7.09 s: **1.91x** (1.85x) | 70.8% | 2,422 |
| Option 2, the repeat rule most favourable to him (R2), macro lists | 6.88 s: **2.27x** (1.97x) | 5.64 s: **2.40x** (2.15x) | 89.0% | 193 |
| Option 3, R2, macro lists | 6.40 s: **2.44x** (2.13x) | 5.22 s: **2.59x** (2.35x) | 89.1% | 178 |
| Ideal fragment cache, option 2: any earlier identical parse | 6.46 s: **2.41x** (2.08x) | 5.25 s: **2.57x** (2.29x) | 93.3% | 0 |
| Ideal fragment cache, option 3 | 5.87 s: **2.66x** (2.28x) | 4.73 s: **2.86x** (2.56x) | 94.4% | 0 |
| Ceiling: every fragment reused, option 2 | 5.83 s: **2.68x** (2.28x) | 4.68 s: **2.89x** (2.58x) |  |  |
| Ceiling: every option-3 fragment reused, #define lines included | 5.31 s: **2.94x** (2.51x) | 4.23 s: **3.20x** (2.89x) |  |  |

Times are totals over the compiles; bold, the speed-up of the total; in parentheses, the geometric mean of the per-compile speed-ups. Bothner rows: restoring a reused fragment costs nothing, 1.65 ms of compiler start-up is credited per compile. Option 3 also counts its #define lines' time as saved (replayed for free; not checked by blank.py). The ceilings reuse what every rule rejects: nested fragments and repeats.

Speed-up of the total under each credit for what a persistent server skips (all compiles / redis-server):

| | none | start-up | start-up + header files | whole process |
|---|---|---|---|---|
| Header cache | 2.69x / 3.03x | 2.69x / 3.03x | 2.69x / 3.03x | 2.69x / 3.03x |
| Option 2 (as he built it), our repeat rule R1, macro lists | 1.86x / 1.98x | 1.93x / 2.05x | 2.05x / 2.18x | 2.25x / 2.29x |
| Option 3 (his preferred design), R1, macro lists | 1.96x / 2.10x | 2.04x / 2.17x | 2.18x / 2.32x | 2.40x / 2.45x |
| the same, its #define lines' time not counted | 1.85x / 1.98x | 1.92x / 2.04x | 2.05x / 2.17x | 2.24x / 2.29x |
| Option 2, his repeat rule as written (R0), macro lists | 1.60x / 1.67x | 1.66x / 1.71x | 1.75x / 1.80x | 1.89x / 1.88x |
| Option 2, R1, no macro checks (as his 2003 prototype) | 1.86x / 1.98x | 1.93x / 2.05x | 2.05x / 2.18x | 2.25x / 2.29x |
| Option 2, R1, p.30's two bits for macros | 1.75x / 1.85x | 1.82x / 1.91x | 1.92x / 2.02x | 2.09x / 2.12x |
| Option 2, the repeat rule most favourable to him (R2), macro lists | 2.17x / 2.31x | 2.27x / 2.40x | 2.44x / 2.58x | 2.72x / 2.75x |
| Option 3, R2, macro lists | 2.33x / 2.49x | 2.44x / 2.59x | 2.64x / 2.80x | 2.97x / 3.00x |
| Ideal fragment cache, option 2: any earlier identical parse | 2.30x / 2.48x | 2.41x / 2.57x | 2.61x / 2.78x | 2.93x / 2.98x |
| Ideal fragment cache, option 3 | 2.53x / 2.74x | 2.66x / 2.86x | 2.90x / 3.12x | 3.30x / 3.37x |
| Ceiling: every fragment reused, option 2 | 2.54x / 2.77x | 2.68x / 2.89x | 2.92x / 3.16x | 3.33x / 3.41x |
| Ceiling: every option-3 fragment reused, #define lines included | 2.78x / 3.05x | 2.94x / 3.20x | 3.23x / 3.53x | 3.74x / 3.85x |

Credits per compile: none; compiler start-up, 1.65 ms; that and a server's cache of header text (p.24), at most the calibrated cost of each #include and file entry; or the whole process start outside the compile's own work, which his design did not save (his driver still ran).
