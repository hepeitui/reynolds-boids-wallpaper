// Build (MSVC): cl /std:c++20 /O2 /EHsc /I native native\verify.cpp native\simulation.cpp /Fe:verify.exe
// Build (GCC/Clang): c++ -std=c++20 -O2 -pthread -I native native/verify.cpp native/simulation.cpp -o verify
#include "simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace flock;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void near(double actual, double expected, double tolerance, std::string_view label) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::ostringstream out;
        out << label << ": expected " << expected << " +/- " << tolerance << ", got " << actual;
        fail(out.str());
    }
}

bool validNeighborPhase(Phase phase) {
    return phase == Phase::Entering || phase == Phase::Active;
}
std::size_t liveBirdCount(std::span<const Bird> birds) {
    return static_cast<std::size_t>(std::count_if(birds.begin(), birds.end(),
        [](const Bird& bird) { return bird.phase != Phase::Dead; }));
}

const Bird* findBird(std::span<const Bird> birds, std::uint32_t id) {
    const auto found = std::find_if(birds.begin(), birds.end(),
        [id](const Bird& bird) { return bird.phase != Phase::Dead && bird.id == id; });
    return found == birds.end() ? nullptr : &*found;
}

const Bird* firstLiveBird(std::span<const Bird> birds) {
    const auto found = std::find_if(birds.begin(), birds.end(),
        [](const Bird& bird) { return bird.phase != Phase::Dead; });
    return found == birds.end() ? nullptr : &*found;
}

std::size_t slotForBird(std::span<const Bird> birds, std::uint32_t id) {
    for (std::size_t slot = 0; slot < birds.size(); ++slot)
        if (birds[slot].phase != Phase::Dead && birds[slot].id == id) return slot;
    fail("requested bird ID is not present in the live slot view");
}

const Bird& requireBird(std::span<const Bird> birds, std::uint32_t id) {
    const Bird* found = findBird(birds, id);
    if (!found) fail("requested bird ID is not present in the live slot view");
    return *found;
}

const Bird& requireFirstLiveBird(std::span<const Bird> birds) {
    const Bird* found = firstLiveBird(birds);
    if (!found) fail("expected a live bird, but the slot view contains none");
    return *found;
}

void requireLiveCount(std::span<const Bird> birds, std::size_t expected,
                      std::string_view message) {
    if (liveBirdCount(birds) != expected) fail(message);
}

Settings quietSettings(int count, float speed = 100.f) {
    Settings settings{};
    settings.count = count;
    settings.speed = speed;
    settings.speedError = 0.f;
    settings.separation = 0.f;
    settings.alignment = 0.f;
    settings.cohesion = 0.f;
    settings.edge = 0.f;
    settings.trails = true;
    settings.trailDuration = .28f;
    settings.randomColor = false;
    settings.avoidTaskbar = false;
    settings.marginLeft = settings.marginRight = 0.f;
    settings.marginTop = settings.marginBottom = 0.f;
    return settings;
}

Bird makeBird(std::uint32_t id, Phase phase, Vec2 position, Vec2 velocity,
              float speedBias = 0.f) {
    Bird bird{};
    bird.id = id;
    bird.phase = phase;
    bird.position = position;
    bird.velocity = velocity;
    bird.speedBias = speedBias;
    bird.baseColor = Color{.518f, .847f, .878f};
    bird.color = bird.baseColor;
    return bird;
}

bool sameVec(Vec2 a, Vec2 b, double tolerance = 0.0) {
    return std::abs(static_cast<double>(a.x) - b.x) <= tolerance &&
           std::abs(static_cast<double>(a.y) - b.y) <= tolerance;
}

bool sameColor(Color a, Color b, double tolerance = 0.0) {
    return std::abs(static_cast<double>(a.r) - b.r) <= tolerance &&
           std::abs(static_cast<double>(a.g) - b.g) <= tolerance &&
           std::abs(static_cast<double>(a.b) - b.b) <= tolerance;
}

bool sameTrailPoint(const TrailPoint& a, const TrailPoint& b) {
    return sameVec(a.position, b.position) && a.time == b.time && sameColor(a.color, b.color);
}

bool sameBirdState(const Bird& a, const Bird& b) {
    if (!sameVec(a.position, b.position) || !sameVec(a.velocity, b.velocity) ||
        a.id != b.id || a.phase != b.phase || a.speedBias != b.speedBias ||
        a.phaseTime != b.phaseTime || a.boostUntil != b.boostUntil ||
        a.wingPhase != b.wingPhase || !sameColor(a.baseColor, b.baseColor) ||
        !sameColor(a.color, b.color) || a.historyHead != b.historyHead ||
        a.historyCount != b.historyCount || a.lastSample != b.lastSample) return false;
    for (std::size_t i = 0; i < a.history.size(); ++i)
        if (!sameTrailPoint(a.history[i], b.history[i])) return false;
    return true;
}

bool sameGhost(const Ghost& a, const Ghost& b) {
    if (a.historyHead != b.historyHead || a.historyCount != b.historyCount ||
        a.retiredAt != b.retiredAt) return false;
    for (std::size_t i = 0; i < a.history.size(); ++i)
        if (!sameTrailPoint(a.history[i], b.history[i])) return false;
    return true;
}
std::size_t visibleGhostCount(std::span<const Ghost> ghosts) {
    return static_cast<std::size_t>(std::count_if(ghosts.begin(), ghosts.end(),
        [](const Ghost& ghost) { return ghost.historyCount > 1; }));
}


bool sameStats(const SimStats& a, const SimStats& b) {
    return a.active == b.active && a.entering == b.entering &&
           a.leaving == b.leaving && a.dying == b.dying &&
           a.candidates == b.candidates && a.simulationMs == b.simulationMs;
}

