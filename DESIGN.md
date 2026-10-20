# Design Notes

This document explains each design decision in the order book and what it
costs. It is written for someone learning the material: each section says
what was chosen, why, and what the alternative would have been.

---

## 1. What a limit order book does

A limit order book holds every outstanding buy and sell order for one
instrument.

- **Bids** are buy orders. The highest bid is the *best bid*.
- **Asks** are sell orders. The lowest ask is the *best ask*.
- The gap between the best bid and the best ask is the **spread**.
- Orders at the same price form a **price level**.

When a new order arrives, the engine checks whether it **crosses** the spread
(a buy priced at or above the best ask, or a sell at or below the best bid).
If it does, it trades against resting orders. Any quantity left over either
**rests** in the book (limit orders) or is **discarded** (market orders).

**Price-time priority** decides which resting orders trade first:

1. better price first (the lowest ask for a buyer, the highest bid for a seller), then
2. at the same price, the order that arrived earliest.

Trades print at the **resting** order's price. The resting order was there
first and set the price. The aggressor accepted it, and may get a better price
than its own limit.

---

## 2. One interface, two implementations: concept vs virtual base

Both books satisfy the `OrderBook` concept in `order_book.hpp`. Tests and the
benchmark are templates over the book type, for example
`template <OrderBook Book> bool apply(Book&, const Message&, ...)`.

**Why a concept rather than an abstract base class with virtual functions?**

- With a template, the compiler knows the concrete type at every call, so it
  can inline the calls. A virtual call is an indirect jump that usually cannot
  be inlined. It is cheap, but not free, and it would add the same constant to
  both books, blurring the comparison.
- The concept also documents the interface and gives clear compile errors if
  an implementation is missing a method.

**Trade-off:** with templates you cannot pick the implementation at runtime
from a config file, and each user of the interface is compiled once per type.
A real system that needs to swap implementations at runtime would use
virtuals at a coarse boundary (per batch of messages, not per message).

---

## 3. Why integer ticks

Prices are `std::int64_t` counts of the instrument's **tick size** (the
smallest allowed price increment). With a tick of 0.01, the price 100.25 is
stored as 10025.

- **Exactness.** Binary floating point cannot represent 0.1 or 0.01 exactly.
  `0.1 + 0.2 != 0.3`. Two orders at "the same price" could fail to compare
  equal and end up on different levels, which is a correctness bug.
- **Usable as keys and indices.** Integers hash and compare exactly, and in
  the flat book a price *is* an array index: `levels[price - min_price]`.
- **Speed.** Integer comparisons are cheap and have no special cases (NaN,
  −0.0, denormals).

Converting to and from decimal happens only at the system edge, in the gateway
or market-data decoder, never inside the book. Quantities are integers for the
same reasons.

---

## 4. Storing price levels: `std::map` vs a flat array

### Baseline: `std::map<Price, Level>`

A balanced binary search tree (red-black tree) keyed by price, one per side.

- Insert or find a level: **O(log L)**, where L is the number of levels.
- Best price: `begin()` for asks or the last element for bids. GCC's
  implementation caches both ends, so this is O(1).
- Handles any price, with memory proportional to the number of levels that
  actually exist.
- **Costs:** every level is a separately heap-allocated node. Finding one
  means following a chain of pointers to nodes scattered through memory, and
  each hop can be a cache miss (~5 ns from L2/L3, ~80 ns from DRAM). The
  comparisons along the path are hard for the CPU to predict.

### Optimised: flat array indexed by tick

`std::vector<Level> levels` with one entry per tick in
`[min_price, max_price]`, per side.

- Find a level: **O(1)**, a subtraction and an index.
- Best bid and best ask are stored explicitly:
  - *Adding* can only improve the best price, so it is one comparison.
  - *Emptying the best level* (by a fill or cancel) means scanning outward,
    tick by tick, to the next non-empty level. The cost is the gap in ticks.
    Real books are dense near the touch, so the gap is usually 0–2 ticks.
  - If a side becomes completely empty, we know from `order_count == 0` and
    do not scan at all.
