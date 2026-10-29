#pragma once
#include "model.hpp"
#include <memory>
namespace flock {
class Simulation {
public:
    // Simulation is single-owner: external concurrent calls to any member function
    // (step, configure, reset, restore, views) are not supported.
    // Threading controls internal parallelization during step():
    // workers=0: auto (hardware concurrency, conservative maximum 8, including calling thread)
    // workers=1: forced serial
    // workers=2..8: requested participant count upper bound (may clamp to hardware concurrency)
    explicit Simulation(std::uint32_t seed=42, unsigned workers=0);
    ~Simulation();
    Simulation(const Simulation&)=delete;
    Simulation& operator=(const Simulation&)=delete;

    // Configured total participant count (including calling thread).
    // Serial execution under small populations is an internal dispatch detail.
    unsigned workerCount() const noexcept;
    // Exclusions are taskbar rectangles inside a spanned desktop, in the same coordinates.
    // Empty is a rectangular activity area. Configuration copies this non-owning input.
    void configure(const Settings& settings, Bounds bounds,
                   std::span<const Bounds> exclusions = {});
    void step(float dt, const Input& input={}, bool paused=false);
    void reset(std::uint32_t seed=42);
    // Stable slot-index views: skip Phase::Dead birds and empty ghost histories.
    // Views remain valid until destruction; values change on configure/step/reset/restore.
    std::span<const Bird> birds() const;
    std::span<const Ghost> ghosts() const;
    const Settings& settings() const;
    Bounds bounds() const;
    // Epoch-relative animation clock, periodically rebased for long-running precision.
    // Use differences with history/boost timestamps; use a host clock for process uptime.
    float time() const;
    SimStats stats() const;
    // Deterministic state injection for reproducing behavioral edge cases.
    // Replaces population and history, does not bypass normal stepping logic.
    void restore(std::span<const Bird> birds, float time=0);
    // Exact radius result for diagnostics, writes into caller-owned storage.
    // Returns the number written; provide MaxBirds entries for the complete result.
    int neighbors(std::size_t index, std::span<int> output) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
