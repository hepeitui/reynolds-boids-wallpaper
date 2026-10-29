// Build (MSVC): cl /std:c++20 /O2 /EHsc /I native native\benchmark.cpp native\simulation.cpp /Fe:benchmark.exe
// Build (GCC/Clang): c++ -std=c++20 -O2 -pthread -I native native/benchmark.cpp native/simulation.cpp -o benchmark
// Reports CPU simulation-step timings only; this is not a rendered-FPS benchmark.
#include "simulation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace flock;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t BenchmarkSeed = 0x21192026u;
constexpr std::size_t DefaultSteps = 600;
constexpr std::size_t WarmupSteps = 30;

struct Options {
    std::size_t steps = DefaultSteps;
    std::optional<std::string> outputPath;
    bool randomColor = false;
    unsigned threads = 0;
    std::vector<int> counts;
    bool help = false;
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

std::size_t parseSteps(std::string_view value) {
    std::size_t result = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result == 0)
        fail("--steps expects a positive integer");
    return result;
}

unsigned parseThreads(std::string_view value) {
    unsigned result = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result > 8)
        fail("--threads expects an integer in 0..8 (0 for auto)");
    return result;
}

void parseCounts(std::string_view value, std::vector<int>& counts) {
    if (value.empty() || value.back() == ',')
        fail("--count expects a nonempty list of positive bird counts");
    std::size_t start = 0;
    while (start < value.size()) {
        std::size_t comma = value.find(',', start);
        if (comma == std::string_view::npos) comma = value.size();
        std::string_view item = value.substr(start, comma - start);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item.empty()) fail("--count item must not be empty");
        int countVal = 0;
        const char* begin = item.data();
        const char* end = begin + item.size();
        const auto parsed = std::from_chars(begin, end, countVal);
        if (parsed.ec != std::errc{} || parsed.ptr != end || countVal <= 0)
            fail("--count expects positive integer bird counts");
        counts.push_back(countVal);
        start = comma + 1;
    }
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--random-color") {
            options.randomColor = true;
        } else if (argument == "--steps") {
            if (++i >= argc) fail("--steps requires a value");
            options.steps = parseSteps(argv[i]);
        } else if (argument == "--output") {
            if (++i >= argc) fail("--output requires a file path");
            options.outputPath = argv[i];
            if (options.outputPath->empty()) fail("--output path must not be empty");
        } else if (argument == "--threads") {
            if (++i >= argc) fail("--threads requires a value");
            options.threads = parseThreads(argv[i]);
        } else if (argument == "--count") {
            if (++i >= argc) fail("--count requires a value");
            parseCounts(argv[i], options.counts);
        } else {
            std::ostringstream message;
            message << "unknown argument: " << argument;
            fail(message.str());
        }
    }
    return options;
}

class FixedRandom {
public:
    explicit FixedRandom(std::uint32_t seed) : state_(seed ? seed : 1u) {}

    std::uint32_t next() {
        std::uint32_t value = state_;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        state_ = value;
        return value;
    }

    float unit() {
        return static_cast<float>(static_cast<double>(next()) / 4294967296.0);
    }

private:
    std::uint32_t state_;
};

struct Scenario {
    const char* name;
    int width;
    int height;
    bool dense;
};

struct Sample {
    int count;
    const Scenario* scenario;
    double totalMs;
    double p50Ms;
    double p95Ms;
    double p99Ms;
    std::uint64_t candidates;
    double candidatesPerStep;
    unsigned requestedWorkers;
    unsigned configuredWorkers;
};

double percentile(const std::vector<double>& sortedSamples, double fraction) {
    if (sortedSamples.empty()) return 0.0;
    const double rank = std::ceil(fraction * static_cast<double>(sortedSamples.size()));
    const std::size_t index = std::min(sortedSamples.size() - 1,
        static_cast<std::size_t>(std::max(1.0, rank)) - 1);
    return sortedSamples[index];
}

std::vector<Bird> makeBirds(int count, const Scenario& scenario) {
    FixedRandom random(BenchmarkSeed);
    std::vector<Bird> birds;
    birds.reserve(static_cast<std::size_t>(count));
    const float centerX = static_cast<float>(scenario.width) * .5f;
    const float centerY = static_cast<float>(scenario.height) * .5f;
    constexpr float DenseHalfExtent = 110.f;
    constexpr double Tau = 6.283185307179586476925286766559;
    for (int i = 0; i < count; ++i) {
        const float x = scenario.dense
            ? centerX + (random.unit() * 2.f - 1.f) * DenseHalfExtent
            : 24.f + random.unit() * (static_cast<float>(scenario.width) - 48.f);
        const float y = scenario.dense
            ? centerY + (random.unit() * 2.f - 1.f) * DenseHalfExtent
            : 24.f + random.unit() * (static_cast<float>(scenario.height) - 48.f);
        const double angle = static_cast<double>(random.unit()) * Tau;
        const float speed = 87.f;
        Bird bird{};
        bird.id = static_cast<std::uint32_t>(i + 1);
        bird.phase = Phase::Active;
        bird.position = {x, y};
        bird.velocity = {static_cast<float>(std::cos(angle) * speed),
                         static_cast<float>(std::sin(angle) * speed)};
        bird.speedBias = random.unit() * 2.f - 1.f;
        birds.push_back(bird);
    }
    return birds;
}