- **Costs:**
  - The price range must be known up front. Our benchmark uses 20,001 ticks
    × 24 bytes × 2 sides ≈ 1 MB, which is fine. A range of millions of ticks
    would waste memory and cache.
  - A sparse book (few levels spread far apart) makes the outward scan slow
    in the worst case.
  - Orders outside the range are rejected.

**Fixes a production system would use:**

- A **hierarchical bitmap** of non-empty levels, so the next level is found
  with a few count-trailing-zeros instructions instead of a scan.
- A **sliding window / ring buffer** of levels that re-centres as the market
  moves, with a fallback `std::map` for the rare far-away order.

**When the map is the right choice:** an instrument with a huge or unbounded
price range, or a book with few orders where simplicity matters more than
nanoseconds.

---

## 5. Orders within a level: FIFO queues

Time priority means each level is a **first-in, first-out queue**. New orders
join the back, and matching takes from the front. We also need to remove an
order from the *middle* of a queue when it is cancelled.

| Container | push back | pop front | erase from middle |
|---|---|---|---|
| `std::vector` | O(1) amortised | O(n) | O(n) |
| `std::deque` | O(1) | O(1) | O(n) |
| `std::list` | O(1) | O(1) | **O(1) given an iterator** |

A doubly linked list is the only option with O(1) erase from the middle.

- **Baseline:** `std::list<Order>`. Each node is a separate heap allocation
  holding the order and the prev/next pointers.
- **Optimised:** an **intrusive** list. `prev` and `next` are fields of the
  `Order` itself, and the `Level` keeps only `head`, `tail` and the total
  quantity. Linking or unlinking an order is a few pointer writes, and the
  order object comes from the pool rather than from `new`.

"Intrusive" means the container's bookkeeping lives inside the element. That
is less general (an order can be in only one list at a time), but it removes a
layer of indirection and an allocation.

---

## 6. Fast cancels: hash map to iterator, or vector by id

A cancel arrives with only an order id. Without an index we would have to
search every level.

### Baseline: `std::unordered_map<OrderId, Location>`

`Location` stores two iterators: one to the order's level in the `std::map`
and one to the order's node in the `std::list`. Cancel is then:

1. hash lookup — O(1) on average
2. `list.erase(order_it)` — O(1)
3. if the level is now empty, `map.erase(level_it)` — amortised O(1), because
   we already hold the iterator and need no O(log L) search.

This only works because `std::map` and `std::list` **never invalidate
iterators to other elements** when you insert or erase. With a `std::vector`
the stored iterators would break.

The costs are a heap-allocated node per entry, a pointer chase through a
bucket, and the occasional **rehash** when the table grows. A rehash
reallocates and moves every entry, which causes a large latency spike.

### Optimised: `std::vector<Order*>` indexed by order id

The lookup is one array index: no hashing, no collisions, no rehash.

- **Trade-off:** ids must be dense integers below a fixed maximum. Exchanges
  often assign ids sequentially per session, which fits. If ids are sparse or
  come from outside, use a preallocated open-addressing hash map with
  `reserve()`d capacity instead. It is still allocation-free, at the cost of
  hashing and probing.
- **Memory:** 8 bytes per possible id (40 MB for 5M ids). Storing 32-bit pool
  indices instead of pointers would halve that.

---

## 7. What the object pool saves

`ObjectPool<T>` allocates `capacity` objects in one `std::vector<T>` up front,
plus a stack of free slot pointers. `allocate()` pops a pointer and
`release()` pushes it back.

Compared with `new`/`delete` (`malloc`/`free`) per order, this saves:

1. **Allocator cost.** A general-purpose allocator has to find a suitable
   free block, handle thread safety and keep its own metadata. That is often
   20–100 ns, with occasional much slower calls when it has to ask the OS for
   more memory. A pool pop is a couple of instructions.
2. **Latency spikes.** Pool time is the same on every call. The allocator's
   worst case is not.
3. **Locality.** Pool objects sit next to each other in one block. Heap nodes
   end up wherever the allocator put them, interleaved with unrelated data,
   so they cause more cache and TLB misses.
4. **Page faults during trading.** The pool's memory is written once at
   construction (the vector value-initialises it), so its pages are already
   mapped before timing starts.

**Trade-offs:**

- Capacity is fixed. When the pool is full, `add_limit` is rejected. We
  reject *before* matching, so a rejected order has no side effects.
- Memory is reserved even when unused.
- Use-after-release is not detected. A released slot can be handed out
  again immediately.

---

## 8. How price-time priority is enforced

Both books run the same matching loop (see `match()` in either `.cpp` file):

```
while incoming quantity > 0 and the opposite side is not empty:
    best = best opposite price
    if incoming does not cross best (buy: best > limit, sell: best < limit): stop
    resting = oldest order at best          <- time priority
    fill = min(incoming remaining, resting remaining)
    emit Trade(aggressor, resting, best, fill)   <- resting order's price
    reduce both; if resting is fully filled, remove it
        (and if its level is now empty, move "best" to the next level)
```

- **Price priority** comes from always reading the *best* opposite level:
  `map.begin()` / last element in the baseline, the tracked `best` in the
  flat book.
- **Time priority** comes from always taking the *front* of the level's FIFO
  queue, and from always appending new orders at the back.
- **Invariant (flat book):** while a side has orders, its `best` level is
  non-empty. Every removal that empties the best level restores this by
  scanning outward. That is why the loop can take `level.head` without
  checking for null.
- A **market order** is a limit order whose limit crosses everything
  (`INT64_MAX` / `INT64_MIN` in the baseline, `max_price` / `min_price` in the
  flat book). Its leftover quantity is simply not rested.

### Modify rules and why

- **Same price, smaller quantity:** reduced in place, keeps its queue
  position. Reducing cannot hurt anyone behind you in the queue.
- **Price change, or quantity increase:** treated as cancel + new order,
  which goes to the back of the queue. Otherwise you could join the queue
  early with a small order and later raise it to a large one, jumping ahead
  of people who committed size earlier. Real exchanges follow the same rule.
- A modify that moves the price across the spread trades immediately, with
  the modified order as the aggressor.

---

## 9. What dominates latency

The benchmark breaks latency down by message outcome (see the README). For the
flat book:

- **Adds that rest (~100 ns including ~45 ns of timer overhead):** a few
  integer operations and writes to an order slot and a level that are usually
  already in cache, because activity concentrates near the touch.
- **Cancels (median similar, but a much longer tail: p99 ~475 ns):** a cancel
  targets a random live order, which may not have been touched for a while.
  Reading `orders_by_id_[id]`, then the order, then its neighbours, can be up
  to three **cache misses**. Cancels are bound by memory latency, not
  computation.
- **Orders that trade (~175 ns):** they do the most work — several fills, one
  trade written per fill, orders removed, and possibly a scan to the next
  level.

For the baseline, the extra cost on every path comes from **heap allocation
and free** (adds and fills) and from **pointer-chasing through tree and hash
nodes** scattered across the heap.

Outside the code:

- **Timer overhead.** A serialised `rdtsc` pair costs ~45 ns here, which is a
  big fraction of a ~65 ns operation. That is why throughput is measured
  separately, with no per-message timing.
- **OS noise** sets the maximums (hundreds of µs or more). Pinning to CPU 0
  on Windows, which services many interrupts, pushed the baseline's p99.9
  from ~2 µs to ~52 µs. Production systems isolate cores (`isolcpus`, IRQ
  affinity), disable frequency scaling and busy-poll instead of sleeping.

