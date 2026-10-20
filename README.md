# Limit Order Book & Matching Engine (C++20)

A single-instrument, single-threaded limit order book with a price-time
priority matching engine, built twice behind one interface:

| | Baseline (`MapOrderBook`) | Optimised (`FlatOrderBook`) |
|---|---|---|
| Price levels | `std::map<Price, Level>` per side | flat array indexed by tick, explicit best bid/ask |
| Orders in a level | `std::list` (FIFO) | intrusive doubly linked list |
| Order storage | heap (`new` per node) | preallocated object pool, no heap use on the hot path |
| Cancel lookup | `std::unordered_map<OrderId, iterators>` | `std::vector<Order*>` indexed by order id |

A randomised differential test checks that both produce identical trades and
identical book state on 1.4 million messages. A benchmark then compares
throughput and latency percentiles.

**Result:** on this machine the optimised book processes about **4.8× more
messages per second** and has roughly **3× lower** median, p99 and p99.9
latency than the baseline.

## Features

- Limit and market orders, buy and sell
- Add, cancel by id, modify
  - quantity reduction at the same price keeps time priority
  - price change or quantity increase loses priority (cancel + re-add, may trade)
- Price-time priority matching with partial fills and multi-level sweeps;
  trades print at the resting order's price
- Unfilled limit quantity rests; unfilled market quantity is discarded
- A `Trade {aggressor_id, resting_id, price, quantity}` for every fill, appended
  to a caller-owned vector
- Integer tick prices throughout: no floating point in the book

## Layout

```
include/lob/
  types.hpp              Price/Quantity/OrderId, Side, Trade, snapshots
  order_book.hpp         OrderBook concept: the common interface
  map_order_book.hpp     baseline
  flat_order_book.hpp    optimised
  object_pool.hpp        fixed-capacity pool used by the flat book
  message.hpp            Message type + apply(book, message)
  message_generator.hpp  seeded synthetic order flow
src/                     implementations of the above
tests/
  test_framework.hpp     ~100-line test framework (no dependencies)
  order_book_tests.cpp   unit tests, each run against both books
  differential_test.cpp  randomised map-vs-flat comparison
bench/
  benchmark.cpp          throughput + latency benchmark
  results/               raw benchmark output from the runs below
DESIGN.md                design write-up and interview questions
```

## Build and run

Needs CMake ≥ 3.20 and a C++20 compiler (tested with GCC 13.1).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

```bash
./build/lob_benchmark
```

The benchmark takes an optional message count and core to pin to:
`lob_benchmark 5000000 --cpu 2` (these are the defaults).

Release builds use `-O2`. To compare `-O3 -march=native`, use a second build directory:

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DLOB_NATIVE=ON
```

On Windows this was built with the MSYS2 UCRT64 toolchain (`g++`, `cmake`, `ninja`)
using `-G Ninja`.

## Benchmark results

**Machine:** Intel Core i5-13420H laptop (4 P-cores + 4 E-cores), Windows 11,
GCC 13.1 (MSYS2 UCRT64), benchmark thread pinned to logical CPU 2.

**Workload:** 5,000,000 generated messages, seed 42. Roughly 60% passive adds
near the touch, 30% cancels, 10% aggressive (half market orders, half
marketable limits). The first 500k messages are untimed warm-up; 4.5M are
measured. The book holds about 2,000 resting orders in steady state, and the
measured window produces 1,860,290 trades (identical for both books).

Throughput is the median of 5 timed passes. Latency is a separate pass timing
every message with `rdtsc`. Each figure below is the **median of three
separate benchmark runs** (all raw output is in [`bench/results/`](bench/results)).

### `-O2` (default Release)

| Implementation | Throughput (M msg/s) | Mean (ns) | p50 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|---:|
| `map` (baseline) | 3.06 | 396 | 342 | 1,204 | 1,939 |
| `flat` (optimised) | **14.78** | **132** | **110** | **405** | **632** |
| Speedup | **4.8×** | 3.0× | 3.1× | 3.0× | 3.1× |

### `-O3 -march=native`

| Implementation | Throughput (M msg/s) | p50 (ns) | p99 (ns) | p99.9 (ns) |
|---|---:|---:|---:|---:|
| `map` (baseline) | 2.75 | 378 | 1,274 | 2,065 |
| `flat` (optimised) | 13.45 | 117 | 445 | 704 |

`-O3 -march=native` made **no measurable difference**: every figure is within
the ±10% run-to-run variation of this laptop. The code is dominated by memory
access and branches, not by arithmetic that wider vectors could speed up.

### Latency by what the message did (`-O2`, one representative run)

| Implementation | Outcome | Share | p50 (ns) | p99 (ns) | p99.9 (ns) |
|---|---|---:|---:|---:|---:|
| map | add that rests | 55% | 391 | 761 | 1,173 |
| map | cancel | 30% | 288 | 610 | 1,058 |
| map | order that traded | 15% | 408 | 1,703 | 2,393 |
| flat | add that rests | 55% | 103 | 172 | 356 |
| flat | cancel | 30% | 113 | 474 | 786 |
| flat | order that traded | 15% | 177 | 452 | 628 |

**Reading the numbers**

- Every latency sample includes about 45 ns of timer overhead (two
  `lfence; rdtsc` pairs, measured and printed by the benchmark). With that
  removed, the flat book's median is about 65 ns against about 300 ns for the
  baseline. That ~4.6× ratio matches the throughput ratio.
- Max latencies (not shown) are in the hundreds of microseconds to
  milliseconds for both books. Those are OS interrupts and context switches,
  not the code. With the thread pinned to CPU 0, which services many
  interrupts on Windows, the baseline's p99.9 jumped to ~52 µs. That is why the
  benchmark pins to CPU 2 by default.

## Why the optimised version is faster

1. **No heap allocation per order.** The baseline makes at least two
   allocations for each resting order (a `std::list` node and an
   `unordered_map` node), plus a `std::map` node for each new price level, and
   frees them again on fill or cancel. The flat book takes a slot from a
   preallocated pool by popping a pointer off a free list.
2. **Array index instead of tree search.** Finding a price level is
   `levels[price - min_price]`, not a walk down a red-black tree through
   pointers scattered across the heap. The best price is a stored integer, not
   `map.begin()`.
3. **Fewer, more predictable memory accesses.** Orders live in one contiguous
   pool, the id lookup is a direct vector index (no hashing, no bucket chains),
   and the intrusive list puts the `prev`/`next` links inside the order itself,
   so unlinking an order touches the order and its two neighbours and nothing
   else.

The cost is generality: the flat book needs a bounded price range, a maximum
number of resting orders and dense order ids, all fixed up front. See
[DESIGN.md](DESIGN.md) for the full trade-off discussion.

## Correctness testing

- **Unit tests** (`order_book_tests`): 28 behaviours, each run against both
  implementations (resting, full and partial fills, multi-level sweeps, time
  and price priority, cancels of resting and unknown ids, all modify cases,
  market orders on empty and thin books, validation), plus flat-book capacity
  limits. 59 test cases in total.
- **Differential test** (`differential_test`): five seeded streams totalling
  1.4M messages (including modifies, stale cancels and aggressive-heavy flow
  that keeps emptying the book) go through both books. The test requires the
  same return value and the same trades, in order, for every message, and the
  same full book snapshot (every level, every order, in queue order) at
  regular checkpoints.
  To check that the test can catch real bugs, I planted one in the flat book
  (quantity increases keeping priority). The test reported a divergence at
  message 345.
- Tests also pass under libstdc++ debug mode (`-D_GLIBCXX_DEBUG`, checked
  iterators).