Sample runSample(Simulation& simulation, const Scenario& scenario, int count,
                 std::size_t steps, bool randomColor, unsigned requestedWorkers) {
    const std::vector<Bird> birds = makeBirds(count, scenario);
    Settings settings{};
    settings.count = count;
    settings.randomColor = randomColor;
    settings.avoidTaskbar = false;
    settings.marginLeft = settings.marginRight = 0.f;
    settings.marginTop = settings.marginBottom = 0.f;
    const Bounds bounds{0.f, 0.f, static_cast<float>(scenario.width),
                        static_cast<float>(scenario.height)};
    simulation.configure(settings, bounds);
    simulation.restore(std::span<const Bird>(birds.data(), birds.size()));

    constexpr float Dt = 1.f / 60.f;
    for (std::size_t i = 0; i < WarmupSteps; ++i) simulation.step(Dt);

    std::vector<double> stepMs;
    stepMs.reserve(steps);
    std::uint64_t candidates = 0;
    const auto totalStart = Clock::now();
    for (std::size_t i = 0; i < steps; ++i) {
        const auto stepStart = Clock::now();
        simulation.step(Dt);
        const auto stepEnd = Clock::now();
        stepMs.push_back(std::chrono::duration<double, std::milli>(stepEnd - stepStart).count());
        candidates += simulation.stats().candidates;
    }
    const auto totalEnd = Clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
    std::sort(stepMs.begin(), stepMs.end());
    const double p50 = percentile(stepMs, .50);
    const double p95 = percentile(stepMs, .95);
    const double p99 = percentile(stepMs, .99);
    return Sample{count, &scenario, totalMs, p50, p95, p99, candidates,
                  static_cast<double>(candidates) / static_cast<double>(steps),
                  requestedWorkers, simulation.workerCount()};
}

void writeHeader(std::ostream& output) {
    output << "scenario,count,width,height,seed,warmup_steps,measured_steps,random_color,"
              "simulation_total_ms,step_p50_ms,step_p95_ms,step_p99_ms,"
              "candidate_total,candidate_per_step,requested_workers,configured_workers\n";
}

void writeSample(std::ostream& output, const Sample& sample, std::size_t steps,
                 bool randomColor) {
    output << sample.scenario->name << ',' << sample.count << ','
           << sample.scenario->width << ',' << sample.scenario->height << ','
           << BenchmarkSeed << ',' << WarmupSteps << ',' << steps << ','
           << (randomColor ? "true" : "false") << ','
           << std::fixed << std::setprecision(3) << sample.totalMs << ','
           << sample.p50Ms << ',' << sample.p95Ms << ',' << sample.p99Ms << ','
           << sample.candidates << ',' << std::setprecision(2)
           << sample.candidatesPerStep << ','
           << sample.requestedWorkers << ','
           << sample.configuredWorkers << '\n';
}

void printUsage(std::ostream& output) {
    output << "Usage: benchmark [--steps N] [--output FILE] [--random-color] [--threads N] [--count N[,N...]]\n"
              "  --steps N          measured fixed simulation steps per scenario (default 600)\n"
              "  --output FILE      write CSV to FILE instead of stdout\n"
              "  --random-color     enable flock color grouping while measuring\n"
              "  --threads N        simulation worker count (0 auto, 1 serial, 2..8 max workers; default 0)\n"
              "  --count N          bird count to measure (can be repeated or comma-separated; default 500,1500,3000)\n"
              "                     Note: count cannot exceed compile-time MaxBirds (" << MaxBirds << ")\n"
              "  --help             show this help\n"
              "Runs benchmark scenarios in uniform 1440x900, uniform 1920x1080,\n"
              "and dense 220x220-core distributions. Reports simulation timings, not FPS.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.help) {
            printUsage(std::cout);
            return EXIT_SUCCESS;
        }

        std::ofstream file;
        std::ostream* output = &std::cout;
        if (options.outputPath) {
            file.open(*options.outputPath, std::ios::out | std::ios::trunc);
            if (!file) {
                std::ostringstream message;
                message << "cannot open output file: " << *options.outputPath;
                fail(message.str());
            }
            output = &file;
        }

        const std::vector<int> populations = options.counts.empty()
            ? std::vector<int>{500, 1500, 3000}
            : options.counts;

        for (int count : populations) {
            if (count > MaxBirds) {
                std::ostringstream message;
                message << "requested bird count " << count << " exceeds MaxBirds (" << MaxBirds << ")";
                fail(message.str());
            }
        }

        constexpr std::array<Scenario, 3> scenarios{{
            {"uniform-1440x900", 1440, 900, false},
            {"uniform-1920x1080", 1920, 1080, false},
            {"dense-core-1440x900", 1440, 900, true}
        }};
        writeHeader(*output);
        for (const Scenario& scenario : scenarios) {
            for (int count : populations) {
                Simulation simulation(BenchmarkSeed, options.threads);
                const Sample sample = runSample(simulation, scenario, count,
                                                 options.steps, options.randomColor,
                                                 options.threads);
                writeSample(*output, sample, options.steps, options.randomColor);
                output->flush();
                if (!*output) fail("failed while writing benchmark output");
                if (options.outputPath)
                    std::cerr << "completed " << scenario.name << " / " << count << '\n';
            }
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