Further ways to shrink the flat book:

- shrink `Order` from 48 to 32 bytes (32-bit price, quantity and indices) so
  more fits in cache
- bitmap-based next-level search
- emit trades through a templated callback instead of a vector
- prefetch the order on cancel

---

## 10. Testing strategy

1. **Unit tests** pin down the rules on small, hand-checked scenarios. Every
   test is a template, registered once per implementation, so both books are
   held to the same specification.
2. **Differential testing.** The baseline is simple enough to trust once its
   unit tests pass. The fast book is then checked against it on 1.4M random
   messages: same return value, same trades in the same order, and identical
   snapshots (every level and every order in queue order). This catches
   edge-case bugs that hand-written tests miss, such as best-price
   bookkeeping after a particular sequence of fills and cancels.
3. **Testing the test.** A deliberately planted bug (quantity increases
   keeping priority) was caught at message 345, which shows the differential
   test is sensitive.

The generator feeds a shadow `MapOrderBook` so that cancels and modifies
mostly target orders that are actually live. About 5% of cancels target
possibly-dead ids, to exercise the "unknown id" path the way real cancel/fill
races do. It uses its own modulo-based draws on `std::mt19937_64` rather than
`std::uniform_int_distribution`, whose output differs between standard
libraries. A seed therefore produces the same stream on every platform.

---

## 11. Benchmark methodology

- **Generate first, measure second.** All messages are created before timing.
- **Warm-up.** The first 10% of messages are untimed. This brings the book to
  its steady-state depth (~2k orders) and warms caches, the branch predictor
  and the allocator.
- **Throughput and latency are measured separately.** Throughput is one
  `steady_clock` interval over 4.5M messages (median of 5 runs). Latency
  timestamps every message with `lfence; rdtsc; lfence`. `rdtsc` costs a few
  ns, while `steady_clock` on Windows has only 100 ns resolution. The TSC
  ticks at a constant rate on modern CPUs and is calibrated against
  `steady_clock`.
- **No allocation while timing.** The latency array is allocated and
  zero-filled beforehand, and the trade vector has reserved capacity.
- **Pinning** to one core prevents migration between performance and
  efficiency cores on this hybrid CPU.
- **Caveat:** this measures *service time* per message on a single thread. It
  does not model a queue of messages arriving at a fixed rate, so it says
  nothing about queueing delay under load (the "coordinated omission"
  problem).

---

## 12. Scaling up

### Multiple instruments

- One book per instrument. Map the instrument symbol to a dense integer index
  once at the gateway, and keep the books in a `std::vector<Book>`.
- Instruments are independent, so shard them across cores. Each core runs
  its own single-threaded matching loop over its own books, with no locks.
- Size each flat book per instrument: its tick size, its price band (from
  exchange price limits) and its expected order count. A very wide-range or
  illiquid instrument might use the map-based book instead.
- Memory adds up quickly (levels + pool + id table per book). Smaller
  `Order`s and pool indices instead of pointers start to matter.

### Multiple threads

- **Do not put a mutex around the book.** Matching is a strictly ordered
  sequence of state changes, and the order of messages *is* the semantics.
  Locks add contention and make the order nondeterministic.
- Use the **single-writer principle**: exactly one thread owns each book.
  Other threads talk to it through queues.
  - The network/gateway thread decodes messages and pushes them onto a
    **lock-free single-producer/single-consumer ring buffer**.
  - The matching thread pops, applies and pushes trades and acknowledgements
    onto an outbound ring buffer.
  - Market-data and risk threads consume that output.
- Watch for **false sharing**: put the producer and consumer indices of each
  ring buffer on separate cache lines (`alignas(64)`).
- Pin threads to isolated cores and busy-poll, so that a sleeping thread does
  not pay a wake-up delay in the microseconds.