void verifyNeighborsAgainstOracle() {
    Simulation simulation(0x2119u);
    Settings settings = quietSettings(7);
    const Bounds bounds{0.f, 0.f, 300.f, 200.f};
    simulation.configure(settings, bounds);

    std::vector<Bird> birds;
    birds.push_back(makeBird(1, Phase::Active, {-100.f, -200.f}, {1.f, 0.f}));
    birds.push_back(makeBird(2, Phase::Entering, {-42.f, -200.f}, {1.f, 0.f})); // exactly R
    birds.push_back(makeBird(3, Phase::Active, {-41.999f, -200.f}, {1.f, 0.f})); // just beyond R
    birds.push_back(makeBird(4, Phase::Active, {-100.f, -142.f}, {1.f, 0.f})); // exactly R
    birds.push_back(makeBird(5, Phase::Active, {-100.f, -141.999f}, {1.f, 0.f})); // just beyond R
    birds.push_back(makeBird(6, Phase::Leaving, {-99.f, -200.f}, {1.f, 0.f}));
    birds.push_back(makeBird(7, Phase::Dying, {-100.f, -199.f}, {1.f, 0.f}));
    simulation.restore(std::span<const Bird>(birds.data(), birds.size()));

    const auto restored = simulation.birds();
    require(liveBirdCount(restored) == birds.size(),
            "restore did not preserve the multi-bird fixture in its live slots");
    std::array<int, MaxBirds> result{};
    for (std::size_t i = 0; i < restored.size(); ++i) {
        if (restored[i].phase == Phase::Dead) continue;
        const int found = simulation.neighbors(i, std::span<int>(result.data(), result.size()));
        require(found >= 0 && static_cast<std::size_t>(found) <= result.size(),
                "neighbors returned an invalid result count");
        std::vector<int> actual(result.begin(), result.begin() + found);
        std::vector<int> expected;
        if (validNeighborPhase(restored[i].phase)) {
            for (std::size_t j = 0; j < restored.size(); ++j) {
                if (j == i || !validNeighborPhase(restored[j].phase)) continue;
                const double dx = static_cast<double>(restored[j].position.x) - restored[i].position.x;
                const double dy = static_cast<double>(restored[j].position.y) - restored[i].position.y;
                if (std::hypot(dx, dy) <= PerceptionRadius)
                    expected.push_back(static_cast<int>(j));
            }
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        if (actual != expected) {
            std::ostringstream out;
            out << "neighbors differs from independent O(N^2) radius oracle for slot " << i;
            fail(out.str());
        }
    }
    std::array<int, MaxBirds> firstNeighbors{};
    const std::size_t firstSlot = slotForBird(restored, 1);
    const int firstCount = simulation.neighbors(firstSlot, firstNeighbors);
    require(firstCount == 2, "R-boundary fixture should have exactly two valid neighbors");
    std::vector<int> actualFirst(firstNeighbors.begin(), firstNeighbors.begin() + firstCount);
    std::vector<int> expectedFirst{
        static_cast<int>(slotForBird(restored, 2)),
        static_cast<int>(slotForBird(restored, 4))
    };
    std::sort(actualFirst.begin(), actualFirst.end());
    std::sort(expectedFirst.begin(), expectedFirst.end());
    require(actualFirst == expectedFirst,
            "R-boundary fixture did not include exactly the two points at radius R");

    simulation.restore(std::span<const Bird>{});
    requireLiveCount(simulation.birds(), 0, "restore of an empty population was not empty");
    settings.count = 1;
    simulation.configure(settings, bounds);
    const Bird only = makeBird(20, Phase::Active, {-25.f, -30.f}, {1.f, 0.f});
    simulation.restore(std::span<const Bird>(&only, 1));
    requireLiveCount(simulation.birds(), 1, "restore did not preserve the one-bird population");
    std::array<int, MaxBirds> noNeighbors{};
    const std::size_t onlySlot = slotForBird(simulation.birds(), 20);
    require(simulation.neighbors(onlySlot, noNeighbors) == 0,
            "a one-bird population must not report itself as its neighbor");
}

std::vector<TrailPoint> validTrail(const std::array<TrailPoint, HistoryCapacity>& history,
                                   int head, int count) {
    std::vector<TrailPoint> result;
    if (count <= 0) return result;
    const int capacity = static_cast<int>(history.size());
    const int first = (head - count + 1 + capacity * 2) % capacity;
    result.reserve(static_cast<std::size_t>(count));
    for (int offset = 0; offset < count; ++offset)
        result.push_back(history[static_cast<std::size_t>((first + offset) % capacity)]);
    return result;
}

bool trailHasPoint(const std::array<TrailPoint, HistoryCapacity>& history,
                   int head, int count, Vec2 point) {
    for (const TrailPoint& sample : validTrail(history, head, count))
        if (sameVec(sample.position, point, 1e-4)) return true;
    return false;
}

bool anyPathContainsPoint(std::span<const Bird> birds, std::span<const Ghost> ghosts,
                          std::uint32_t birdId, Vec2 point) {
    if (const Bird* bird = findBird(birds, birdId))
        if (trailHasPoint(bird->history, bird->historyHead, bird->historyCount, point))
            return true;
    for (const Ghost& ghost : ghosts)
        if (ghost.historyCount > 1 &&
            trailHasPoint(ghost.history, ghost.historyHead, ghost.historyCount, point))
            return true;
    return false;
}

bool noTrailMotionAfter(std::span<const Bird> birds, std::span<const Ghost> ghosts,
                        std::uint32_t birdId, float contactTime, Vec2 contact) {
    const auto stationaryAfterContact = [contactTime, contact](
        const std::array<TrailPoint, HistoryCapacity>& history, int head, int count) {
        for (const TrailPoint& sample : validTrail(history, head, count))
            if (sample.time > contactTime + 1e-5f &&
                !sameVec(sample.position, contact, 1e-4)) return false;
        return true;
    };
    if (const Bird* bird = findBird(birds, birdId))
        if (!stationaryAfterContact(bird->history, bird->historyHead, bird->historyCount))
            return false;
    for (const Ghost& ghost : ghosts)
        if (ghost.historyCount > 1 &&
            !stationaryAfterContact(ghost.history, ghost.historyHead, ghost.historyCount))
            return false;
    return true;
}

void verifyBoundaryDeathPauseAndReplenish() {
    const Bounds bounds{0.f, 0.f, 100.f, 100.f};
    Settings settings = quietSettings(1);
    settings.speed = 120.f;
    constexpr float FixedDt = 1.f / 60.f;
    Simulation simulation(0xB0A7u);
    simulation.configure(settings, bounds);

    Bird bird = makeBird(77, Phase::Active, {99.f, 50.f}, {1.f, 0.f});
    bird.historyHead = 2;
    bird.historyCount = 3;
    bird.lastSample = -.01f;
    bird.history[0] = TrailPoint{{11.f, 13.f}, -.05f, Color{.1f, .2f, .3f}};
    bird.history[1] = TrailPoint{{12.f, 14.f}, -.03f, Color{.2f, .3f, .4f}};
    bird.history[2] = TrailPoint{{13.f, 17.f}, -.01f, Color{.3f, .4f, .5f}};
    simulation.restore(std::span<const Bird>(&bird, 1));

    simulation.step(FixedDt);
    requireLiveCount(simulation.birds(), 1,
                     "crossing the boundary removed the bird before its death animation");
    const Bird firstDeath = requireBird(simulation.birds(), bird.id);
    require(firstDeath.phase == Phase::Dying, "active bird did not enter Dying after crossing the boundary");
    near(firstDeath.position.x, 100.0, 1e-4, "boundary contact x");
    near(firstDeath.position.y, 50.0, 1e-4, "boundary contact y");
    const float contactTime = FixedDt * .5f;
    require(anyPathContainsPoint(simulation.birds(), simulation.ghosts(), bird.id,
                                 {100.f, 50.f}),
            "boundary contact was not retained across the bird/ghost trail views");
    require(noTrailMotionAfter(simulation.birds(), simulation.ghosts(), bird.id,
                               contactTime, {100.f, 50.f}),
            "the trail recorded movement after the boundary contact");

    const Bird beforePause = requireBird(simulation.birds(), bird.id);
    const auto ghostsBeforePause = simulation.ghosts();
    std::vector<Ghost> ghostCopy(ghostsBeforePause.begin(), ghostsBeforePause.end());
    const SimStats statsBeforePause = simulation.stats();
    const float timeBeforePause = simulation.time();
    simulation.step(FixedDt, Input{}, true);
    near(simulation.time(), timeBeforePause, 0.0, "paused simulation time");
    requireLiveCount(simulation.birds(), 1, "pause changed the live dying population");
    require(sameBirdState(requireBird(simulation.birds(), bird.id), beforePause),
            "pause changed a dying bird's position, phase, or trail history");
    require(simulation.ghosts().size() == ghostCopy.size(), "pause changed ghost slot population");
    for (std::size_t i = 0; i < ghostCopy.size(); ++i)
        require(sameGhost(simulation.ghosts()[i], ghostCopy[i]), "pause changed ghost trail data");
    require(sameStats(simulation.stats(), statsBeforePause), "pause changed simulation statistics");

    settings.count = 0;
    simulation.configure(settings, bounds);
    for (int step = 0; step < 16; ++step) simulation.step(FixedDt);
    requireLiveCount(simulation.birds(), 1, "death expired before 0.28 seconds from contact");
    require(requireBird(simulation.birds(), bird.id).phase == Phase::Dying,
            "boundary death expired before 0.28 seconds from its contact time");
    near(requireBird(simulation.birds(), bird.id).position.x, 100.0, 1e-4,
         "frozen dying position");
    near(simulation.time() - contactTime, .275, .001,
         "elapsed time at fixed-step death boundary");
    require(anyPathContainsPoint(simulation.birds(), simulation.ghosts(), bird.id,
                                 {13.f, 17.f}) &&
            anyPathContainsPoint(simulation.birds(), simulation.ghosts(), bird.id,
                                 {100.f, 50.f}),
            "the pre-expiry trail lost its old path or boundary contact");
    require(noTrailMotionAfter(simulation.birds(), simulation.ghosts(), bird.id,
                               contactTime, {100.f, 50.f}),
            "the death trail recorded movement after boundary contact");

    simulation.step(.004f);
    require(requireBird(simulation.birds(), bird.id).phase == Phase::Dying,
            "boundary death expired before 0.28 seconds from its contact time");
    near(simulation.time() - contactTime, .279, .001,
         "elapsed time at the pre-expiry death boundary");
    require(noTrailMotionAfter(simulation.birds(), simulation.ghosts(), bird.id,
                               contactTime, {100.f, 50.f}),
            "the death trail recorded movement after boundary contact");
    simulation.step(.002f);
    requireLiveCount(simulation.birds(), 0, "target zero replenished a bird after the death animation");
    require(anyPathContainsPoint(simulation.birds(), simulation.ghosts(), bird.id,
                                 {13.f, 17.f}) &&
            anyPathContainsPoint(simulation.birds(), simulation.ghosts(), bird.id,
                                 {100.f, 50.f}),
            "retirement lost the old trail or contact sample");

    settings.count = 1;
    simulation.configure(settings, bounds);
    for (int attempt = 0; attempt < 8 && liveBirdCount(simulation.birds()) == 0; ++attempt)
        simulation.step(FixedDt);
    requireLiveCount(simulation.birds(), 1, "restoring the target did not produce a replacement bird");
    const Bird& replacement = requireFirstLiveBird(simulation.birds());
    require(replacement.id != bird.id,
            "restoring the target revived the retired bird instead of creating a new one");
    require(replacement.phase == Phase::Entering,
            "the replacement was not born as a new entering bird");
    require(!trailHasPoint(replacement.history, replacement.historyHead,
                           replacement.historyCount, {13.f, 17.f}),
            "a new bird inherited the retired bird's trail history");
}

void verifyEnteringBirdCrossesBoundaryInward() {
    const Bounds bounds{0.f, 0.f, 100.f, 100.f};
    Settings settings = quietSettings(1, 240.f);
    Simulation simulation(0x1A2E7u);
    simulation.configure(settings, bounds);
    const Bird entering = makeBird(78, Phase::Entering, {-1.f, 50.f}, {240.f, 0.f});
    simulation.restore(std::span<const Bird>(&entering, 1));

    simulation.step(1.f / 60.f);
    requireLiveCount(simulation.birds(), 1,
                     "an entering bird moving inward was removed at the boundary");
    const Bird& moved = requireBird(simulation.birds(), entering.id);
    require(moved.phase != Phase::Dying,
            "an entering bird moving inward was incorrectly marked Dying");
    near(moved.position.x, 3.0, 1e-3, "inward boundary crossing x");
    near(moved.position.y, 50.0, 1e-3, "inward boundary crossing y");
}

void verifyTaskbarExclusionBehavior() {
    const Bounds bounds{0.f, 0.f, 200.f, 100.f};
    Settings settings = quietSettings(1, 60.f);
    settings.avoidTaskbar = true;
    const std::array<Bounds, 1> taskbarExclusion{Bounds{0.f, 80.f, 100.f, 100.f}};
    constexpr float Frame = 1.f / 60.f;

    const auto stepOnce = [&](const Bird& input, std::span<const Bounds> exclusions,
                              float dt) -> Bird {
        Simulation simulation(0x7A5B4u + input.id);
        simulation.configure(settings, bounds, exclusions);
        simulation.restore(std::span<const Bird>(&input, 1));
        simulation.step(dt);
        return requireBird(simulation.birds(), input.id);
    };

    const Bird approachingTaskbar = makeBird(1, Phase::Active, {50.f, 79.f}, {0.f, 60.f});
    const Bird taskbarContact = stepOnce(approachingTaskbar, taskbarExclusion, Frame);
    require(taskbarContact.phase == Phase::Dying,
            "active bird did not enter Dying when it contacted the taskbar exclusion");
    near(taskbarContact.position.x, 50.f, 1e-4, "taskbar contact x");
    near(taskbarContact.position.y, 80.f, 1e-4, "taskbar contact y");

    const Bird onSecondaryMonitor = makeBird(2, Phase::Active, {150.f, 79.f}, {0.f, 60.f});
    const Bird secondaryAfter = stepOnce(onSecondaryMonitor, taskbarExclusion, 2.f * Frame);
    require(secondaryAfter.phase == Phase::Active,
            "taskbar exclusion on the primary monitor killed a bird on the secondary monitor");
    near(secondaryAfter.position.x, 150.f, 1e-4, "secondary monitor x");
    near(secondaryAfter.position.y, 81.f, 1e-4, "secondary monitor y");

    const Bird atMonitorSeam = makeBird(3, Phase::Active, {99.f, 50.f}, {60.f, 0.f});
    const Bird seamAfter = stepOnce(atMonitorSeam, taskbarExclusion, 2.f * Frame);
    require(seamAfter.phase == Phase::Active,
            "taskbar exclusion blocked crossing the seam between monitors");
    near(seamAfter.position.x, 101.f, 1e-4, "monitor seam crossing x");

    const std::array<Bounds, 1> narrowExclusion{Bounds{90.f, 10.f, 91.f, 90.f}};
    const Bird crossingNarrowExclusion = makeBird(4, Phase::Active, {89.f, 50.f}, {60.f, 0.f});
    const Bird narrowContact = stepOnce(crossingNarrowExclusion, narrowExclusion, .1f);
    require(narrowContact.phase == Phase::Dying,
            "a large step tunneled through a narrow taskbar exclusion");
    near(narrowContact.position.x, 90.f, 1e-4, "narrow exclusion contact x");
    near(narrowContact.position.y, 50.f, 1e-4, "narrow exclusion contact y");

    const Bird entering = makeBird(5, Phase::Entering, {150.f, 101.f}, {0.f, -60.f});
    const Bird entered = stepOnce(entering, taskbarExclusion, .1f);
    require(entered.phase == Phase::Entering || entered.phase == Phase::Active,
            "an entering bird was killed while moving into the safe monitor area");
    require(entered.position.x > 100.f && entered.position.x < bounds.right &&
            entered.position.y < bounds.bottom && entered.position.y > 80.f,
            "an entering bird failed to move into the secondary monitor's safe area");

    Simulation newlyExcluded(0x7A5B9u);
    newlyExcluded.configure(settings, bounds, std::span<const Bounds>{});
    const Bird inNewExclusion = makeBird(6, Phase::Active, {50.f, 50.f}, {60.f, 0.f});
    newlyExcluded.restore(std::span<const Bird>(&inNewExclusion, 1));
    const std::array<Bounds, 1> addedExclusion{Bounds{40.f, 40.f, 60.f, 60.f}};
    newlyExcluded.configure(settings, bounds, addedExclusion);
    const Bird& afterReconfigure = requireBird(newlyExcluded.birds(), inNewExclusion.id);
    require(sameVec(afterReconfigure.position, inNewExclusion.position),
            "adding a taskbar exclusion teleported a bird already at that location");
}

void verifyLatestPopulationTarget() {
    const Bounds bounds{0.f, 0.f, 100000.f, 100000.f};
    Settings settings = quietSettings(12, 120.f);
    Simulation simulation(0xC0A17u);
    simulation.configure(settings, bounds);
    simulation.restore(std::span<const Bird>{});

    const std::array<int, 7> rapidTargets{24, 2, 18, 1, 12, 0, 5};
    for (int target : rapidTargets) {
        settings.count = target;
        simulation.configure(settings, bounds);
        simulation.step(.02f);
        simulation.step(.02f);
    }

    constexpr int finalTarget = 5;
    int stableSteps = 0;
    constexpr int requiredStableSteps = 60;
    for (int step = 0; step < 6000 && stableSteps < requiredStableSteps; ++step) {
        simulation.step(1.f / 60.f);
        int entering = 0;
        int active = 0;
        int leaving = 0;
        int dying = 0;
        for (const Bird& current : simulation.birds()) {
            switch (current.phase) {
            case Phase::Entering: ++entering; break;
            case Phase::Active: ++active; break;
            case Phase::Leaving: ++leaving; break;
            case Phase::Dying: ++dying; break;
            case Phase::Dead: break;
            }
        }
        if (active + entering == finalTarget && leaving == 0 && dying == 0 &&
            liveBirdCount(simulation.birds()) == static_cast<std::size_t>(finalTarget))
            ++stableSteps;
        else
            stableSteps = 0;
    }
    require(stableSteps == requiredStableSteps,
            "population did not converge and remain at the latest target after rapid count changes");
}

std::vector<Bird> forceFixture(std::string_view force) {
    std::vector<Bird> birds;
    birds.push_back(makeBird(1, Phase::Active, {10000.f, 10000.f}, {1.f, 0.f}));
    if (force == "cohesion")
        birds.push_back(makeBird(2, Phase::Active, {10000.f, 10040.f}, {1.f, 0.f}));
    else if (force == "alignment")
        birds.push_back(makeBird(2, Phase::Active, {10000.f, 10040.f}, {0.f, 1.f}));
    else
        birds.push_back(makeBird(2, Phase::Active, {10010.f, 10010.f}, {1.f, 0.f}));
    return birds;
}

float firstBirdHeadingAfterStep(std::string_view force, bool enabled) {
    Settings settings = quietSettings(2, 80.f);
    if (enabled) {
        if (force == "cohesion") settings.cohesion = 1.f;
        else if (force == "alignment") settings.alignment = 1.f;
        else settings.separation = 1.f;
    }
    Simulation simulation(0xF0CEu);
    simulation.configure(settings, Bounds{-100000.f, -100000.f, 100000.f, 100000.f});
    const std::vector<Bird> fixture = forceFixture(force);
    simulation.restore(std::span<const Bird>(fixture.data(), fixture.size()));
    simulation.step(.05f);
    const Vec2 velocity = requireBird(simulation.birds(), 1).velocity;
    return std::atan2(velocity.y, velocity.x);
}

void verifySpeedAndIndependentWeights() {
    const Bounds wide{-1000000.f, -1000000.f, 1000000.f, 1000000.f};
    Settings settings = quietSettings(1, 120.f);
    Simulation simulation(0x5EEDu);
    simulation.configure(settings, wide);
    const Bird bird = makeBird(1, Phase::Active, {0.f, 0.f}, {1.f, 0.f});
    simulation.restore(std::span<const Bird>(&bird, 1));
    simulation.step(.05f);
    const Bird& moved = requireBird(simulation.birds(), 1);
    near(std::hypot(static_cast<double>(moved.velocity.x), moved.velocity.y), 120.0, 1e-3,
         "speedError=0 velocity magnitude");
    near(std::hypot(static_cast<double>(moved.position.x), moved.position.y), 6.0, 1e-3,
         "speedError=0 displacement magnitude");

    settings.speed = 100.f;
    settings.speedError = .2f;
    simulation.configure(settings, wide);
    Bird biased = makeBird(2, Phase::Active, {0.f, 0.f}, {1.f, 0.f}, .5f);
    simulation.restore(std::span<const Bird>(&biased, 1));
    for (int step = 0; step < 4; ++step) {
        const Vec2 before = requireBird(simulation.birds(), 2).position;
        simulation.step(.05f);
        const Bird& current = requireBird(simulation.birds(), 2);
        near(std::hypot(static_cast<double>(current.velocity.x), current.velocity.y), 110.0,
             1e-3, "stable speedError proportion");
        near(std::hypot(static_cast<double>(current.position.x - before.x),
                        current.position.y - before.y), 5.5, 1e-3,
             "stable speedError displacement");
        near(current.speedBias, .5, 1e-6, "stable per-bird speed bias");
    }

    for (const std::string_view force : {"cohesion", "alignment", "separation"}) {
        const float disabledHeading = firstBirdHeadingAfterStep(force, false);
        const float enabledHeading = firstBirdHeadingAfterStep(force, true);
        near(disabledHeading, 0.0, 1e-4, std::string(force) + " zero weight heading");
        require(std::abs(enabledHeading - disabledHeading) > .01f,
                std::string(force) + " weight had no independent effect in its isolated fixture");
    }
}

void verifyClickBoostRefreshAndRestore() {
    Settings settings = quietSettings(1, 100.f);
    Simulation simulation(0xC11C4u);
    simulation.configure(settings, Bounds{-10000.f, -10000.f, 10000.f, 10000.f});
    const Bird bird = makeBird(5, Phase::Active, {0.f, 0.f}, {1.f, 0.f});
    simulation.restore(std::span<const Bird>(&bird, 1));

    Input click{};
    click.click = true;
    click.pointer = {0.f, 0.f};
    simulation.step(.05f, click);
    for (int i = 0; i < 30; ++i) simulation.step(.05f);
    double speed = std::hypot(static_cast<double>(requireBird(simulation.birds(), 5).velocity.x),
                              requireBird(simulation.birds(), 5).velocity.y);
    near(speed, 110.0, .05, "click boost approaches ten percent");

    const Vec2 positionAtRefresh = requireBird(simulation.birds(), 5).position;
    click.pointer = positionAtRefresh;
    simulation.step(.05f, click);
    speed = std::hypot(static_cast<double>(requireBird(simulation.birds(), 5).velocity.x),
                       requireBird(simulation.birds(), 5).velocity.y);
    require(speed <= 110.2, "repeated click stacked the ten-percent speed boost");

    for (int i = 0; i < 13; ++i) simulation.step(.05f);
    speed = std::hypot(static_cast<double>(requireBird(simulation.birds(), 5).velocity.x),
                       requireBird(simulation.birds(), 5).velocity.y);
    require(speed > 109.5,
            "a refreshed click boost expired at the original click's timeout");

    for (int i = 0; i < 50; ++i) simulation.step(.05f);
    speed = std::hypot(static_cast<double>(requireBird(simulation.birds(), 5).velocity.x),
                       requireBird(simulation.birds(), 5).velocity.y);
    near(speed, 100.0, .05, "click boost restoration to normal speed");
}

void verifyHistoryAndPausedVisualSettings() {
    const Bounds wide{-1000000.f, -1000000.f, 1000000.f, 1000000.f};
    Settings settings = quietSettings(1, 80.f);
    settings.trails = false;
    Simulation historySimulation(0x7157u);
    historySimulation.configure(settings, wide);
    const Bird bird = makeBird(8, Phase::Active, {0.f, 0.f}, {1.f, 0.f});
    historySimulation.restore(std::span<const Bird>(&bird, 1));
    for (int i = 0; i < 8; ++i) historySimulation.step(1.f / 60.f);
    require(requireBird(historySimulation.birds(), 8).historyCount > 1,
            "disabling trail display also disabled trail-history recording");

    Settings baseline = quietSettings(1, 80.f);
    baseline.trailDuration = .28f;
    baseline.color = Color{.2f, .4f, .6f};
    Simulation control(0x7199u);
    Simulation changed(0x7199u);
    control.configure(baseline, wide);
    changed.configure(baseline, wide);
    Bird start = makeBird(12, Phase::Active, {100.f, 200.f}, {1.f, .2f});
    start.historyHead = 1;
    start.historyCount = 2;
    start.history[0] = TrailPoint{{98.f, 199.6f}, -.02f, baseline.color};
    start.history[1] = TrailPoint{{99.f, 199.8f}, -.01f, baseline.color};
    start.lastSample = -.01f;
    control.restore(std::span<const Bird>(&start, 1));
    changed.restore(std::span<const Bird>(&start, 1));
    for (int i = 0; i < 2; ++i) {
        control.step(1.f / 60.f);
        changed.step(1.f / 60.f);
    }
    const Bird pausedBefore = requireBird(changed.birds(), 12);
    const float pausedTime = changed.time();
    changed.step(.75f, Input{}, true);
    near(changed.time(), pausedTime, 0.0, "paused visual settings time");
    require(sameBirdState(requireBird(changed.birds(), 12), pausedBefore),
            "paused step changed recorded trail data");

    Settings visualChange = baseline;
    visualChange.trailDuration = .1f;
    visualChange.color = Color{.95f, .1f, .25f};
    changed.configure(visualChange, wide);
    require(requireBird(changed.birds(), 12).historyCount == pausedBefore.historyCount &&
            requireBird(changed.birds(), 12).historyHead == pausedBefore.historyHead,
            "changing trail duration or color while paused rewrote the recorded trail");
    for (int i = 0; i < 30; ++i) {
        control.step(1.f / 60.f);
        changed.step(1.f / 60.f);
        require(sameVec(requireBird(control.birds(), 12).position,
                        requireBird(changed.birds(), 12).position, 1e-4),
                "paused trail duration/color adjustment changed the resumed trajectory");
        require(sameVec(requireBird(control.birds(), 12).velocity,
                        requireBird(changed.birds(), 12).velocity, 1e-4),
                "paused trail duration/color adjustment changed resumed velocity");
    }
    const Bird& controlBird = requireBird(control.birds(), 12);
    const Bird& changedBird = requireBird(changed.birds(), 12);
    const auto controlTrail = validTrail(controlBird.history,
                                         controlBird.historyHead,
                                         controlBird.historyCount);
    const auto changedTrail = validTrail(changedBird.history,
                                         changedBird.historyHead,
                                         changedBird.historyCount);
    require(controlTrail.size() == changedTrail.size(),
            "changing trail duration changed the retained history sample count");
    for (std::size_t i = 0; i < controlTrail.size(); ++i) {
        require(sameVec(controlTrail[i].position, changedTrail[i].position, 1e-4) &&
                std::abs(controlTrail[i].time - changedTrail[i].time) < 1e-5,
                "changing trail duration/color altered a recorded path point");
    }
}

void requireEquivalentTrailAges(
    const std::array<TrailPoint, HistoryCapacity>& firstHistory, int firstHead, int firstCount,
    float firstTime, const std::array<TrailPoint, HistoryCapacity>& secondHistory,
    int secondHead, int secondCount, float secondTime, std::string_view label) {
    const auto first = validTrail(firstHistory, firstHead, firstCount);
    const auto second = validTrail(secondHistory, secondHead, secondCount);
    require(first.size() == second.size(), label);
    for (std::size_t i = 0; i < first.size(); ++i) {
        require(sameVec(first[i].position, second[i].position, 1e-4) &&
                sameColor(first[i].color, second[i].color, 1e-4),
                "epoch rebase changed trail order or sampled path");
        near(static_cast<double>(firstTime) - first[i].time,
             static_cast<double>(secondTime) - second[i].time, .002, label);
    }
}

const Ghost* firstVisibleGhost(std::span<const Ghost> ghosts) {
    const auto found = std::find_if(ghosts.begin(), ghosts.end(),
        [](const Ghost& ghost) { return ghost.historyCount > 1; });
    return found == ghosts.end() ? nullptr : &*found;
}

void requireEquivalentGhostAges(const Simulation& first, const Simulation& second) {
    const std::size_t firstCount = visibleGhostCount(first.ghosts());
    const std::size_t secondCount = visibleGhostCount(second.ghosts());
    require(firstCount == secondCount, "epoch rebase changed when the death ghost is visible");
    if (firstCount == 0) return;
    require(firstCount == 1, "rebase fixture should contain exactly one visible death ghost");
    const Ghost* firstGhost = firstVisibleGhost(first.ghosts());
    const Ghost* secondGhost = firstVisibleGhost(second.ghosts());
    require(firstGhost && secondGhost, "visible ghost lookup failed");
    requireEquivalentTrailAges(firstGhost->history, firstGhost->historyHead,
                               firstGhost->historyCount, first.time(),
                               secondGhost->history, secondGhost->historyHead,
                               secondGhost->historyCount, second.time(),
                               "epoch rebase changed death-ghost sample ages");
    near(static_cast<double>(first.time()) - firstGhost->retiredAt,
         static_cast<double>(second.time()) - secondGhost->retiredAt, .002,
         "epoch rebase changed the death-ghost retirement age");
}

Bird makeEpochBird(std::uint32_t id, Phase phase, Vec2 position, Vec2 velocity,
                   float epochTime) {
    constexpr float FixedDt = 1.f / 60.f;
    Bird bird = makeBird(id, phase, position, velocity);
    bird.historyHead = 2;
    bird.historyCount = 3;
    bird.history[0] = TrailPoint{{position.x - 3.f, position.y},
                                 epochTime - 3.f * FixedDt, bird.color};
    bird.history[1] = TrailPoint{{position.x - 2.f, position.y},
                                 epochTime - 2.f * FixedDt, bird.color};
    bird.history[2] = TrailPoint{{position.x - 1.f, position.y},
                                 epochTime - FixedDt, bird.color};
    bird.lastSample = epochTime - FixedDt;
    return bird;
}

void verifyEpochRebaseContinuity() {
    constexpr float FixedDt = 1.f / 60.f;
    constexpr std::size_t ProbeStepLimit = 100000;
    constexpr std::size_t RegressionStepLimit = 240;
    constexpr float RebaseLead = .35f;
    constexpr float BoostRemaining = .73f;
    const Bounds bounds{0.f, 0.f, 10000.f, 10000.f};

    // Discover a public clock rollover instead of encoding the implementation's epoch threshold.
    Simulation probe(0xE90C4u);
    Settings probeSettings = quietSettings(0);
    probe.configure(probeSettings, bounds);
    probe.restore(std::span<const Bird>{}, 0.f);
    float priorProbeTime = probe.time();
    double observedRolloverTime = 0.0;
    bool rolloverObserved = false;
    for (std::size_t step = 0; step < ProbeStepLimit; ++step) {
        probe.step(FixedDt);
        const float currentProbeTime = probe.time();
        if (currentProbeTime < priorProbeTime - FixedDt * .5f) {
            observedRolloverTime = static_cast<double>(priorProbeTime) + FixedDt;
            rolloverObserved = true;
            break;
        }
        priorProbeTime = currentProbeTime;
    }
    require(rolloverObserved, "public simulation clock did not expose an epoch rebase");

    const float highStartTime = static_cast<float>(observedRolloverTime - RebaseLead);
    require(highStartTime > 0.f, "observed rollover is too close to construct the rebase fixture");
    Settings settings = quietSettings(1, 100.f);
    Simulation rebased(0xE90C4u);
    Simulation reference(0xE90C4u);
    rebased.configure(settings, bounds);
    reference.configure(settings, bounds);

    Bird highBooster = makeEpochBird(901, Phase::Active, {1000.f, 1000.f},
                                     {1.f, 0.f}, highStartTime);
    Bird lowBooster = makeEpochBird(901, Phase::Active, {1000.f, 1000.f},
                                    {1.f, 0.f}, 0.f);
    highBooster.boostUntil = highStartTime + BoostRemaining;
    lowBooster.boostUntil = BoostRemaining;
    Bird highDying = makeEpochBird(902, Phase::Dying, {5000.f, 5000.f},
                                   {0.f, 1.f}, highStartTime);
    Bird lowDying = makeEpochBird(902, Phase::Dying, {5000.f, 5000.f},
                                  {0.f, 1.f}, 0.f);
    highDying.phaseTime = .1f;
    lowDying.phaseTime = .1f;
    const std::array<Bird, 2> highFixture{{highBooster, highDying}};
    const std::array<Bird, 2> lowFixture{{lowBooster, lowDying}};
    rebased.restore(std::span<const Bird>(highFixture.data(), highFixture.size()), highStartTime);
    reference.restore(std::span<const Bird>(lowFixture.data(), lowFixture.size()), 0.f);

    bool sawGhostBeforeRollover = false;
    bool sawGhostAtRollover = false;
    bool sawRollover = false;
    bool sawGhostDecay = false;
    bool sawBoostRestoration = false;
    bool ghostAgeInitialized = false;
    double previousGhostAge = 0.0;
    float elapsed = 0.f;
    for (std::size_t step = 0; step < RegressionStepLimit; ++step) {
        const float previousHighTime = rebased.time();
        const float previousLowTime = reference.time();
        rebased.step(FixedDt);
        reference.step(FixedDt);
        elapsed += FixedDt;
        const bool rollover = rebased.time() < previousHighTime - FixedDt * .5f;
        if (rollover) {
            require(!sawRollover, "unexpected repeated clock rollover in the short regression interval");
            sawRollover = true;
        }
        require(reference.time() > previousLowTime,
                "low-time reference unexpectedly changed its clock epoch");

        requireLiveCount(rebased.birds(), liveBirdCount(reference.birds()),
                         "epoch rebase changed the live bird population");
        const Bird& highActive = requireBird(rebased.birds(), 901);
        const Bird& lowActive = requireBird(reference.birds(), 901);
        require(highActive.phase == lowActive.phase &&
                sameVec(highActive.position, lowActive.position, 1e-3) &&
                sameVec(highActive.velocity, lowActive.velocity, 1e-3),
                "epoch rebase interrupted or displaced active-bird motion");
        const double highSpeed = std::hypot(static_cast<double>(highActive.velocity.x),
                                            highActive.velocity.y);
        const double lowSpeed = std::hypot(static_cast<double>(lowActive.velocity.x),
                                           lowActive.velocity.y);
        near(highSpeed, lowSpeed, .01, "epoch rebase changed remaining click-boost behavior");
        requireEquivalentTrailAges(highActive.history, highActive.historyHead,
                                   highActive.historyCount, rebased.time(),
                                   lowActive.history, lowActive.historyHead,
                                   lowActive.historyCount, reference.time(),
                                   "epoch rebase changed active trail ages");

        const std::size_t highGhostCount = visibleGhostCount(rebased.ghosts());
        const std::size_t lowGhostCount = visibleGhostCount(reference.ghosts());
        require(highGhostCount == lowGhostCount,
                "epoch rebase changed death-ghost decay relative to simulation age");
        if (highGhostCount != 0) {
            sawGhostBeforeRollover = sawGhostBeforeRollover || !sawRollover;
            const Ghost* highGhost = firstVisibleGhost(rebased.ghosts());
            require(highGhost != nullptr, "visible death ghost was not available through its slot view");
            const double ghostAge = static_cast<double>(rebased.time()) -
                highGhost->history[static_cast<std::size_t>(highGhost->historyHead)].time;
            if (ghostAgeInitialized)
                near(ghostAge, previousGhostAge + FixedDt, .003,
                     "death-ghost age jumped across an epoch rebase");
            previousGhostAge = ghostAge;
            ghostAgeInitialized = true;
        } else if (sawRollover) {
            sawGhostDecay = true;
        }
        requireEquivalentGhostAges(rebased, reference);
        if (rollover) {
            require(sawGhostBeforeRollover && highGhostCount == 1,
                    "the death ghost did not remain alive across the observed epoch rebase");
            require(highSpeed > settings.speed + 5.0,
                    "the active boost expired during epoch rebase");
            sawGhostAtRollover = true;
        }
        if (elapsed > BoostRemaining + .5f) {
            near(highSpeed, settings.speed, .5, "boost did not decay after its relative lifetime");
            near(lowSpeed, settings.speed, .5, "reference boost did not decay after its relative lifetime");
            sawBoostRestoration = true;
        }
        if (sawRollover && sawGhostDecay && sawBoostRestoration) break;
    }
    require(sawRollover, "high-time fixture did not cross the observed epoch rebase");
    require(sawGhostAtRollover, "ghost age was not exercised during the epoch rebase");
    require(sawGhostDecay, "retired ghost did not disappear as its sampled path aged");
    require(sawBoostRestoration, "boost was not observed returning to baseline after rebase");
}


std::vector<Bird> colorCluster() {
    constexpr std::array<Color, 6> colors{{
        {1.f, .15f, .12f}, {.1f, .9f, .2f}, {.1f, .2f, 1.f},
        {1.f, .8f, .1f}, {.8f, .1f, .9f}, {.1f, .9f, .9f}
    }};
    std::vector<Bird> birds;
    birds.reserve(colors.size());
    for (std::size_t i = 0; i < colors.size(); ++i) {
        Bird bird = makeBird(static_cast<std::uint32_t>(i + 1), Phase::Active,
                             {900.f + 8.f * static_cast<float>(i), 500.f + 2.f * static_cast<float>(i)},
                             {1.f, 0.f}, -.5f + .2f * static_cast<float>(i));
        bird.baseColor = colors[i];
        bird.color = colors[i];
        birds.push_back(bird);
    }
    return birds;
}

double colorDistance(Color a, Color b) {
    const double dr = static_cast<double>(a.r) - b.r;
    const double dg = static_cast<double>(a.g) - b.g;
    const double db = static_cast<double>(a.b) - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

double maxChannelDelta(Color a, Color b) {
    return std::max({std::abs(static_cast<double>(a.r) - b.r),
                     std::abs(static_cast<double>(a.g) - b.g),
                     std::abs(static_cast<double>(a.b) - b.b)});
}

void verifyRandomColorIndependenceAndTransition() {
    const Bounds bounds{0.f, 0.f, 1800.f, 1000.f};
    Settings plainSettings = quietSettings(6, 70.f);
    plainSettings.speedError = .08f;
    plainSettings.separation = .8f;
    plainSettings.alignment = .5f;
    plainSettings.cohesion = .4f;
    Settings colorSettings = plainSettings;
    colorSettings.randomColor = true;
    Simulation plain(0xC010A5u);
    Simulation colored(0xC010A5u);
    plain.configure(plainSettings, bounds);
    colored.configure(colorSettings, bounds);
    const std::vector<Bird> fixture = colorCluster();
    plain.restore(std::span<const Bird>(fixture.data(), fixture.size()));
    colored.restore(std::span<const Bird>(fixture.data(), fixture.size()));

    std::array<Color, 6> birthColors{};
    for (std::size_t i = 0; i < birthColors.size(); ++i)
        birthColors[i] = requireBird(colored.birds(), fixture[i].id).baseColor;
    bool anyTransition = false;
    for (int step = 0; step < 240; ++step) {
        const std::array<Color, 6> previousColors = [&] {
            std::array<Color, 6> snapshot{};
            for (std::size_t i = 0; i < snapshot.size(); ++i)
                snapshot[i] = requireBird(colored.birds(), fixture[i].id).color;
            return snapshot;
        }();
        plain.step(1.f / 60.f);
        colored.step(1.f / 60.f);
        require(liveBirdCount(plain.birds()) == fixture.size() &&
                liveBirdCount(colored.birds()) == fixture.size(),
                "random-color mode changed population behavior");
        for (std::size_t i = 0; i < fixture.size(); ++i) {
            const std::uint32_t id = fixture[i].id;
            const Bird& noColor = requireBird(plain.birds(), id);
            const Bird& withColor = requireBird(colored.birds(), id);
            require(noColor.phase == withColor.phase,
                    "random-color mode changed bird lifecycle");
            require(sameVec(noColor.position, withColor.position, 1e-4) &&
                    sameVec(noColor.velocity, withColor.velocity, 1e-4),
                    "random-color updates changed motion trajectory");
            require(sameColor(withColor.baseColor, birthColors[i]),
                    "a bird's base/birth color changed during its lifetime");
            const double delta = maxChannelDelta(previousColors[i], withColor.color);
            require(delta <= .2,
                    "random-color transition changed a bird's visible color abruptly in one step");
            anyTransition = anyTransition || colorDistance(previousColors[i], withColor.color) > 1e-5;
        }
    }
    require(anyTransition,
            "random-color mode did not gradually transition any member of the aligned flock");

    Settings birthSettings = quietSettings(1, 40.f);
    birthSettings.randomColor = true;
    Simulation births(0xB17D5u);
    births.configure(birthSettings, Bounds{0.f, 0.f, 200000.f, 200000.f});
    births.restore(std::span<const Bird>{});
    for (int step = 0; step < 5 && liveBirdCount(births.birds()) == 0; ++step)
        births.step(1.f / 60.f);
    requireLiveCount(births.birds(), 1, "random-color mode failed to create its requested bird");
    const std::uint32_t bornId = requireFirstLiveBird(births.birds()).id;
    const Color bornBase = requireBird(births.birds(), bornId).baseColor;
    for (int step = 0; step < 120; ++step) {
        births.step(1.f / 60.f);
        requireLiveCount(births.birds(), 1,
                         "newborn color fixture unexpectedly changed population");
        require(sameColor(requireBird(births.birds(), bornId).baseColor, bornBase),
                "a spawned bird's birth color was not stable");
    }
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

void requireIdenticalSimulations(const Simulation& expected, const Simulation& actual,
                                 std::string_view phaseLabel, int step) {
    if (expected.time() != actual.time()) {
        std::ostringstream out;
        out << phaseLabel << " step " << step << ": simulation time mismatch ("
            << expected.time() << " vs " << actual.time() << ")";
        fail(out.str());
    }
    const SimStats expStats = expected.stats();
    const SimStats actStats = actual.stats();
    if (expStats.candidates != actStats.candidates) {
        std::ostringstream out;
        out << phaseLabel << " step " << step << ": candidate count mismatch ("
            << expStats.candidates << " vs " << actStats.candidates << ")";
        fail(out.str());
    }
    if (expStats.active != actStats.active || expStats.entering != actStats.entering ||
        expStats.leaving != actStats.leaving || expStats.dying != actStats.dying) {
        std::ostringstream out;
        out << phaseLabel << " step " << step << ": phase count mismatch (active="
            << expStats.active << "/" << actStats.active << ", entering="
            << expStats.entering << "/" << actStats.entering << ", leaving="
            << expStats.leaving << "/" << actStats.leaving << ", dying="
            << expStats.dying << "/" << actStats.dying << ")";
        fail(out.str());
    }
    const auto expBirds = expected.birds();
    const auto actBirds = actual.birds();
    if (expBirds.size() != actBirds.size()) {
        std::ostringstream out;
        out << phaseLabel << " step " << step << ": bird view size mismatch ("
            << expBirds.size() << " vs " << actBirds.size() << ")";
        fail(out.str());
    }
    for (std::size_t i = 0; i < expBirds.size(); ++i) {
        if (!sameBirdState(expBirds[i], actBirds[i])) {
            std::ostringstream out;
            out << phaseLabel << " step " << step << ": bird state mismatch at slot " << i
                << " (id " << expBirds[i].id << " vs " << actBirds[i].id << ")";
            fail(out.str());
        }
    }
    const auto expGhosts = expected.ghosts();
    const auto actGhosts = actual.ghosts();
    if (expGhosts.size() != actGhosts.size()) {
        std::ostringstream out;
        out << phaseLabel << " step " << step << ": ghost view size mismatch ("
            << expGhosts.size() << " vs " << actGhosts.size() << ")";
        fail(out.str());
    }
    for (std::size_t i = 0; i < expGhosts.size(); ++i) {
        if (!sameGhost(expGhosts[i], actGhosts[i])) {
            std::ostringstream out;
            out << phaseLabel << " step " << step << ": ghost state mismatch at slot " << i;
            fail(out.str());
        }
    }
}

std::vector<Bird> makeParallelFixture(int count) {
    std::vector<Bird> fixture;
    fixture.reserve(static_cast<std::size_t>(count));
    FixedRandom rng(0x1337BEEFu);
    constexpr double Tau = 6.283185307179586476925286766559;
    const int activeCount = std::max(1, count - 60);
    const int enteringThreshold = activeCount + (count - activeCount) / 3;
    const int leavingThreshold = activeCount + 2 * (count - activeCount) / 3;
    for (int i = 0; i < count; ++i) {
        Bird b{};
        b.id = static_cast<std::uint32_t>(i + 1);
        if (i < activeCount) {
            b.phase = Phase::Active;
            const float x = 200.f + rng.unit() * 200.f;
            const float y = 200.f + rng.unit() * 200.f;
            const double angle = static_cast<double>(rng.unit()) * Tau;
            b.position = {x, y};
            b.velocity = {static_cast<float>(std::cos(angle) * 85.f),
                          static_cast<float>(std::sin(angle) * 85.f)};
        } else if (i < enteringThreshold) {
            b.phase = Phase::Entering;
            b.position = {-210.f + rng.unit() * 5.f, -50.f + rng.unit() * 100.f};
            b.velocity = {90.f, 10.f};
        } else if (i < leavingThreshold) {
            b.phase = Phase::Leaving;
            b.position = {1200.f + rng.unit() * 100.f, 855.f + rng.unit() * 20.f};
            b.velocity = {10.f, 80.f};
        } else {
            b.phase = Phase::Dying;
            b.position = {50.f + rng.unit() * 50.f, 50.f + rng.unit() * 50.f};
            b.velocity = {40.f, -40.f};
            b.phaseTime = 0.1f;
        }
        b.speedBias = rng.unit() * 0.14f - 0.07f;
        b.baseColor = Color{0.5f + rng.unit() * 0.3f, 0.4f + rng.unit() * 0.4f, 0.8f};
        b.color = b.baseColor;
        fixture.push_back(b);
    }
    return fixture;
}

void verifyParallelDeterminismAndStateTransitions() {
    const int highCount = std::min(1250, MaxBirds);
    const std::vector<Bird> heavyFixture = makeParallelFixture(highCount);
    const Bounds bounds{-200.f, -150.f, 1600.f, 950.f};

    // Repeated construction and destruction with >=1200 fixture to verify thread pools join cleanly
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        Simulation tempSerial(0x55AA1000u + cycle, 1);
        Simulation tempParallel(0x55AA1000u + cycle, 4);
        Settings settings = quietSettings(highCount, 95.f);
        tempSerial.configure(settings, bounds);
        tempParallel.configure(settings, bounds);
        tempSerial.restore(std::span<const Bird>(heavyFixture.data(), heavyFixture.size()));
        tempParallel.restore(std::span<const Bird>(heavyFixture.data(), heavyFixture.size()));
        for (int s = 0; s < 4; ++s) {
            tempSerial.step(1.f / 60.f);
            tempParallel.step(1.f / 60.f);
            requireIdenticalSimulations(tempSerial, tempParallel, "lifecycle-churn", s);
        }
    }

    // Coexisting instances: serial (workers=1) vs workers=2 vs workers=4 vs workers=0 (auto)
    constexpr std::uint32_t Seed = 0xD37E57u;
    Simulation simSerial(Seed, 1);
    Simulation simWorkers2(Seed, 2);
    Simulation simWorkers4(Seed, 4);
    Simulation simWorkersAuto(Seed, 0);
    std::array<Simulation*, 4> sims{&simSerial, &simWorkers2, &simWorkers4, &simWorkersAuto};

    const std::array<Bounds, 2> exclusions{{
        {-200.f, 850.f, 700.f, 950.f},
        {800.f, 860.f, 1600.f, 950.f}
    }};

    Settings settings = quietSettings(highCount, 95.f);
    settings.separation = 1.38f;
    settings.alignment = 1.0f;
    settings.cohesion = 0.65f;
    settings.edge = 0.175f;
    settings.speedError = 0.07f;
    settings.trails = true;
    settings.trailDuration = 0.28f;
    settings.avoidTaskbar = true;

    for (auto* sim : sims) {
        sim->configure(settings, bounds, exclusions);
        sim->restore(std::span<const Bird>(heavyFixture.data(), heavyFixture.size()));
    }

    constexpr float Dt = 1.f / 60.f;
    auto stepAll = [&](const Input& input, bool paused, std::string_view label,
                       int step, bool requireParallelExecution = false) {
        if (step % 2 == 0) {
            for (std::size_t i = 0; i < sims.size(); ++i) sims[i]->step(Dt, input, paused);
        } else {
            for (std::size_t i = sims.size(); i-- > 0; ) sims[i]->step(Dt, input, paused);
        }

        if (requireParallelExecution && highCount >= 1000) {
            require(liveBirdCount(sims[0]->birds()) >= 1000,
                    std::string(label) + ": expected >= 1000 live birds for parallel execution");
            require(sims[0]->stats().candidates > 0,
                    std::string(label) + ": parallel heavy load must generate candidate pairs (> 0)");
        }

        for (std::size_t i = 1; i < sims.size(); ++i) {
            requireIdenticalSimulations(*sims[0], *sims[i], label, step);
        }
    };

    // Dense cluster, negative coordinates, and mixed phases
    for (int step = 0; step < 15; ++step) {
        stepAll(Input{}, false, "dense-mixed-parallel", step, true);
    }

    // Click boost and pulse decay
    for (int step = 0; step < 8; ++step) {
        Input input{};
        if (step == 0) {
            input.pointer = Vec2{300.f, 300.f};
            input.pointerActive = true;
            input.click = true;
        }
        stepAll(input, false, "click-boost-parallel", step, true);
    }

    // Dynamic configuration: random color and updated exclusions
    settings.randomColor = true;
    settings.color = Color{0.9f, 0.2f, 0.4f};
    const std::array<Bounds, 3> updatedExclusions{{
        {-200.f, 850.f, 700.f, 950.f},
        {800.f, 860.f, 1600.f, 950.f},
        {350.f, 250.f, 550.f, 400.f}
    }};
    for (auto* sim : sims) {
        sim->configure(settings, bounds, updatedExclusions);
    }
    for (int step = 0; step < 12; ++step) {
        stepAll(Input{}, false, "configure-color-exclusions", step, true);
    }

    // Paused simulation
    for (int step = 0; step < 5; ++step) {
        stepAll(Input{}, true, "paused-simulation", step, false);
    }

    // Small population: explicitly restore low count to guarantee serial execution (<1000)
    const int smallCount = std::min(80, highCount / 4);
    std::vector<Bird> smallFixture;
    smallFixture.reserve(static_cast<std::size_t>(smallCount));
    for (int i = 0; i < smallCount; ++i) {
        smallFixture.push_back(heavyFixture[static_cast<std::size_t>(i)]);
    }
    settings.count = smallCount;
    for (auto* sim : sims) {
        sim->configure(settings, bounds, updatedExclusions);
        sim->restore(std::span<const Bird>(smallFixture.data(), smallFixture.size()));
    }
    for (int step = 0; step < 15; ++step) {
        stepAll(Input{}, false, "small-population-serial", step, false);
    }

    // Population reduced to zero and restore empty
    settings.count = 0;
    for (auto* sim : sims) {
        sim->configure(settings, bounds, updatedExclusions);
        sim->restore(std::span<const Bird>{});
    }
    for (int step = 0; step < 4; ++step) {
        stepAll(Input{}, false, "empty-population", step, false);
    }

    // Dynamic replenishment from empty: verify birth logic determinism
    settings.count = 150;
    for (auto* sim : sims) {
        sim->configure(settings, bounds, updatedExclusions);
    }
    for (int step = 0; step < 20; ++step) {
        stepAll(Input{}, false, "dynamic-replenish", step, false);
    }

    // Explicit large flock restore to verify worker pool re-waking
    settings.count = highCount;
    for (auto* sim : sims) {
        sim->configure(settings, bounds, updatedExclusions);
        sim->restore(std::span<const Bird>(heavyFixture.data(), heavyFixture.size()));
    }
    for (int step = 0; step < 15; ++step) {
        stepAll(Input{}, false, "restore-rewake-parallel", step, true);
    }

    // Deterministic reset
    for (auto* sim : sims) {
        sim->reset(0x76543210u);
        sim->configure(settings, bounds, exclusions);
    }
    for (int step = 0; step < 12; ++step) {
        stepAll(Input{}, false, "post-reset-determinism", step, false);
    }
}

void run(std::string_view name, void (*test)()) {
    test();
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main() {
    try {
        run("neighbors / radius oracle / phase eligibility", verifyNeighborsAgainstOracle);
        run("boundary contact / death / pause / replenish / ghost isolation",
            verifyBoundaryDeathPauseAndReplenish);
        run("entering bird crosses the boundary inward", verifyEnteringBirdCrossesBoundaryInward);
        run("per-monitor taskbar exclusion contact and safe crossings",
            verifyTaskbarExclusionBehavior);
        run("rapid population target convergence", verifyLatestPopulationTarget);
        run("speed error and independent steering weights", verifySpeedAndIndependentWeights);
        run("click boost refresh and restoration", verifyClickBoostRefreshAndRestore);
        run("history recording and paused visual settings", verifyHistoryAndPausedVisualSettings);
        run("epoch rebase preserves movement, boost, trails, and ghost age",
            verifyEpochRebaseContinuity);
        run("random color independence and gradual transition",
            verifyRandomColorIndependenceAndTransition);
        run("parallel determinism / interleaved stepping / state transitions",
            verifyParallelDeterminismAndStateTransitions);
        std::cout << "All native simulation behavior checks passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
