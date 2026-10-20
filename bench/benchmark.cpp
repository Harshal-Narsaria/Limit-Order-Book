// Throughput and latency benchmark for both order book implementations.
//
// Method:
//   1. Generate the whole message stream up front (seeded, reproducible), so
//      generation cost is never measured.
//   2. For each implementation, build a fresh book and replay a warm-up prefix
//      untimed. This brings the book to a steady-state depth and warms caches
//      and branch predictors.
//   3. Throughput: time the remaining messages as one interval with
//      steady_clock (no per-message timer calls to distort the result).
//      Repeated several times; the median is reported.
//   4. Latency: a separate pass that timestamps every message with the CPU
//      timestamp counter (rdtsc, a few ns per read) into a preallocated array,
//      so nothing is allocated while timing. Percentiles are computed after.
//
// Usage: lob_benchmark [message_count] [--cpu N]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lob/flat_order_book.hpp"
#include "lob/map_order_book.hpp"
#include "lob/message.hpp"
#include "lob/message_generator.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define LOB_HAS_TSC 1
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#ifndef LOB_BUILD_FLAGS
#define LOB_BUILD_FLAGS "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;

// --- Timing primitives -----------------------------------------------------------

// Reads a monotonically increasing tick counter. On x86 this is the TSC, which
// on modern CPUs ticks at a constant rate regardless of frequency scaling.
// lfence stops the CPU from reordering the read around the code being timed.
inline std::uint64_t read_ticks() {
#ifdef LOB_HAS_TSC
    _mm_lfence();
    const std::uint64_t ticks = __rdtsc();
    _mm_lfence();
    return ticks;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
#endif
}

// Measures how many ticks elapse per nanosecond of steady_clock time.
double calibrate_ticks_per_ns() {
    const auto wall_start = Clock::now();
    const std::uint64_t tick_start = read_ticks();
    while (Clock::now() - wall_start < std::chrono::milliseconds(300)) {
    }
    const std::uint64_t tick_end = read_ticks();
    const auto wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall_start).count();
    return static_cast<double>(tick_end - tick_start) / static_cast<double>(wall_ns);
}