- **Determinism** is a feature: a single sequenced input stream lets you
  replay a day's messages and reproduce every trade exactly. That is useful
  for debugging, auditing and failover to a replica.

---

## 13. Likely interview questions

**Q: What is the time complexity of add, cancel and match in each version?**
A: Baseline: add O(log L) to find or create the level, plus O(1) to append;
cancel O(1) on average (hash lookup, then erase via stored iterators); each
fill O(1) plus O(log L) amortised level removal. Flat: add O(1); cancel O(1);
each fill O(1); the only non-constant step is the scan to the next best level
when the best level empties, which is O(gap in ticks).

**Q: Why not use `double` for prices?**
A: Floating point cannot represent most decimal prices exactly, so equal
prices can compare unequal. Integer ticks are exact, hash and compare cheaply,
and work directly as array indices.

**Q: Why a linked list for a level and not a vector?**
A: Cancels remove orders from the middle of the queue. A list does that in
O(1) given an iterator or pointer, and other elements never move, so stored
references stay valid. A vector would shift elements (O(n)) and invalidate
the stored positions.

**Q: Why store iterators in the hash map rather than just the price?**
A: With only the price we would need an O(log L) map lookup and then an O(n)
scan of the level to find the order. Iterators go straight to the node.
`std::map` and `std::list` guarantee that iterators stay valid when other
elements are inserted or erased.

**Q: What does the object pool buy you, and what does it cost?**
A: It removes allocator calls from the hot path, which gives faster and more
predictable latency and better locality, and it avoids page faults during
trading. The cost is a fixed capacity decided up front, memory reserved even
when unused, and the need to handle "pool full" (we reject the order).

**Q: How do you find the new best price when the best level empties?**
A: Scan outward tick by tick until a non-empty level appears. It is cheap
because books are dense near the touch, and we skip it entirely if the side
is empty. For sparse books, use a hierarchical bitmap and find-first-set.

**Q: What happens if the market moves outside your flat array's range?**
A: Here, the order is rejected. In production you would re-centre a sliding
window of levels, or keep a fallback tree for out-of-band prices, with the
range set from the exchange's price limits.

**Q: How do you know the optimised book is correct?**
A: The same unit tests run against both books, and a randomised differential
test compares every trade and the full book state with the baseline over
1.4M messages. I also planted a bug to confirm the differential test catches
it.

**Q: Why is a cancel's tail latency worse than an add's in the flat book?**
A: A cancel touches a random, possibly long-idle order. The id table entry,
the order and its neighbours may all be cache misses. Adds land near the
touch on levels that are already in cache.

**Q: Why does a quantity increase lose time priority but a decrease does not?**
A: Fairness. A decrease cannot disadvantage anyone queued behind you. An
increase would let you reserve a queue place with a small order and add size
later, jumping ahead of people who committed size earlier.

**Q: How would you make this multi-threaded?**
A: Keep each book single-threaded. One thread owns each book (or a shard of
instruments), with lock-free SPSC queues in and out. Locks would serialise
the work anyway, add contention, and make message order (and therefore
results) nondeterministic.

**Q: How did you measure latency, and what are the pitfalls?**
A: `rdtsc` around each message into a preallocated array, after a warm-up,
pinned to one core, with throughput measured in a separate untimed-per-message
pass. The pitfalls are timer overhead (~45 ns here, reported), OS interrupts
dominating the maximum (core choice changed p99.9 25×), frequency scaling,
allocation inside the timed region, and coordinated omission when modelling
arrival rates.

**Q: Why did `-O3 -march=native` not help?**
A: The work is pointer-chasing, branches and memory access, with almost no
vectorisable arithmetic. Faster code generation cannot hide cache misses.
Data layout (smaller orders, contiguous storage) matters far more.

**Q: Why use a concept instead of a virtual interface?**
A: The compiler sees the concrete type, so calls are inlined and there is no
indirect-call overhead in the measurements. The price is that the
implementation has to be chosen at compile time.