// Median cost of an empty start/stop timer pair, i.e. the floor of any latency sample.
std::uint64_t timer_overhead_ticks() {
    std::vector<std::uint64_t> samples(100'000);
    for (auto& sample : samples) {
        const std::uint64_t start = read_ticks();
        sample = read_ticks() - start;
    }
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}

// Keeps the benchmark on one core so the OS does not migrate it mid-run
// (this matters on hybrid CPUs with performance and efficiency cores).
// The default is core 2 rather than 0: on Windows, core 0 services a large
// share of interrupts, which showed up as ~50us spikes at p99.9.
void pin_to_cpu(int cpu) {
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

// --- Statistics ------------------------------------------------------------------

struct LatencyStats {
    std::size_t samples = 0;
    double mean_ns = 0;
    double p50_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;
    double max_ns = 0;
};

LatencyStats summarise(std::vector<std::uint64_t> ticks, double ticks_per_ns) {
    LatencyStats stats;
    if (ticks.empty()) {
        return stats;
    }
    std::sort(ticks.begin(), ticks.end());
    auto percentile = [&](double q) {
        const auto index = std::min(ticks.size() - 1, static_cast<std::size_t>(q * static_cast<double>(ticks.size())));
        return static_cast<double>(ticks[index]) / ticks_per_ns;
    };
    double total = 0;
    for (std::uint64_t t : ticks) {
        total += static_cast<double>(t);
    }
    stats.samples = ticks.size();
    stats.mean_ns = total / static_cast<double>(ticks.size()) / ticks_per_ns;
    stats.p50_ns = percentile(0.50);
    stats.p99_ns = percentile(0.99);
    stats.p999_ns = percentile(0.999);
    stats.max_ns = static_cast<double>(ticks.back()) / ticks_per_ns;
    return stats;
}

// Latency is broken down by what the message did, to show where time goes.
enum Category : std::uint8_t { PassiveAdd, Cancel, Aggressive, Other, CategoryCount };
constexpr const char* category_names[CategoryCount] = {"add (rests, no fill)", "cancel",
                                                       "order that traded", "other"};

Category classify(const lob::Message& message, std::size_t trade_count) {
    if (trade_count > 0) {
        return Aggressive;
    }
    if (message.type == lob::MessageType::Cancel) {
        return Cancel;
    }
    if (message.type == lob::MessageType::AddLimit) {
        return PassiveAdd;
    }
    return Other;
}

// --- Benchmark driver ------------------------------------------------------------

struct Result {
    std::string name;
    double median_throughput = 0;  // messages per second
    LatencyStats overall;
    LatencyStats by_category[CategoryCount];
    std::size_t trade_count = 0;
    std::size_t resting_at_end = 0;
};

constexpr int kThroughputRepetitions = 5;
constexpr std::size_t kTradeReserve = 4096;  // > the largest possible sweep in our flow

template <typename MakeBook>
Result run_benchmark(const std::string& name, MakeBook make_book,
                     const std::vector<lob::Message>& messages, std::size_t warmup,
                     double ticks_per_ns) {
    Result result;
    result.name = name;
    const std::size_t measured = messages.size() - warmup;
    std::vector<lob::Trade> trades;
    trades.reserve(kTradeReserve);

    // Throughput passes.
    std::vector<double> throughputs;
    for (int rep = 0; rep < kThroughputRepetitions; ++rep) {
        auto book = make_book();
        for (std::size_t i = 0; i < warmup; ++i) {
            trades.clear();
            lob::apply(book, messages[i], trades);
        }
        std::size_t trade_count = 0;
        const auto start = Clock::now();
        for (std::size_t i = warmup; i < messages.size(); ++i) {
            trades.clear();
            lob::apply(book, messages[i], trades);
            trade_count += trades.size();  // also stops the work being optimised away
        }
        const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        throughputs.push_back(static_cast<double>(measured) / elapsed);
        result.trade_count = trade_count;
    }
    std::sort(throughputs.begin(), throughputs.end());
    result.median_throughput = throughputs[throughputs.size() / 2];

    // Latency pass. Both arrays are allocated and touched before timing starts.
    std::vector<std::uint64_t> latency_ticks(measured, 0);
    std::vector<std::uint8_t> categories(measured, 0);
    {
        auto book = make_book();
        for (std::size_t i = 0; i < warmup; ++i) {
            trades.clear();
            lob::apply(book, messages[i], trades);
        }
        for (std::size_t j = 0; j < measured; ++j) {
            const lob::Message& message = messages[warmup + j];
            trades.clear();
            const std::uint64_t start = read_ticks();
            lob::apply(book, message, trades);
            const std::uint64_t stop = read_ticks();
            latency_ticks[j] = stop - start;
            categories[j] = classify(message, trades.size());
        }
        result.resting_at_end = book.order_count();
    }

    // Post-processing (allocation is fine from here on).
    std::vector<std::uint64_t> split[CategoryCount];
    for (std::size_t j = 0; j < measured; ++j) {
        split[categories[j]].push_back(latency_ticks[j]);
    }
    for (int c = 0; c < CategoryCount; ++c) {
        result.by_category[c] = summarise(std::move(split[c]), ticks_per_ns);
    }
    result.overall = summarise(std::move(latency_ticks), ticks_per_ns);
    return result;
}

void print_results(const std::vector<Result>& results) {
    std::printf("\n%-18s %14s %9s %8s %8s %9s %10s\n", "implementation", "throughput", "mean", "p50",
                "p99", "p99.9", "max");
    std::printf("%-18s %14s %9s %8s %8s %9s %10s\n", "", "(M msg/s)", "(ns)", "(ns)", "(ns)", "(ns)",
                "(ns)");
    for (const Result& r : results) {
        std::printf("%-18s %14.2f %9.1f %8.0f %8.0f %9.0f %10.0f\n", r.name.c_str(),
                    r.median_throughput / 1e6, r.overall.mean_ns, r.overall.p50_ns, r.overall.p99_ns,
                    r.overall.p999_ns, r.overall.max_ns);
    }
    if (results.size() == 2) {
        const Result& base = results[0];
        const Result& fast = results[1];
        std::printf("%-18s %13.2fx %8.2fx %7.2fx %7.2fx %8.2fx\n", "speedup",
                    fast.median_throughput / base.median_throughput,
                    base.overall.mean_ns / fast.overall.mean_ns, base.overall.p50_ns / fast.overall.p50_ns,
                    base.overall.p99_ns / fast.overall.p99_ns, base.overall.p999_ns / fast.overall.p999_ns);
    }

    std::printf("\nLatency by message outcome (ns):\n");
    std::printf("%-18s %-22s %9s %8s %8s %8s %9s\n", "implementation", "outcome", "share", "mean", "p50",
                "p99", "p99.9");
    for (const Result& r : results) {
        for (int c = 0; c < CategoryCount; ++c) {
            const LatencyStats& s = r.by_category[c];
            if (s.samples == 0) {
                continue;
            }
            const double share = 100.0 * static_cast<double>(s.samples) / static_cast<double>(r.overall.samples);
            std::printf("%-18s %-22s %8.1f%% %8.1f %8.0f %8.0f %9.0f\n", r.name.c_str(), category_names[c],
                        share, s.mean_ns, s.p50_ns, s.p99_ns, s.p999_ns);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t message_count = 5'000'000;
    int cpu = 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            cpu = std::atoi(argv[++i]);
        } else {
            message_count = std::strtoull(argv[i], nullptr, 10);
        }
    }
    const std::size_t warmup = message_count / 10;

    pin_to_cpu(cpu);

    const lob::GeneratorConfig config{.seed = 42, .message_count = message_count};
    std::printf("Generating %zu messages (seed %llu: %d%% add, %d%% cancel, %d%% aggressive)...\n",
                message_count, static_cast<unsigned long long>(config.seed), config.add_percent,
                config.cancel_percent, 100 - config.add_percent - config.cancel_percent - config.modify_percent);
    const std::vector<lob::Message> messages = lob::generate_messages(config);

    const double ticks_per_ns = calibrate_ticks_per_ns();
    const double overhead_ns = static_cast<double>(timer_overhead_ticks()) / ticks_per_ns;
    std::printf("Compiler: %s, flags: %s\n", __VERSION__, LOB_BUILD_FLAGS);
    std::printf("Pinned to CPU %d. Timer: %.3f ticks/ns, empty timer pair ~%.1f ns (included in latency figures)\n",
                cpu, ticks_per_ns, overhead_ns);
    std::printf("Warm-up %zu messages, measured %zu, throughput = median of %d runs\n", warmup,
                message_count - warmup, kThroughputRepetitions);

    const lob::FlatOrderBookConfig flat_config{.min_price = 0,
                                               .max_price = 20'000,
                                               .max_orders = 1 << 20,
                                               .max_order_id = message_count + 2};

    std::vector<Result> results;
    results.push_back(run_benchmark("map (baseline)", [] { return lob::MapOrderBook{}; }, messages,
                                    warmup, ticks_per_ns));
    results.push_back(run_benchmark("flat (optimised)", [&] { return lob::FlatOrderBook{flat_config}; },
                                    messages, warmup, ticks_per_ns));

    if (results[0].trade_count != results[1].trade_count) {
        std::printf("ERROR: implementations produced different trade counts (%zu vs %zu)\n",
                    results[0].trade_count, results[1].trade_count);
        return 1;
    }
    std::printf("Both implementations produced %zu trades in the measured window; %zu orders resting at the end.\n",
                results[0].trade_count, results[0].resting_at_end);
    print_results(results);
    return 0;
}
