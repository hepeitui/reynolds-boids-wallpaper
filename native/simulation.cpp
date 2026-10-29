#include "simulation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
namespace flock {
namespace {
constexpr float Pi = 3.14159265358979323846f;
constexpr float Tau = 2.f * Pi;
constexpr float Radius = PerceptionRadius;
constexpr float RadiusSquared = Radius * Radius;
constexpr float SeparationDistance = flock::SeparationRadius;
constexpr float SeparationRadiusSquared = SeparationDistance * SeparationDistance;
constexpr float CellSize = 40.f;
constexpr float DeathDuration = .28f;
constexpr double HistoryInterval = 1.0 / 60.0;
constexpr float HistoryLifetime = .5f;
constexpr double ClockRebaseThreshold = 120.0;
constexpr double ClockRebasePeriod = 6.2831853071795864769 / 7.0;
constexpr std::size_t MinimumHashSize = 16384;
constexpr std::size_t RequiredHashSize = static_cast<std::size_t>(MaxBirds) * 4;
constexpr int NeighborHashSize = static_cast<int>(
    std::bit_ceil(std::max(MinimumHashSize, RequiredHashSize)));
constexpr int NeighborHashMask = NeighborHashSize - 1;
constexpr int GroupHashSize = static_cast<int>(
    std::bit_ceil(std::max(MinimumHashSize, RequiredHashSize)));
constexpr int GroupHashMask = GroupHashSize - 1;
constexpr int GhostCapacity = MaxGhosts;
constexpr int SectorCount = 16;
constexpr int StationarySector = SectorCount;
constexpr int LeaderCount = 3;
constexpr float MinAlignment = .7071067811865475244f;
constexpr float EdgeDenominator = 6.3890560989306502272f; // expm1(2)
constexpr std::array<int, 5> CellOffsets{0, -1, 1, -2, 2};
constexpr std::array<int, 7> SectorOffsets{0, -1, 1, -2, 2, -3, 3};

unsigned resolveWorkerCount(unsigned workers) {
    if (workers == 1) {
        return 1;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned available = hw > 0 ? hw : 1u;
    if (workers == 0) {
        return std::clamp(available, 1u, 8u);
    }
    const unsigned requested = std::clamp(workers, 1u, 8u);
    return std::clamp(requested, 1u, available);
}

float clampFloat(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(maximum, value));
}

float quantize(float value, float minimum, float maximum, float step, float fallback) {
    if (!std::isfinite(value)) value = fallback;
    const float bounded = clampFloat(value, minimum, maximum);
    const float units = std::round((bounded - minimum) / step);
    return clampFloat(minimum + units * step, minimum, maximum);
}

Settings sanitizeSettings(const Settings& source) {
    const Settings defaults{};
    Settings result = source;
    result.count = static_cast<int>(quantize(static_cast<float>(source.count), 0.f,
                                             static_cast<float>(MaxBirds), 1.f,
                                             static_cast<float>(defaults.count)));
    result.size = quantize(source.size, 1.f, 16.f, .01f, defaults.size);
    result.speed = quantize(source.speed, 20.f, 240.f, 1.f, defaults.speed);
    result.speedError = quantize(source.speedError, 0.f, .5f, .01f, defaults.speedError);
    result.separation = quantize(source.separation, 0.f, 4.f, .01f, defaults.separation);
    result.alignment = quantize(source.alignment, 0.f, 4.f, .01f, defaults.alignment);
    result.cohesion = quantize(source.cohesion, 0.f, 4.f, .01f, defaults.cohesion);
    result.edge = quantize(source.edge, 0.f, .35f, .001f, defaults.edge);
    result.trailDuration = quantize(source.trailDuration, .1f, .5f, .01f,
                                    defaults.trailDuration);
    result.brightness = quantize(source.brightness, .1f, 2.f, .01f, defaults.brightness);
    result.contrast = quantize(source.contrast, .5f, 2.f, .01f, defaults.contrast);
    result.marginLeft = quantize(source.marginLeft, 0.f, 200.f, 1.f, defaults.marginLeft);
    result.marginRight = quantize(source.marginRight, 0.f, 200.f, 1.f, defaults.marginRight);
    result.marginTop = quantize(source.marginTop, 0.f, 200.f, 1.f, defaults.marginTop);
    result.marginBottom = quantize(source.marginBottom, 0.f, 200.f, 1.f, defaults.marginBottom);
    result.renderScale = quantize(source.renderScale, .5f, 1.f, .01f, defaults.renderScale);
    result.fps = static_cast<int>(quantize(static_cast<float>(source.fps), 15.f, 120.f,
                                           1.f, static_cast<float>(defaults.fps)));
    result.trailHz = static_cast<int>(quantize(static_cast<float>(source.trailHz), 10.f,
                                               60.f, 1.f,
                                               static_cast<float>(defaults.trailHz)));
    result.color.r = std::isfinite(source.color.r)
                         ? clampFloat(source.color.r, 0.f, 1.f)
                         : defaults.color.r;
    result.color.g = std::isfinite(source.color.g)
                         ? clampFloat(source.color.g, 0.f, 1.f)
                         : defaults.color.g;
    result.color.b = std::isfinite(source.color.b)
                         ? clampFloat(source.color.b, 0.f, 1.f)
                         : defaults.color.b;
    return result;
}

Bounds sanitizeBounds(Bounds bounds, const Bounds& fallback) {
    if (!std::isfinite(bounds.left) || !std::isfinite(bounds.top) ||
        !std::isfinite(bounds.right) || !std::isfinite(bounds.bottom)) {
        return fallback;
    }
    if (bounds.right - bounds.left < 1.f) {
        const float center = (bounds.left + bounds.right) * .5f;
        bounds.left = center - .5f;
        bounds.right = center + .5f;
    }
    if (bounds.bottom - bounds.top < 1.f) {
        const float center = (bounds.top + bounds.bottom) * .5f;
        bounds.top = center - .5f;
        bounds.bottom = center + .5f;
    }
    return bounds;
}

int cellCoordinate(float coordinate) {
    const double cell = std::floor(static_cast<double>(coordinate) / CellSize);
    constexpr int limit = std::numeric_limits<int>::max() / 4;
    if (cell < -static_cast<double>(limit)) return -limit;
    if (cell > static_cast<double>(limit)) return limit;
    return static_cast<int>(cell);
}

std::uint32_t hashCell(int x, int y) {
    return static_cast<std::uint32_t>(x) * 73856093u ^
           static_cast<std::uint32_t>(y) * 19349663u;
}

std::uint32_t hashGroup(int x, int y, int sector) {
    return static_cast<std::uint32_t>(x) * 73856093u ^
           static_cast<std::uint32_t>(y) * 19349663u ^
           static_cast<std::uint32_t>(sector) * 83492791u;
}

float wrappedDifference(float to, float from) {
    const float difference = to - from;
    return difference - std::floor(difference + .5f);
}

float normalizeHue(float hue) {
    hue -= std::floor(hue);
    return hue;
}

Color brightHue(float hue) {
    hue = normalizeHue(hue);
    const auto channel = [hue](float offset) {
        float value = std::fmod(hue * 6.f + offset, 6.f);
        if (value < 0.f) value += 6.f;
        value = std::clamp(std::fabs(value - 3.f) - 1.f, 0.f, 1.f);
        return 1.f + (value - 1.f) * .82f;
    };
    return {channel(0.f), channel(4.f), channel(2.f)};
}

float hueFromColor(Color color) {
    const float maximum = std::max({color.r, color.g, color.b});
    const float minimum = std::min({color.r, color.g, color.b});
    const float delta = maximum - minimum;
    if (delta <= 1e-6f) return .5f;
    float hue;
    if (maximum == color.r) {
        hue = (color.g - color.b) / delta;
        if (hue < 0.f) hue += 6.f;
    } else if (maximum == color.g) {
        hue = (color.b - color.r) / delta + 2.f;
    } else {
        hue = (color.r - color.g) / delta + 4.f;
    }
    return normalizeHue(hue / 6.f);
}

std::uint32_t identityHash(std::uint32_t id, std::uint32_t seed) {
    std::uint32_t value = (id ^ seed ^ 0x9e3779b9u) * 0x21f0aaadu;
    value = (value ^ (value >> 16)) * 0x735a2d97u;
    return value ^ (value >> 15);
}

float edgeForce(float distance, float band) {
    if (band <= 0.f || distance >= band) return 0.f;
    const float exponent = 2.f * std::min(2.f, std::max(0.f, 1.f - distance / band));
    return std::expm1(exponent) / EdgeDenominator;
}

bool containsPoint(const Bounds& rectangle, float x, float y) {
    return x >= rectangle.left && x <= rectangle.right &&
           y >= rectangle.top && y <= rectangle.bottom;
}

float segmentRectangleEntry(float x, float y, float nextX, float nextY,
                            const Bounds& rectangle) {
    if (containsPoint(rectangle, x, y)) return 0.f;
    float entry = 0.f;
    float exit = 1.f;
    const auto clipAxis = [&entry, &exit](float origin, float delta,
                                          float minimum, float maximum) {
        if (delta == 0.f) return origin >= minimum && origin <= maximum;
        float first = (minimum - origin) / delta;
        float last = (maximum - origin) / delta;
        if (first > last) std::swap(first, last);
        entry = std::max(entry, first);
        exit = std::min(exit, last);
        return entry <= exit;
    };
    if (!clipAxis(x, nextX - x, rectangle.left, rectangle.right) ||
        !clipAxis(y, nextY - y, rectangle.top, rectangle.bottom) ||
        exit < 0.f || entry > 1.f) {
        return -1.f;
    }
    return clampFloat(entry, 0.f, 1.f);
}

Vec2 exclusionRepulsion(float x, float y, float velocityX, float velocityY,
                        const Bounds& rectangle, float band) {
    if (band <= 0.f) return {};
    if (containsPoint(rectangle, x, y)) {
        const float distances[4] = {
            x - rectangle.left, rectangle.right - x,
            y - rectangle.top, rectangle.bottom - y,
        };
        const Vec2 normals[4] = {{-1.f, 0.f}, {1.f, 0.f}, {0.f, -1.f}, {0.f, 1.f}};
        int closest = 0;
        for (int side = 1; side < 4; ++side) {
            if (distances[side] < distances[closest] ||
                (distances[side] == distances[closest] &&
                 velocityX * normals[side].x + velocityY * normals[side].y <
                     velocityX * normals[closest].x + velocityY * normals[closest].y)) {
                closest = side;
            }
        }
        return normals[closest];
    }
    const float closestX = clampFloat(x, rectangle.left, rectangle.right);
    const float closestY = clampFloat(y, rectangle.top, rectangle.bottom);
    const float dx = x - closestX;
    const float dy = y - closestY;
    const float distance = std::hypot(dx, dy);
    if (distance <= 1e-6f || distance >= band) return {};
    const float magnitude = edgeForce(distance, band);
    return {dx * magnitude / distance, dy * magnitude / distance};
}

bool isNeighborLive(const Bird& bird) {
    return bird.phase == Phase::Entering || bird.phase == Phase::Active;
}

bool validPhase(Phase phase) {
    switch (phase) {
        case Phase::Entering:
        case Phase::Active:
        case Phase::Leaving:
        case Phase::Dying:
            return true;
        case Phase::Dead:
            return false;
    }
    return false;
}

float pointTime(const std::array<TrailPoint, HistoryCapacity>& history,
                int head, int count) {
    if (count <= 0 || head < 0 || head >= HistoryCapacity) return -std::numeric_limits<float>::infinity();
    return history[static_cast<std::size_t>(head)].time;
}

void appendTrail(Bird& bird, Vec2 position, float time, Color color) {
    if (bird.historyCount > 0 && bird.historyHead >= 0 &&
        bird.historyHead < HistoryCapacity) {
        TrailPoint& last = bird.history[static_cast<std::size_t>(bird.historyHead)];
        if (std::fabs(last.time - time) <= 1e-6f) {
            last.position = position;
            last.color = color;
            bird.lastSample = time;
            return;
        }
    }
    const int next = (bird.historyHead + 1) % HistoryCapacity;
    bird.history[static_cast<std::size_t>(next)] = {position, time, color};
    bird.historyHead = next;
    bird.historyCount = std::min(HistoryCapacity, bird.historyCount + 1);
    bird.lastSample = time;
}


} // namespace

struct Simulation::Impl {
    struct NeighborCell {
        int x = 0;
        int y = 0;
        int head = -1;
        int load = 0;
        float sumX = 0.f;
        float sumY = 0.f;
        float sumUnitX = 0.f;
        float sumUnitY = 0.f;
    };

    struct GroupCell {
        int x = 0;
        int y = 0;
        int sector = 0;
        int head = -1;
        int load = 0;
        int coreHead = -1;
        int parent = -1;
        float angleMin = 0.f;
        float angleMax = 0.f;
    };

    struct Motion {
        float oldX = 0.f;
        float oldY = 0.f;
        float oldHue = 0.f;
        float newX = 0.f;
        float newY = 0.f;
        float newHue = 0.f;
        double moveEndTime = 0.0;
        float phaseTime = 0.f;
        float boost = 0.f;
        Vec2 newVelocity{};
        Phase phase = Phase::Dead;
        bool deathContact = false;
        double contactTime = 0.0;
        Vec2 contact{};
        bool retire = false;
    };

    explicit Impl(std::uint32_t requestedSeed, unsigned requestedWorkers = 0)
        : configuredWorkers(resolveWorkerCount(requestedWorkers)),
          initialSeed(requestedSeed),
          randomState(requestedSeed) {
        settings = sanitizeSettings(Settings{});
        bounds = Bounds{};
        neighborBuckets.fill(-1);
        groupBuckets.fill(-1);
        for (Bird& bird : birdSlots) {
            bird.phase = Phase::Dead;
            bird.historyHead = -1;
            bird.lastSample = -100.f;
        }
        for (Ghost& ghost : ghostSlots) {
            ghost.historyHead = -1;
            ghost.retiredAt = -1.f;
        }
    }

    ~Impl() {
        stopWorkers();
    }

    Settings settings{};
    Bounds bounds{};
    std::vector<Bounds> exclusions;
    std::array<Bird, MaxBirds> birdSlots{};
    std::array<Ghost, GhostCapacity> ghostSlots{};
    std::array<int, MaxBirds> activeSlots{};
    std::array<Vec2, MaxBirds> exitTargets{};
    std::array<float, MaxBirds> boosts{};
    int activeCount = 0;
    std::array<Motion, MaxBirds> motion{};

    std::array<int, NeighborHashSize> neighborBuckets{};
    std::array<NeighborCell, MaxBirds> neighborCells{};
    std::array<int, MaxBirds> neighborNext{};
    int neighborCellCount = 0;
    bool neighborGridDirty = true;
    std::array<float, MaxBirds> unitX{};
    std::array<float, MaxBirds> unitY{};
    std::array<float, MaxBirds> posX{};
    std::array<float, MaxBirds> posY{};

    std::array<int, GroupHashSize> groupBuckets{};
    std::array<GroupCell, MaxBirds> groupCells{};
    std::array<int, MaxBirds> groupNext{};
    std::array<int, MaxBirds> groupSlotCell{};
    std::array<int, MaxBirds> groupMembership{};
    std::array<int, MaxBirds> groupNextCore{};
    std::array<std::uint8_t, MaxBirds> groupCore{};
    std::array<float, MaxBirds> groupUnitX{};
    std::array<float, MaxBirds> groupUnitY{};
    std::array<float, MaxBirds> groupAngle{};
    std::array<int, MaxBirds> groupSize{};
    std::array<float, MaxBirds> groupSumX{};
    std::array<float, MaxBirds> groupSumY{};
    std::array<float, MaxBirds> groupDirectionX{};
    std::array<float, MaxBirds> groupDirectionY{};
    std::array<int, MaxBirds> groupHeadingSlot{};
    std::array<int, MaxBirds * LeaderCount> leaders{};
    std::array<float, MaxBirds * LeaderCount> leaderScores{};
    int groupCellCount = 0;

    std::array<float, MaxBirds> baseHue{};
    std::array<float, MaxBirds> hue{};
    std::array<float, MaxBirds> previousHue{};
    std::array<float, MaxBirds> targetHue{};

    std::uint32_t initialSeed = 42;
    std::uint32_t randomState = 42;
    std::uint32_t nextId = 1;
    double simulationTime = 0.0;
    double nextColorUpdate = 0.0;
    double lastHistoryTime = -1.0;
    float spawnCredit = 0.f;
    float pulseX = 0.f;
    float pulseY = 0.f;
    float pulseUntil = -1.f;
    int ghostCursor = 0;
    SimStats currentStats{};

    static constexpr int MotionChunkSize = 64;
    static constexpr int MinParallelActiveCount = 1000;
    static constexpr unsigned MaxWorkerParticipants = 8;

    struct WorkerContext {
        alignas(64) std::uint64_t candidateCount = 0;
    };

    struct MotionParams {
        float dt;
        Input input;
        double startClock;
        double endClock;
        float endTime;
        float width;
        float height;
        float bandX;
        float bandY;
        float exclusionBand;
    };

    unsigned configuredWorkers = 1;
    bool workersStarted = false;
    std::vector<std::thread> workers;
    std::mutex poolMutex;
    std::condition_variable cvStart;
    std::condition_variable cvDone;
    bool poolStop = false;
    std::uint64_t currentWorkEpoch = 0;
    std::atomic<int> nextChunkIndex{0};
    int totalChunkCount = 0;
    int runningWorkers = 0;
    const MotionParams* activeMotionParams = nullptr;
    std::array<WorkerContext, MaxWorkerParticipants> workerContexts{};

    float random() {
        std::uint32_t value = randomState;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        randomState = value;
        return static_cast<float>(value) / 4294967296.f;
    }

    bool inExclusion(float x, float y) const {
        for (const Bounds& rectangle : exclusions) {
            if (containsPoint(rectangle, x, y)) return true;
        }
        return false;
    }
    void rebaseClock() {
        if (simulationTime < ClockRebaseThreshold) return;
        const double cycles = std::floor((simulationTime - 1.0) / ClockRebasePeriod);
        if (cycles <= 0.0) return;
        const double shift = cycles * ClockRebasePeriod;
        simulationTime -= shift;
        nextColorUpdate -= shift;
        if (lastHistoryTime >= 0.0) lastHistoryTime -= shift;
        if (pulseUntil >= 0.f)
            pulseUntil = static_cast<float>(static_cast<double>(pulseUntil) - shift);
        const auto shiftHistory = [shift](auto& history, int head, int count) {
            if (count <= 0 || head < 0 || head >= HistoryCapacity) return;
            const int oldest = (head - count + 1 + HistoryCapacity * 2) % HistoryCapacity;
            for (int offset = 0; offset < count; ++offset) {
                const int index = (oldest + offset) % HistoryCapacity;
                TrailPoint& point = history[static_cast<std::size_t>(index)];
                point.time = static_cast<float>(static_cast<double>(point.time) - shift);
            }
        };
        for (Bird& bird : birdSlots) {
            if (bird.historyCount > 0) {
                shiftHistory(bird.history, bird.historyHead, bird.historyCount);
                if (std::isfinite(bird.lastSample))
                    bird.lastSample = static_cast<float>(static_cast<double>(bird.lastSample) - shift);
            }
            if (std::isfinite(bird.boostUntil) && bird.boostUntil >= 0.f)
                bird.boostUntil = static_cast<float>(static_cast<double>(bird.boostUntil) - shift);
        }
        for (Ghost& ghost : ghostSlots) {
            if (ghost.historyCount <= 0) continue;
            shiftHistory(ghost.history, ghost.historyHead, ghost.historyCount);
            ghost.retiredAt = static_cast<float>(static_cast<double>(ghost.retiredAt) - shift);
        }
    }

    int findNeighborCell(int x, int y, bool create) {
        int bucket = static_cast<int>(hashCell(x, y) & NeighborHashMask);
        for (int probes = 0; probes < NeighborHashSize; ++probes) {
            const int cell = neighborBuckets[static_cast<std::size_t>(bucket)];
            if (cell < 0) {
                if (!create || neighborCellCount >= MaxBirds) return -1;
                const int created = neighborCellCount++;
                NeighborCell& entry = neighborCells[static_cast<std::size_t>(created)];
                entry = {};
                entry.x = x;
                entry.y = y;
                entry.head = -1;
                neighborBuckets[static_cast<std::size_t>(bucket)] = created;
                return created;
            }
            const NeighborCell& entry = neighborCells[static_cast<std::size_t>(cell)];
            if (entry.x == x && entry.y == y) return cell;
            bucket = (bucket + 1) & NeighborHashMask;
        }
        return -1;
    }

    int findNeighborCell(int x, int y) const {
        int bucket = static_cast<int>(hashCell(x, y) & NeighborHashMask);
        for (int probes = 0; probes < NeighborHashSize; ++probes) {
            const int cell = neighborBuckets[static_cast<std::size_t>(bucket)];
            if (cell < 0) return -1;
            const NeighborCell& entry = neighborCells[static_cast<std::size_t>(cell)];
            if (entry.x == x && entry.y == y) return cell;
            bucket = (bucket + 1) & NeighborHashMask;
        }
        return -1;
    }

    void buildNeighborGrid() {
        neighborBuckets.fill(-1);
        neighborCellCount = 0;
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            const Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            posX[static_cast<std::size_t>(slot)] = bird.position.x;
            posY[static_cast<std::size_t>(slot)] = bird.position.y;
            if (bird.phase == Phase::Dying || bird.phase == Phase::Dead) {
                unitX[static_cast<std::size_t>(slot)] = 0.f;
                unitY[static_cast<std::size_t>(slot)] = 0.f;
                neighborNext[static_cast<std::size_t>(slot)] = -1;
                continue;
            }
            const float speed = std::hypot(bird.velocity.x, bird.velocity.y);
            const float inverse = speed > 1e-12f ? 1.f / speed : 0.f;
            unitX[static_cast<std::size_t>(slot)] = bird.velocity.x * inverse;
            unitY[static_cast<std::size_t>(slot)] = bird.velocity.y * inverse;
            if (!isNeighborLive(bird)) {
                neighborNext[static_cast<std::size_t>(slot)] = -1;
                continue;
            }
            const int x = cellCoordinate(bird.position.x);
            const int y = cellCoordinate(bird.position.y);
            const int cellIndex = findNeighborCell(x, y, true);
            if (cellIndex < 0) continue;
            NeighborCell& cell = neighborCells[static_cast<std::size_t>(cellIndex)];
            neighborNext[static_cast<std::size_t>(slot)] = cell.head;
            cell.head = slot;
            ++cell.load;
            cell.sumX += bird.position.x;
            cell.sumY += bird.position.y;
            cell.sumUnitX += unitX[static_cast<std::size_t>(slot)];
            cell.sumUnitY += unitY[static_cast<std::size_t>(slot)];
        }
        neighborGridDirty = false;
    }

    int queryNeighbors(int source, std::span<int> output) {
        if (source < 0 || source >= MaxBirds ||
            !isNeighborLive(birdSlots[static_cast<std::size_t>(source)])) return 0;
        if (neighborGridDirty) buildNeighborGrid();
        const Bird& bird = birdSlots[static_cast<std::size_t>(source)];
        const int minX = cellCoordinate(bird.position.x - Radius);
        const int maxX = cellCoordinate(bird.position.x + Radius);
        const int minY = cellCoordinate(bird.position.y - Radius);
        const int maxY = cellCoordinate(bird.position.y + Radius);
        int written = 0;
        for (int cy = minY; cy <= maxY; ++cy) {
            for (int cx = minX; cx <= maxX; ++cx) {
                const int cellIndex = findNeighborCell(cx, cy);
                if (cellIndex < 0) continue;
                for (int candidate = neighborCells[static_cast<std::size_t>(cellIndex)].head;
                     candidate >= 0;
                     candidate = neighborNext[static_cast<std::size_t>(candidate)]) {
                    if (candidate == source) continue;
                    const float dx = posX[static_cast<std::size_t>(candidate)] - bird.position.x;
                    const float dy = posY[static_cast<std::size_t>(candidate)] - bird.position.y;
                    if (dx * dx + dy * dy <= RadiusSquared &&
                        written < static_cast<int>(output.size())) {
                        output[static_cast<std::size_t>(written++)] = candidate;
                    }
                }
            }
        }
        return written;
    }

    void aggregateForces(int source, float& count, float& sumX, float& sumY,
                         float& directionX, float& directionY,
                         float& separationX, float& separationY,
                         std::uint64_t& localCandidates) {
        const Bird& bird = birdSlots[static_cast<std::size_t>(source)];
        const float x = bird.position.x;
        const float y = bird.position.y;
        const int minX = cellCoordinate(x - Radius);
        const int maxX = cellCoordinate(x + Radius);
        const int minY = cellCoordinate(y - Radius);
        const int maxY = cellCoordinate(y + Radius);
        const bool separate = settings.separation != 0.f;
        for (int cy = minY; cy <= maxY; ++cy) {
            for (int cx = minX; cx <= maxX; ++cx) {
                const int index = findNeighborCell(cx, cy);
                if (index < 0) continue;
                const NeighborCell& cell = neighborCells[static_cast<std::size_t>(index)];
                const float left = static_cast<float>(cx) * CellSize;
                const float top = static_cast<float>(cy) * CellSize;
                const float farX = std::max(std::fabs(left - x), std::fabs(left + CellSize - x));
                const float farY = std::max(std::fabs(top - y), std::fabs(top + CellSize - y));
                const bool whole = farX * farX + farY * farY <= RadiusSquared;
                const float nearX = x < left ? left - x : x > left + CellSize ? x - (left + CellSize) : 0.f;
                const float nearY = y < top ? top - y : y > top + CellSize ? y - (top + CellSize) : 0.f;
                const float nearDistanceSquared = nearX * nearX + nearY * nearY;
                if (!whole && nearDistanceSquared > RadiusSquared) continue;
                if (whole) {
                    const int own = cell.x == cellCoordinate(x) && cell.y == cellCoordinate(y) ? 1 : 0;
                    const int neighbours = cell.load - own;
                    count += static_cast<float>(neighbours);
                    sumX += cell.sumX - static_cast<float>(neighbours + own) * x;
                    sumY += cell.sumY - static_cast<float>(neighbours + own) * y;
                    directionX += cell.sumUnitX - static_cast<float>(own) * unitX[static_cast<std::size_t>(source)];
                    directionY += cell.sumUnitY - static_cast<float>(own) * unitY[static_cast<std::size_t>(source)];
                    if (!separate) continue;
                    if (nearDistanceSquared > SeparationRadiusSquared) continue;
                }
                for (int candidate = cell.head; candidate >= 0;
                     candidate = neighborNext[static_cast<std::size_t>(candidate)]) {
                    if (candidate == source) continue;
                    ++localCandidates;
                    const float dx = posX[static_cast<std::size_t>(candidate)] - x;
                    const float dy = posY[static_cast<std::size_t>(candidate)] - y;
                    const float distanceSquared = dx * dx + dy * dy;
                    if (!whole && distanceSquared <= RadiusSquared) {
                        count += 1.f;
                        sumX += dx;
                        sumY += dy;
                        directionX += unitX[static_cast<std::size_t>(candidate)];
                        directionY += unitY[static_cast<std::size_t>(candidate)];
                    }
                    if (separate && distanceSquared < SeparationRadiusSquared &&
                        distanceSquared > 1e-8f) {
                        const float distance = std::sqrt(distanceSquared);
                        const float factor = (1.f - distance / SeparationDistance) / distance;
                        separationX -= dx * factor;
                        separationY -= dy * factor;
                    }
                }
            }
        }
    }

    float randomIdentityHue(std::uint32_t id) const {
        return static_cast<float>(identityHash(id, initialSeed)) / 4294967296.f;
    }

    void spawn() {
        int slot = 0;
        while (slot < MaxBirds && birdSlots[static_cast<std::size_t>(slot)].phase != Phase::Dead) ++slot;
        if (slot >= MaxBirds || activeCount >= MaxBirds) return;
        Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
        bird = Bird{};
        bird.historyHead = -1;
        bird.lastSample = -100.f;
        const int side = static_cast<int>(random() * 4.f);
        const float width = bounds.right - bounds.left;
        const float height = bounds.bottom - bounds.top;
        bird.position.x = side == 0 ? bounds.left - 24.f
                         : side == 1 ? bounds.right + 24.f
                                     : bounds.left + random() * width;
        bird.position.y = side == 2 ? bounds.top - 24.f
                         : side == 3 ? bounds.bottom + 24.f
                                     : bounds.top + random() * height;
        const float targetX = bounds.left + width * (.25f + random() * .5f);
        const float targetY = bounds.top + height * (.25f + random() * .5f);
        const float angle = std::atan2(targetY - bird.position.y, targetX - bird.position.x);
        bird.speedBias = random() * 2.f - 1.f;
        const float speed = settings.speed * (1.f + bird.speedBias * settings.speedError);
        bird.velocity = {std::cos(angle) * speed, std::sin(angle) * speed};
        bird.id = nextId++;
        if (nextId == 0) nextId = 1;
        baseHue[static_cast<std::size_t>(slot)] = randomIdentityHue(bird.id);
        hue[static_cast<std::size_t>(slot)] = baseHue[static_cast<std::size_t>(slot)];
        previousHue[static_cast<std::size_t>(slot)] = hue[static_cast<std::size_t>(slot)];
        targetHue[static_cast<std::size_t>(slot)] = hue[static_cast<std::size_t>(slot)];
        bird.baseColor = brightHue(baseHue[static_cast<std::size_t>(slot)]);
        bird.color = bird.baseColor;
        bird.phase = Phase::Entering;
        bird.phaseTime = 0.f;
        bird.boostUntil = -1.f;
        boosts[static_cast<std::size_t>(slot)] = 0.f;
        bird.wingPhase = static_cast<float>((static_cast<std::uint64_t>(bird.id) * 2654435761u) & 0xffffu) *
                         (Tau / 65536.f);
        activeSlots[static_cast<std::size_t>(activeCount++)] = slot;
    }

    void setLeaveTarget(int slot) {
        Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
        const float distances[4] = {
            bird.position.x - bounds.left,
            bounds.right - bird.position.x,
            bird.position.y - bounds.top,
            bounds.bottom - bird.position.y,
        };
        int side = 0;
        for (int i = 1; i < 4; ++i)
            if (distances[i] < distances[side]) side = i;
        exitTargets[static_cast<std::size_t>(slot)] = bird.position;
        if (side == 0) exitTargets[static_cast<std::size_t>(slot)].x = bounds.left - 80.f;
        else if (side == 1) exitTargets[static_cast<std::size_t>(slot)].x = bounds.right + 80.f;
        else if (side == 2) exitTargets[static_cast<std::size_t>(slot)].y = bounds.top - 80.f;
        else exitTargets[static_cast<std::size_t>(slot)].y = bounds.bottom + 80.f;
    }

    void livePopulation(int& living, int& dying) const {
        living = 0;
        dying = 0;
        for (int a = 0; a < activeCount; ++a) {
            const Phase phase = birdSlots[static_cast<std::size_t>(activeSlots[static_cast<std::size_t>(a)])].phase;
            if (phase == Phase::Dying) ++dying;
            else if (phase != Phase::Leaving) ++living;
        }
    }

    void replenish() {
        int living = 0;
        int dying = 0;
        livePopulation(living, dying);
        while (living + dying < settings.count && activeCount < MaxBirds && spawnCredit >= 1.f) {
            const int before = activeCount;
            spawn();
            if (activeCount == before) break;
            spawnCredit -= 1.f;
            ++living;
        }
    }

    void reconcile(float dt) {
        int living = 0;
        int dying = 0;
        livePopulation(living, dying);
        int desired = living;
        if (desired < settings.count) {
            for (int a = 0; a < activeCount && desired < settings.count; ++a) {
                Bird& bird = birdSlots[static_cast<std::size_t>(activeSlots[static_cast<std::size_t>(a)])];
                if (bird.phase == Phase::Leaving) {
                    bird.phase = Phase::Entering;
                    bird.phaseTime = 0.f;
                    ++desired;
                }
            }
        }
        if (desired > settings.count) {
            int budget = std::max(1, static_cast<int>(std::ceil(450.f * dt)));
            for (int a = activeCount - 1; a >= 0 && desired > settings.count && budget > 0; --a) {
                const int slot = activeSlots[static_cast<std::size_t>(a)];
                Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
                if (bird.phase == Phase::Leaving || bird.phase == Phase::Dying) continue;
                setLeaveTarget(slot);
                bird.phase = Phase::Leaving;
                bird.phaseTime = 0.f;
                --desired;
                --budget;
            }
        }
        spawnCredit = std::min(30.f, spawnCredit + 450.f * dt);
        replenish();
    }

    int findGroupCell(int x, int y, int sector, bool create) {
        int bucket = static_cast<int>(hashGroup(x, y, sector) & GroupHashMask);
        for (int probes = 0; probes < GroupHashSize; ++probes) {
            const int cell = groupBuckets[static_cast<std::size_t>(bucket)];
            if (cell < 0) {
                if (!create || groupCellCount >= MaxBirds) return -1;
                const int created = groupCellCount++;
                GroupCell& entry = groupCells[static_cast<std::size_t>(created)];
                entry = {};
                entry.x = x;
                entry.y = y;
                entry.sector = sector;
                entry.head = -1;
                entry.coreHead = -1;
                entry.parent = created;
                groupBuckets[static_cast<std::size_t>(bucket)] = created;
                return created;
            }
            const GroupCell& entry = groupCells[static_cast<std::size_t>(cell)];
            if (entry.x == x && entry.y == y && entry.sector == sector) return cell;
            bucket = (bucket + 1) & GroupHashMask;
        }
        return -1;
    }

    int findGroupCell(int x, int y, int sector) const {
        int bucket = static_cast<int>(hashGroup(x, y, sector) & GroupHashMask);
        for (int probes = 0; probes < GroupHashSize; ++probes) {
            const int cell = groupBuckets[static_cast<std::size_t>(bucket)];
            if (cell < 0) return -1;
            const GroupCell& entry = groupCells[static_cast<std::size_t>(cell)];
            if (entry.x == x && entry.y == y && entry.sector == sector) return cell;
            bucket = (bucket + 1) & GroupHashMask;
        }
        return -1;
    }

    int groupRoot(int cell) {
        while (cell != groupCells[static_cast<std::size_t>(cell)].parent) {
            GroupCell& entry = groupCells[static_cast<std::size_t>(cell)];
            const int parent = entry.parent;
            entry.parent = groupCells[static_cast<std::size_t>(parent)].parent;
            cell = entry.parent;
        }
        return cell;
    }

    bool aligned(int first, int second) const {
        const float firstX = groupUnitX[static_cast<std::size_t>(first)];
        const float firstY = groupUnitY[static_cast<std::size_t>(first)];
        const float secondX = groupUnitX[static_cast<std::size_t>(second)];
        const float secondY = groupUnitY[static_cast<std::size_t>(second)];
        if (firstX == 0.f && firstY == 0.f)
            return secondX == 0.f && secondY == 0.f;
        return firstX * secondX + firstY * secondY >= MinAlignment - 1e-12f;
    }

    void buildGroups() {
        groupBuckets.fill(-1);
        groupCellCount = 0;
        groupCore.fill(0);
        groupMembership.fill(-1);
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            const Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            if (!isNeighborLive(bird)) {
                groupSlotCell[static_cast<std::size_t>(slot)] = -1;
                continue;
            }
            const float speed = std::hypot(bird.velocity.x, bird.velocity.y);
            const bool moving = speed > 1e-6f;
            groupUnitX[static_cast<std::size_t>(slot)] = moving ? bird.velocity.x / speed : 0.f;
            groupUnitY[static_cast<std::size_t>(slot)] = moving ? bird.velocity.y / speed : 0.f;
            const float angle = moving ? std::atan2(bird.velocity.y, bird.velocity.x) : 0.f;
            groupAngle[static_cast<std::size_t>(slot)] = angle;
            const float normalized = angle < 0.f ? angle + Tau : angle;
            const int sector = moving
                                   ? (static_cast<int>(std::floor(normalized * SectorCount / Tau)) & (SectorCount - 1))
                                   : StationarySector;
            const int cx = cellCoordinate(bird.position.x);
            const int cy = cellCoordinate(bird.position.y);
            const int cellIndex = findGroupCell(cx, cy, sector, true);
            if (cellIndex < 0) continue;
            groupSlotCell[static_cast<std::size_t>(slot)] = cellIndex;
            groupNext[static_cast<std::size_t>(slot)] = groupCells[static_cast<std::size_t>(cellIndex)].head;
            groupCells[static_cast<std::size_t>(cellIndex)].head = slot;
            ++groupCells[static_cast<std::size_t>(cellIndex)].load;
        }

        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            if (!isNeighborLive(birdSlots[static_cast<std::size_t>(slot)])) continue;
            const int sourceCell = groupSlotCell[static_cast<std::size_t>(slot)];
            if (sourceCell < 0) continue;
            const GroupCell& source = groupCells[static_cast<std::size_t>(sourceCell)];
            int companions = source.load - 1;
            if (companions >= 4) {
                groupCore[static_cast<std::size_t>(slot)] = 1;
                continue;
            }
            const int sector = source.sector;
            const Vec2 position = birdSlots[static_cast<std::size_t>(slot)].position;
            for (int oy : CellOffsets) {
                if (companions >= 4) break;
                const int cy = source.y + oy;
                const float gapY = std::max({static_cast<float>(cy) * CellSize - position.y,
                                             position.y - (static_cast<float>(cy + 1) * CellSize), 0.f});
                for (int ox : CellOffsets) {
                    if (companions >= 4) break;
                    const int cx = source.x + ox;
                    const float gapX = std::max({static_cast<float>(cx) * CellSize - position.x,
                                                 position.x - (static_cast<float>(cx + 1) * CellSize), 0.f});
                    if (gapX * gapX + gapY * gapY > RadiusSquared) continue;
                    const int sectorCount = sector == StationarySector ? 1 : static_cast<int>(SectorOffsets.size());
                    for (int os = 0; os < sectorCount && companions < 4; ++os) {
                        const int candidateSector = sector == StationarySector
                            ? StationarySector
                            : (sector + SectorOffsets[static_cast<std::size_t>(os)] + SectorCount) & (SectorCount - 1);
                        const int candidateCell = findGroupCell(cx, cy, candidateSector);
                        if (candidateCell < 0 || candidateCell == sourceCell) continue;
                        for (int other = groupCells[static_cast<std::size_t>(candidateCell)].head;
                             other >= 0;
                             other = groupNext[static_cast<std::size_t>(other)]) {
                            const Vec2 otherPosition = birdSlots[static_cast<std::size_t>(other)].position;
                            const float dx = otherPosition.x - position.x;
                            const float dy = otherPosition.y - position.y;
                            if (dx * dx + dy * dy <= RadiusSquared && aligned(slot, other) && ++companions >= 4)
                                break;
                        }
                    }
                }
            }
            if (companions >= 4) groupCore[static_cast<std::size_t>(slot)] = 1;
        }

        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            if (!groupCore[static_cast<std::size_t>(slot)]) continue;
            GroupCell& cell = groupCells[static_cast<std::size_t>(groupSlotCell[static_cast<std::size_t>(slot)])];
            groupNextCore[static_cast<std::size_t>(slot)] = cell.coreHead;
            cell.coreHead = slot;
        }

        for (int cellIndex = 0; cellIndex < groupCellCount; ++cellIndex) {
            GroupCell& cell = groupCells[static_cast<std::size_t>(cellIndex)];
            if (cell.coreHead < 0) continue;
            const float anchor = groupAngle[static_cast<std::size_t>(cell.coreHead)];
            float minimum = anchor;
            float maximum = anchor;
            for (int slot = groupNextCore[static_cast<std::size_t>(cell.coreHead)];
                 slot >= 0;
                 slot = groupNextCore[static_cast<std::size_t>(slot)]) {
                float angle = groupAngle[static_cast<std::size_t>(slot)];
                angle += Tau * std::floor((anchor - angle) / Tau + .5f);
                minimum = std::min(minimum, angle);
                maximum = std::max(maximum, angle);
            }
            cell.angleMin = minimum;
            cell.angleMax = maximum;
        }

        for (int cellIndex = 0; cellIndex < groupCellCount; ++cellIndex) {
            if (groupCells[static_cast<std::size_t>(cellIndex)].coreHead < 0) continue;
            const GroupCell& cell = groupCells[static_cast<std::size_t>(cellIndex)];
            const int sector = cell.sector;
            for (int dyOffset : CellOffsets) {
                const float gapY = static_cast<float>(std::max(0, std::abs(dyOffset) - 1)) * CellSize;
                for (int dxOffset : CellOffsets) {
                    const float gapX = static_cast<float>(std::max(0, std::abs(dxOffset) - 1)) * CellSize;
                    if (gapX * gapX + gapY * gapY > RadiusSquared) continue;
                    const int sectorCount = sector == StationarySector ? 1 : static_cast<int>(SectorOffsets.size());
                    for (int os = 0; os < sectorCount; ++os) {
                        const int candidateSector = sector == StationarySector
                            ? StationarySector
                            : (sector + SectorOffsets[static_cast<std::size_t>(os)] + SectorCount) & (SectorCount - 1);
                        const int otherIndex = findGroupCell(cell.x + dxOffset, cell.y + dyOffset, candidateSector);
                        if (otherIndex <= cellIndex || otherIndex < 0 ||
                            groupCells[static_cast<std::size_t>(otherIndex)].coreHead < 0) continue;
                        const int leftRoot = groupRoot(cellIndex);
                        const int rightRoot = groupRoot(otherIndex);
                        if (leftRoot == rightRoot) continue;
                        const GroupCell& left = groupCells[static_cast<std::size_t>(leftRoot)];
                        const GroupCell& right = groupCells[static_cast<std::size_t>(rightRoot)];
                        const float leftCenter = (left.angleMin + left.angleMax) * .5f;
                        const float rightCenter = (right.angleMin + right.angleMax) * .5f;
                        const float shift = Tau * std::floor((leftCenter - rightCenter) / Tau + .5f);
                        const float minimum = std::min(left.angleMin, right.angleMin + shift);
                        const float maximum = std::max(left.angleMax, right.angleMax + shift);
                        if (maximum - minimum > Pi * .5f + 1e-12f) continue;
                        bool connected = false;
                        for (int first = groupCells[static_cast<std::size_t>(cellIndex)].coreHead;
                             first >= 0 && !connected;
                             first = groupNextCore[static_cast<std::size_t>(first)]) {
                            for (int second = groupCells[static_cast<std::size_t>(otherIndex)].coreHead;
                                 second >= 0;
                                 second = groupNextCore[static_cast<std::size_t>(second)]) {
                                const Vec2 firstPosition = birdSlots[static_cast<std::size_t>(first)].position;
                                const Vec2 secondPosition = birdSlots[static_cast<std::size_t>(second)].position;
                                const float dx = firstPosition.x - secondPosition.x;
                                const float dy = firstPosition.y - secondPosition.y;
                                if (dx * dx + dy * dy <= RadiusSquared && aligned(first, second)) {
                                    connected = true;
                                    break;
                                }
                            }
                        }
                        if (connected) {
                            const int root = std::min(leftRoot, rightRoot);
                            const int child = std::max(leftRoot, rightRoot);
                            groupCells[static_cast<std::size_t>(child)].parent = root;
                            groupCells[static_cast<std::size_t>(root)].angleMin = minimum;
                            groupCells[static_cast<std::size_t>(root)].angleMax = maximum;
                        }
                    }
                }
            }
        }

        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            if (!isNeighborLive(birdSlots[static_cast<std::size_t>(slot)])) continue;
            if (groupCore[static_cast<std::size_t>(slot)]) {
                const int root = groupRoot(groupSlotCell[static_cast<std::size_t>(slot)]);
                groupMembership[static_cast<std::size_t>(slot)] = groupCells[static_cast<std::size_t>(root)].coreHead;
                continue;
            }
            const int sourceCell = groupSlotCell[static_cast<std::size_t>(slot)];
            const GroupCell& source = groupCells[static_cast<std::size_t>(sourceCell)];
            const int sector = source.sector;
            const Vec2 position = birdSlots[static_cast<std::size_t>(slot)].position;
            int nearest = -1;
            float nearestDistance = RadiusSquared;
            for (int oy : CellOffsets) {
                const int cy = source.y + oy;
                const float gapY = std::max({static_cast<float>(cy) * CellSize - position.y,
                                             position.y - (static_cast<float>(cy + 1) * CellSize), 0.f});
                for (int ox : CellOffsets) {
                    const int cx = source.x + ox;
                    const float gapX = std::max({static_cast<float>(cx) * CellSize - position.x,
                                                 position.x - (static_cast<float>(cx + 1) * CellSize), 0.f});
                    if (gapX * gapX + gapY * gapY > nearestDistance) continue;
                    const int sectorCount = sector == StationarySector ? 1 : static_cast<int>(SectorOffsets.size());
                    for (int os = 0; os < sectorCount; ++os) {
                        const int candidateSector = sector == StationarySector
                            ? StationarySector
                            : (sector + SectorOffsets[static_cast<std::size_t>(os)] + SectorCount) & (SectorCount - 1);
                        const int candidateCell = findGroupCell(cx, cy, candidateSector);
                        if (candidateCell < 0) continue;
                        for (int core = groupCells[static_cast<std::size_t>(candidateCell)].coreHead;
                             core >= 0;
                             core = groupNextCore[static_cast<std::size_t>(core)]) {
                            const Vec2 corePosition = birdSlots[static_cast<std::size_t>(core)].position;
                            const float dx = corePosition.x - position.x;
                            const float dy = corePosition.y - position.y;
                            const float distance = dx * dx + dy * dy;
                            if (distance > nearestDistance || !aligned(slot, core)) continue;
                            if (distance < nearestDistance || nearest < 0 ||
                                birdSlots[static_cast<std::size_t>(core)].id <
                                    birdSlots[static_cast<std::size_t>(nearest)].id) {
                                nearest = core;
                                nearestDistance = distance;
                            }
                        }
                    }
                }
            }
            if (nearest < 0) {
                groupMembership[static_cast<std::size_t>(slot)] = slot;
            } else {
                const int root = groupRoot(groupSlotCell[static_cast<std::size_t>(nearest)]);
                groupMembership[static_cast<std::size_t>(slot)] = groupCells[static_cast<std::size_t>(root)].coreHead;
            }
        }
    }

    void rebuildColors() {
        buildGroups();
        leaders.fill(-1);
        leaderScores.fill(-std::numeric_limits<float>::infinity());
        groupSize.fill(0);
        groupSumX.fill(0.f);
        groupSumY.fill(0.f);
        groupDirectionX.fill(0.f);
        groupDirectionY.fill(0.f);
        groupHeadingSlot.fill(-1);

        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            if (!isNeighborLive(birdSlots[static_cast<std::size_t>(slot)])) continue;
            const int group = groupMembership[static_cast<std::size_t>(slot)];
            if (group < 0) continue;
            ++groupSize[static_cast<std::size_t>(group)];
            groupSumX[static_cast<std::size_t>(group)] += birdSlots[static_cast<std::size_t>(slot)].position.x;
            groupSumY[static_cast<std::size_t>(group)] += birdSlots[static_cast<std::size_t>(slot)].position.y;
            const float speed = std::hypot(birdSlots[static_cast<std::size_t>(slot)].velocity.x,
                                           birdSlots[static_cast<std::size_t>(slot)].velocity.y);
            if (speed > 1e-6f) {
                groupDirectionX[static_cast<std::size_t>(group)] += birdSlots[static_cast<std::size_t>(slot)].velocity.x / speed;
                groupDirectionY[static_cast<std::size_t>(group)] += birdSlots[static_cast<std::size_t>(slot)].velocity.y / speed;
                const int first = groupHeadingSlot[static_cast<std::size_t>(group)];
                if (first < 0 || birdSlots[static_cast<std::size_t>(slot)].id <
                                     birdSlots[static_cast<std::size_t>(first)].id)
                    groupHeadingSlot[static_cast<std::size_t>(group)] = slot;
            }
        }

        for (int group = 0; group < MaxBirds; ++group) {
            const int size = groupSize[static_cast<std::size_t>(group)];
            if (size == 0) continue;
            groupSumX[static_cast<std::size_t>(group)] /= static_cast<float>(size);
            groupSumY[static_cast<std::size_t>(group)] /= static_cast<float>(size);
            const int first = groupHeadingSlot[static_cast<std::size_t>(group)];
            if (first >= 0 && std::hypot(groupDirectionX[static_cast<std::size_t>(group)],
                                        groupDirectionY[static_cast<std::size_t>(group)]) <
                                  static_cast<float>(size) * .15f) {
                const float speed = std::hypot(birdSlots[static_cast<std::size_t>(first)].velocity.x,
                                               birdSlots[static_cast<std::size_t>(first)].velocity.y);
                groupDirectionX[static_cast<std::size_t>(group)] = birdSlots[static_cast<std::size_t>(first)].velocity.x / speed;
                groupDirectionY[static_cast<std::size_t>(group)] = birdSlots[static_cast<std::size_t>(first)].velocity.y / speed;
            }
        }

        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            if (!isNeighborLive(birdSlots[static_cast<std::size_t>(slot)]) ||
                !groupCore[static_cast<std::size_t>(slot)]) continue;
            const int group = groupMembership[static_cast<std::size_t>(slot)];
            if (group < 0 || groupHeadingSlot[static_cast<std::size_t>(group)] < 0) continue;
            const float score = (birdSlots[static_cast<std::size_t>(slot)].position.x - groupSumX[static_cast<std::size_t>(group)]) *
                                    groupDirectionX[static_cast<std::size_t>(group)] +
                                (birdSlots[static_cast<std::size_t>(slot)].position.y - groupSumY[static_cast<std::size_t>(group)]) *
                                    groupDirectionY[static_cast<std::size_t>(group)];
            const std::size_t start = static_cast<std::size_t>(group * LeaderCount);
            for (int rank = 0; rank < LeaderCount; ++rank) {
                const std::size_t p = start + static_cast<std::size_t>(rank);
                const int other = leaders[p];
                if (score < leaderScores[p] ||
                    (score == leaderScores[p] && other >= 0 &&
                     birdSlots[static_cast<std::size_t>(slot)].id > birdSlots[static_cast<std::size_t>(other)].id))
                    continue;
                for (int end = LeaderCount - 1; end > rank; --end) {
                    leaders[start + static_cast<std::size_t>(end)] = leaders[start + static_cast<std::size_t>(end - 1)];
                    leaderScores[start + static_cast<std::size_t>(end)] = leaderScores[start + static_cast<std::size_t>(end - 1)];
                }
                leaders[p] = slot;
                leaderScores[p] = score;
                break;
            }
        }

        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            const float own = baseHue[static_cast<std::size_t>(slot)];
            targetHue[static_cast<std::size_t>(slot)] = own;
            if (!isNeighborLive(birdSlots[static_cast<std::size_t>(slot)])) continue;
            const int group = groupMembership[static_cast<std::size_t>(slot)];
            if (group < 0) continue;
            const std::size_t start = static_cast<std::size_t>(group * LeaderCount);
            float sine = 0.f;
            float cosine = 0.f;
            int first = -1;
            bool isLeader = false;
            for (int rank = 0; rank < LeaderCount; ++rank) {
                const int leader = leaders[start + static_cast<std::size_t>(rank)];
                if (leader == slot) {
                    isLeader = true;
                    break;
                }
                if (leader < 0) continue;
                if (first < 0) first = leader;
                const float angle = hue[static_cast<std::size_t>(leader)] * Tau;
                const float weight = static_cast<float>(LeaderCount - rank);
                sine += std::sin(angle) * weight;
                cosine += std::cos(angle) * weight;
            }
            if (isLeader || first < 0) continue;
            const float leaderHue = std::hypot(sine, cosine) > 1e-5f
                                        ? std::atan2(sine, cosine) / Tau
                                        : hue[static_cast<std::size_t>(first)];
            targetHue[static_cast<std::size_t>(slot)] = own + wrappedDifference(leaderHue, own) * .65f;
        }
    }

    void stepColors(float dt) {
        if (simulationTime >= nextColorUpdate) {
            rebuildColors();
            nextColorUpdate = simulationTime + .1;
        }
        const float blend = 1.f - std::exp(-dt * .8f);
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            previousHue[static_cast<std::size_t>(slot)] = hue[static_cast<std::size_t>(slot)];
            Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            if (bird.phase == Phase::Dying) continue;
            float next = hue[static_cast<std::size_t>(slot)] +
                         wrappedDifference(targetHue[static_cast<std::size_t>(slot)],
                                           hue[static_cast<std::size_t>(slot)]) * blend;
            next = normalizeHue(next);
            hue[static_cast<std::size_t>(slot)] = next;
            bird.color = brightHue(next);
        }
    }

    void retireHistory(Bird& bird, float retiredAt) {
        if (bird.historyCount <= 0 || bird.historyHead < 0) return;
        int destination = ghostCursor;
        bool found = false;
        for (int attempt = 0; attempt < GhostCapacity; ++attempt) {
            Ghost& candidate = ghostSlots[static_cast<std::size_t>(destination)];
            const float newest = pointTime(candidate.history, candidate.historyHead, candidate.historyCount);
            if (candidate.historyCount <= 0 || retiredAt - newest >= HistoryLifetime) {
                found = true;
                break;
            }
            destination = (destination + 1) % GhostCapacity;
        }
        if (!found) {
            destination = ghostCursor;
        }
        Ghost& ghost = ghostSlots[static_cast<std::size_t>(destination)];
        ghost.history = bird.history;
        ghost.historyHead = bird.historyHead;
        ghost.historyCount = bird.historyCount;
        ghost.retiredAt = retiredAt;
        ghostCursor = (destination + 1) % GhostCapacity;
        bird.historyHead = -1;
        bird.historyCount = 0;
        bird.lastSample = -100.f;
    }

    void expireGhosts(float now) {
        for (Ghost& ghost : ghostSlots) {
            if (ghost.historyCount <= 0) continue;
            const float newest = pointTime(ghost.history, ghost.historyHead, ghost.historyCount);
            if (now - newest >= HistoryLifetime) {
                ghost.historyCount = 0;
                ghost.historyHead = -1;
                ghost.retiredAt = -1.f;
            }
        }
    }

    void appendPeriodicSamples(double sampleTime, double startClock, float dt) {
        const float fractionBase = static_cast<float>(
            std::clamp((sampleTime - startClock) / static_cast<double>(dt), 0.0, 1.0));
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            Motion& next = motion[static_cast<std::size_t>(slot)];
            if (next.phase == Phase::Dying && !next.deathContact) continue;
            float fraction = fractionBase;
            if (next.deathContact) {
                if (sampleTime > next.contactTime + 1e-7) continue;
                const double duration = next.contactTime - startClock;
                fraction = duration > 1e-9
                               ? static_cast<float>(std::clamp(
                                     (sampleTime - startClock) / duration, 0.0, 1.0))
                               : 1.f;
            }
            if (sampleTime < startClock - 1e-7 ||
                sampleTime > next.moveEndTime + 1e-7) continue;
            Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            const float h = normalizeHue(next.oldHue + wrappedDifference(next.newHue, next.oldHue) * fraction);
            const Vec2 position{next.oldX + (next.newX - next.oldX) * fraction,
                                next.oldY + (next.newY - next.oldY) * fraction};
            appendTrail(bird, position, static_cast<float>(sampleTime), brightHue(h));
        }
    }

    void recordHistories(double startClock, float dt, double endClock) {
        double sampleTime;
        if (lastHistoryTime < 0.0) sampleTime = endClock;
        else sampleTime = lastHistoryTime + HistoryInterval;
        while (sampleTime <= endClock + 1e-7) {
            appendPeriodicSamples(sampleTime, startClock, dt);
            lastHistoryTime = sampleTime;
            sampleTime += HistoryInterval;
        }
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            Motion& next = motion[static_cast<std::size_t>(slot)];
            if (!next.deathContact) continue;
            Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            const double duration = next.contactTime - startClock;
            const float fraction = duration > 1e-9
                                       ? static_cast<float>(std::clamp(
                                             duration / static_cast<double>(dt), 0.0, 1.0))
                                       : 0.f;
            const float contactHue = normalizeHue(next.oldHue +
                wrappedDifference(next.newHue, next.oldHue) * fraction);
            appendTrail(bird, next.contact, static_cast<float>(next.contactTime),
                        brightHue(contactHue));
            retireHistory(bird, static_cast<float>(next.contactTime));
        }
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            if (!motion[static_cast<std::size_t>(slot)].retire ||
                motion[static_cast<std::size_t>(slot)].deathContact) continue;
            retireHistory(birdSlots[static_cast<std::size_t>(slot)],
                          static_cast<float>(endClock));
        }
    }

    void removeSlotAt(int activeIndex) {
        const int slot = activeSlots[static_cast<std::size_t>(activeIndex)];
        Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
        bird.phase = Phase::Dead;
        bird.phaseTime = 0.f;
        bird.historyHead = -1;
        bird.historyCount = 0;
        bird.lastSample = -100.f;
        activeSlots[static_cast<std::size_t>(activeIndex)] = activeSlots[static_cast<std::size_t>(--activeCount)];
    }

    void computeBirdMotion(int slot, const MotionParams& params,
                           std::uint64_t& localCandidates) {
        const Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
        Motion& next = motion[static_cast<std::size_t>(slot)];
        next = {};
        next.oldX = bird.position.x;
        next.oldY = bird.position.y;
        next.oldHue = previousHue[static_cast<std::size_t>(slot)];
        next.newHue = hue[static_cast<std::size_t>(slot)];
        next.newX = bird.position.x;
        next.newY = bird.position.y;
        next.newVelocity = bird.velocity;
        next.phase = bird.phase;
        next.phaseTime = bird.phaseTime;
        next.boost = boosts[static_cast<std::size_t>(slot)];
        next.moveEndTime = params.endClock;
        if (bird.phase == Phase::Dying) {
            next.phaseTime = bird.phaseTime + params.dt;
            return;
        }

        const float ux = unitX[static_cast<std::size_t>(slot)];
        const float uy = unitY[static_cast<std::size_t>(slot)];
        float forceX = 0.f;
        float forceY = 0.f;
        if (bird.phase == Phase::Leaving) {
            forceX = (exitTargets[static_cast<std::size_t>(slot)].x - bird.position.x) * .06f;
            forceY = (exitTargets[static_cast<std::size_t>(slot)].y - bird.position.y) * .06f;
        } else {
            float neighborCount = 0.f;
            float sumX = 0.f;
            float sumY = 0.f;
            float directionX = 0.f;
            float directionY = 0.f;
            float separationX = 0.f;
            float separationY = 0.f;
            aggregateForces(slot, neighborCount, sumX, sumY, directionX, directionY,
                            separationX, separationY, localCandidates);
            if (neighborCount > 0.f) {
                if (settings.cohesion != 0.f) {
                    forceX += settings.cohesion * (sumX / neighborCount) / Radius;
                    forceY += settings.cohesion * (sumY / neighborCount) / Radius;
                }
                if (settings.alignment != 0.f) {
                    forceX += settings.alignment * (directionX / neighborCount - ux);
                    forceY += settings.alignment * (directionY / neighborCount - uy);
                }
            }
            if (settings.separation != 0.f) {
                forceX += separationX * settings.separation * 2.f;
                forceY += separationY * settings.separation * 2.f;
            }
            float pushX = edgeForce(bird.position.x - bounds.left, params.bandX) -
                          edgeForce(bounds.right - bird.position.x, params.bandX);
            float pushY = edgeForce(bird.position.y - bounds.top, params.bandY) -
                          edgeForce(bounds.bottom - bird.position.y, params.bandY);
            if (params.exclusionBand > 0.f) {
                for (const Bounds& rectangle : exclusions) {
                    const Vec2 push = exclusionRepulsion(
                        bird.position.x, bird.position.y, ux, uy, rectangle,
                        params.exclusionBand);
                    pushX += push.x;
                    pushY += push.y;
                }
            }
            forceX += 6.f * pushX;
            forceY += 6.f * pushY;
            const float urgency = std::max(0.f, -ux * pushX - uy * pushY);
            const float turn = (bird.id & 1u) ? 1.f : -1.f;
            forceX += -uy * urgency * turn * 3.f;
            forceY += ux * urgency * turn * 3.f;
            if (bird.phase == Phase::Entering) {
                forceX += ((bounds.left + params.width * .5f - bird.position.x) / params.width) * 2.2f;
                forceY += ((bounds.top + params.height * .5f - bird.position.y) / params.height) * 2.2f;
            }
        }

        if (params.input.pointerActive) {
            const float dx = bird.position.x - params.input.pointer.x;
            const float dy = bird.position.y - params.input.pointer.y;
            const float distance = std::hypot(dx, dy);
            if (distance < 100.f && distance > .001f) {
                forceX += (dx / distance) * (1.f - distance / 100.f) * 2.f;
                forceY += (dy / distance) * (1.f - distance / 100.f) * 2.f;
            }
        }
        if (pulseUntil > params.endTime) {
            const float dx = bird.position.x - pulseX;
            const float dy = bird.position.y - pulseY;
            const float distance = std::hypot(dx, dy);
            if (distance < 180.f && distance > .001f) {
                const float strength = ((pulseUntil - params.endTime) / 1.2f) *
                                       (1.f - distance / 180.f) * 8.f;
                forceX += (dx / distance) * strength;
                forceY += (dy / distance) * strength;
            }
        }

        const float desiredAngle = std::atan2(uy + forceY * 2.8f * params.dt,
                                              ux + forceX * 2.8f * params.dt);
        const float angle = std::atan2(uy, ux);
        float delta = desiredAngle - angle;
        while (delta > Pi) delta -= Tau;
        while (delta < -Pi) delta += Tau;
        const float maxTurn = 2.4f * params.dt;
        delta = clampFloat(delta, -maxTurn, maxTurn);
        const float boostTarget = bird.boostUntil > params.endTime ? .1f : 0.f;
        next.boost = boosts[static_cast<std::size_t>(slot)] +
                     (boostTarget - boosts[static_cast<std::size_t>(slot)]) *
                         std::min(1.f, params.dt * 6.f);
        const float speed = settings.speed * (1.f + bird.speedBias * settings.speedError) *
                            (1.f + next.boost);
        const float nextAngle = angle + delta;
        next.newVelocity = {std::cos(nextAngle) * speed, std::sin(nextAngle) * speed};
        next.newX = bird.position.x + next.newVelocity.x * params.dt;
        next.newY = bird.position.y + next.newVelocity.y * params.dt;
        next.phaseTime = bird.phaseTime + params.dt;
        if (bird.phase == Phase::Entering &&
            bird.position.x > bounds.left + 2.f && bird.position.x < bounds.right - 2.f &&
            bird.position.y > bounds.top + 2.f && bird.position.y < bounds.bottom - 2.f &&
            !inExclusion(bird.position.x, bird.position.y)) {
            next.phase = Phase::Active;
            next.phaseTime = 0.f;
        }
        if (bird.phase == Phase::Active || next.phase == Phase::Active) {
            float fraction = -1.f;
            float contactX = 0.f;
            float contactY = 0.f;
            const float x = bird.position.x;
            const float y = bird.position.y;
            const float nx = next.newX;
            const float ny = next.newY;
            if (x <= bounds.left || x >= bounds.right || y <= bounds.top || y >= bounds.bottom) {
                fraction = 0.f;
                contactX = clampFloat(x, bounds.left, bounds.right);
                contactY = clampFloat(y, bounds.top, bounds.bottom);
            } else {
                if (nx <= bounds.left && nx != x)
                    fraction = std::min(fraction < 0.f ? 1.f : fraction,
                                        (bounds.left - x) / (nx - x));
                else if (nx >= bounds.right && nx != x)
                    fraction = std::min(fraction < 0.f ? 1.f : fraction,
                                        (bounds.right - x) / (nx - x));
                if (ny <= bounds.top && ny != y)
                    fraction = std::min(fraction < 0.f ? 1.f : fraction,
                                        (bounds.top - y) / (ny - y));
                else if (ny >= bounds.bottom && ny != y)
                    fraction = std::min(fraction < 0.f ? 1.f : fraction,
                                        (bounds.bottom - y) / (ny - y));
                if (fraction > 1.f) fraction = -1.f;
                if (fraction >= 0.f) {
                    contactX = clampFloat(x + (nx - x) * fraction,
                                          bounds.left, bounds.right);
                    contactY = clampFloat(y + (ny - y) * fraction,
                                          bounds.top, bounds.bottom);
                }
            }
            for (const Bounds& rectangle : exclusions) {
                const float exclusionFraction = segmentRectangleEntry(
                    x, y, nx, ny, rectangle);
                if (exclusionFraction >= 0.f &&
                    (fraction < 0.f || exclusionFraction < fraction)) {
                    fraction = exclusionFraction;
                    contactX = x + (nx - x) * fraction;
                    contactY = y + (ny - y) * fraction;
                }
            }
            if (fraction >= 0.f) {
                next.deathContact = true;
                next.contactTime = params.startClock + static_cast<double>(fraction) *
                                                   static_cast<double>(params.dt);
                next.contact = {contactX, contactY};
                next.moveEndTime = next.contactTime;
                next.newX = contactX;
                next.newY = contactY;
                next.phase = Phase::Dying;
                next.phaseTime = static_cast<float>(
                    std::max(0.0, params.endClock - next.contactTime));
            }
        }
        next.retire = next.phase == Phase::Leaving &&
                      (next.newX < bounds.left - 50.f || next.newX > bounds.right + 50.f ||
                       next.newY < bounds.top - 50.f || next.newY > bounds.bottom + 50.f);
        if (next.retire) next.moveEndTime = params.endClock;
    }

    void stopWorkers() {
        {
            std::lock_guard<std::mutex> lock(poolMutex);
            poolStop = true;
            cvStart.notify_all();
        }
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
        workersStarted = false;
    }

    void ensureWorkersStarted() {
        if (workersStarted || configuredWorkers <= 1) {
            return;
        }
        poolStop = false;
        try {
            workers.reserve(configuredWorkers - 1);
            for (unsigned i = 1; i < configuredWorkers; ++i) {
                workers.emplace_back([this, i]() {
                    workerLoop(i);
                });
            }
            workersStarted = true;
        } catch (...) {
            stopWorkers();
            throw;
        }
    }

    void workerLoop(unsigned workerIndex) {
        std::uint64_t seenEpoch = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(poolMutex);
            cvStart.wait(lock, [this, seenEpoch]() {
                return poolStop || currentWorkEpoch > seenEpoch;
            });
            if (poolStop) {
                return;
            }
            seenEpoch = currentWorkEpoch;
            lock.unlock();

            WorkerContext& ctx = workerContexts[workerIndex];
            ctx.candidateCount = 0;
            const MotionParams* params = activeMotionParams;
            const int totalChunks = totalChunkCount;
            const int totalActive = activeCount;

            while (true) {
                const int chunk = nextChunkIndex.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= totalChunks) {
                    break;
                }
                const int start = chunk * MotionChunkSize;
                const int end = std::min(start + MotionChunkSize, totalActive);
                for (int a = start; a < end; ++a) {
                    const int slot = activeSlots[static_cast<std::size_t>(a)];
                    computeBirdMotion(slot, *params, ctx.candidateCount);
                }
            }

            lock.lock();
            --runningWorkers;
            if (runningWorkers == 0) {
                cvDone.notify_one();
            }
        }
    }

    void dispatchParallelMotion(const MotionParams& params) {
        ensureWorkersStarted();

        const int totalChunks = (activeCount + MotionChunkSize - 1) / MotionChunkSize;
        activeMotionParams = &params;
        totalChunkCount = totalChunks;
        nextChunkIndex.store(0, std::memory_order_relaxed);
        workerContexts[0].candidateCount = 0;

        const unsigned activeWorkers = configuredWorkers - 1;
        {
            std::lock_guard<std::mutex> lock(poolMutex);
            runningWorkers = static_cast<int>(activeWorkers);
            ++currentWorkEpoch;
            cvStart.notify_all();
        }

        WorkerContext& callerCtx = workerContexts[0];
        while (true) {
            const int chunk = nextChunkIndex.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= totalChunks) {
                break;
            }
            const int start = chunk * MotionChunkSize;
            const int end = std::min(start + MotionChunkSize, activeCount);
            for (int a = start; a < end; ++a) {
                const int slot = activeSlots[static_cast<std::size_t>(a)];
                computeBirdMotion(slot, params, callerCtx.candidateCount);
            }
        }

        {
            std::unique_lock<std::mutex> lock(poolMutex);
            cvDone.wait(lock, [this]() {
                return runningWorkers == 0;
            });
        }

        for (unsigned i = 0; i < configuredWorkers; ++i) {
            currentStats.candidates += workerContexts[i].candidateCount;
        }
    }

    void initializeMotion(float dt, const Input& input, double startClock, double endClock) {
        const float startTime = static_cast<float>(startClock);
        const float endTime = static_cast<float>(endClock);
        const float width = bounds.right - bounds.left;
        const float height = bounds.bottom - bounds.top;
        const float turnRoom = settings.speed * (1.f + settings.speedError) / 2.4f * 2.f;
        const float bandX = settings.edge > 0.f ? std::max(width * settings.edge, turnRoom) : 0.f;
        const float bandY = settings.edge > 0.f ? std::max(height * settings.edge, turnRoom) : 0.f;
        const float exclusionBand = settings.edge > 0.f
                                        ? std::max(std::min(width, height) * settings.edge,
                                                   turnRoom)
                                        : 0.f;
        if (input.click) {
            pulseX = input.pointer.x;
            pulseY = input.pointer.y;
            pulseUntil = startTime + 1.2f;
            for (int a = 0; a < activeCount; ++a) {
                const int slot = activeSlots[static_cast<std::size_t>(a)];
                const Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
                const float dx = bird.position.x - input.pointer.x;
                const float dy = bird.position.y - input.pointer.y;
                if (dx * dx + dy * dy < 180.f * 180.f)
                    birdSlots[static_cast<std::size_t>(slot)].boostUntil = startTime + 2.f;
            }
        }

        MotionParams params{
            .dt = dt,
            .input = input,
            .startClock = startClock,
            .endClock = endClock,
            .endTime = endTime,
            .width = width,
            .height = height,
            .bandX = bandX,
            .bandY = bandY,
            .exclusionBand = exclusionBand
        };

        if (activeCount < MinParallelActiveCount || configuredWorkers <= 1) {
            std::uint64_t serialCandidates = 0;
            for (int a = 0; a < activeCount; ++a) {
                const int slot = activeSlots[static_cast<std::size_t>(a)];
                computeBirdMotion(slot, params, serialCandidates);
            }
            currentStats.candidates += serialCandidates;
        } else {
            dispatchParallelMotion(params);
        }
    }

    void commitMotion(float dt, double startClock, double endClock) {
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            Motion& next = motion[static_cast<std::size_t>(slot)];
            if (bird.phase == Phase::Dying) {
                bird.phaseTime = next.phaseTime;
                continue;
            }
            bird.position = {next.newX, next.newY};
            bird.velocity = next.newVelocity;
            bird.phase = next.phase;
            bird.phaseTime = next.phaseTime;
            boosts[static_cast<std::size_t>(slot)] = next.boost;
        }
        recordHistories(startClock, dt, endClock);
        for (int a = activeCount - 1; a >= 0; --a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            Motion& next = motion[static_cast<std::size_t>(slot)];
            const bool expiredDeath = bird.phase == Phase::Dying &&
                                      bird.phaseTime + 1e-6f >= DeathDuration;
            if (expiredDeath) retireHistory(bird, static_cast<float>(endClock));
            if (next.retire || expiredDeath) removeSlotAt(a);
        }
        expireGhosts(static_cast<float>(endClock));
    }

    void recountStats() {
        currentStats.active = 0;
        currentStats.entering = 0;
        currentStats.leaving = 0;
        currentStats.dying = 0;
        for (int a = 0; a < activeCount; ++a) {
            const Phase phase = birdSlots[static_cast<std::size_t>(activeSlots[static_cast<std::size_t>(a)])].phase;
            switch (phase) {
                case Phase::Active: ++currentStats.active; break;
                case Phase::Entering: ++currentStats.entering; break;
                case Phase::Leaving: ++currentStats.leaving; break;
                case Phase::Dying: ++currentStats.dying; break;
                case Phase::Dead: break;
            }
        }
    }

    void configure(const Settings& newSettings, Bounds newBounds,
                   std::span<const Bounds> newExclusions) {
        settings = sanitizeSettings(newSettings);
        bounds = sanitizeBounds(newBounds, bounds);
        exclusions.clear();
        for (const Bounds& rectangle : newExclusions) {
            if (!std::isfinite(rectangle.left) || !std::isfinite(rectangle.top) ||
                !std::isfinite(rectangle.right) || !std::isfinite(rectangle.bottom) ||
                rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) {
                continue;
            }
            const Bounds clipped{
                std::max(rectangle.left, bounds.left),
                std::max(rectangle.top, bounds.top),
                std::min(rectangle.right, bounds.right),
                std::min(rectangle.bottom, bounds.bottom),
            };
            if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) continue;
            exclusions.push_back(clipped);
        }
        for (int a = 0; a < activeCount; ++a) {
            const int slot = activeSlots[static_cast<std::size_t>(a)];
            Bird& bird = birdSlots[static_cast<std::size_t>(slot)];
            if (bird.phase == Phase::Active &&
                (bird.position.x <= bounds.left || bird.position.x >= bounds.right ||
                 bird.position.y <= bounds.top || bird.position.y >= bounds.bottom ||
                 inExclusion(bird.position.x, bird.position.y))) {
                bird.phase = Phase::Entering;
                bird.phaseTime = 0.f;
            } else if (bird.phase == Phase::Leaving) {
                setLeaveTarget(slot);
            }
        }
        buildNeighborGrid();
    }

    void reset(std::uint32_t seed) {
        initialSeed = seed;
        randomState = seed;
        nextId = 1;
        boosts.fill(0.f);
        exitTargets.fill(Vec2{});
        simulationTime = 0.f;
        nextColorUpdate = 0.f;
        lastHistoryTime = -1.f;
        spawnCredit = 0.f;
        pulseX = pulseY = 0.f;
        pulseUntil = -1.f;
        ghostCursor = 0;
        activeCount = 0;
        currentStats = {};
        for (Bird& bird : birdSlots) {
            bird = Bird{};
            bird.phase = Phase::Dead;
            bird.historyHead = -1;
            bird.lastSample = -100.f;
        }
        for (Ghost& ghost : ghostSlots) {
            ghost = Ghost{};
            ghost.historyHead = -1;
            ghost.retiredAt = -1.f;
        }
        baseHue.fill(0.f);
        hue.fill(0.f);
        previousHue.fill(0.f);
        targetHue.fill(0.f);
        neighborBuckets.fill(-1);
        groupBuckets.fill(-1);
        neighborCellCount = 0;
        groupCellCount = 0;
        neighborGridDirty = true;
        posX.fill(0.f);
        posY.fill(0.f);
    }

    void restore(std::span<const Bird> source, float restoredTime) {
        const bool aliasesInternal = !source.empty() && source.data() == birdSlots.data();
        const std::size_t inputCount = std::min<std::size_t>(source.size(), MaxBirds);
        if (!aliasesInternal) {
            for (Bird& bird : birdSlots) {
                bird = Bird{};
                bird.phase = Phase::Dead;
                bird.historyHead = -1;
                bird.lastSample = -100.f;
            }
        }
        activeCount = 0;
        nextId = 1;
        randomState = initialSeed;
        simulationTime = std::isfinite(restoredTime) ? static_cast<double>(std::max(0.f, restoredTime)) : 0.0;
        lastHistoryTime = -1.f;
        nextColorUpdate = simulationTime;
        spawnCredit = 0.f;
        pulseUntil = -1.f;
        for (Ghost& ghost : ghostSlots) {
            ghost = Ghost{};
            ghost.historyHead = -1;
            ghost.retiredAt = -1.f;
        }
        ghostCursor = 0;
        std::size_t destination = 0;
        for (std::size_t i = 0; i < inputCount; ++i) {
            Bird incoming = source[i];
            if (!validPhase(incoming.phase)) continue;
            if (aliasesInternal && destination != i) birdSlots[destination] = incoming;
            else if (!aliasesInternal) birdSlots[destination] = incoming;
            Bird& bird = birdSlots[destination];
            if (bird.historyCount == 0 || bird.historyHead < 0 || bird.historyHead >= HistoryCapacity) {
                bird.historyCount = 0;
                bird.historyHead = -1;
                bird.lastSample = -100.f;
            } else {
                const int oldest = (bird.historyHead - bird.historyCount + 1 + HistoryCapacity * 2) %
                                   HistoryCapacity;
                float previousTime = -std::numeric_limits<float>::infinity();
                bool validHistory = true;
                for (int offset = 0; offset < bird.historyCount; ++offset) {
                    const TrailPoint& point = bird.history[static_cast<std::size_t>(
                        (oldest + offset) % HistoryCapacity)];
                    if (!std::isfinite(point.time) || point.time < previousTime ||
                        !std::isfinite(point.position.x) || !std::isfinite(point.position.y) ||
                        !std::isfinite(point.color.r) || !std::isfinite(point.color.g) ||
                        !std::isfinite(point.color.b)) {
                        validHistory = false;
                        break;
                    }
                    previousTime = point.time;
                }
                if (!validHistory) {
                    bird.historyCount = 0;
                    bird.historyHead = -1;
                    bird.lastSample = -100.f;
                } else {
                    bird.lastSample = pointTime(bird.history, bird.historyHead, bird.historyCount);
                }
            }
            bird.phaseTime = std::isfinite(bird.phaseTime) ? std::max(0.f, bird.phaseTime) : 0.f;
            bird.speedBias = std::isfinite(bird.speedBias)
                                 ? clampFloat(bird.speedBias, -1.f, 1.f)
                                 : 0.f;
            bird.boostUntil = std::isfinite(bird.boostUntil) ? bird.boostUntil : 0.f;
            boosts[destination] = 0.f;
            if (!std::isfinite(bird.position.x) || !std::isfinite(bird.position.y) ||
                !std::isfinite(bird.velocity.x) || !std::isfinite(bird.velocity.y)) {
                bird.phase = Phase::Dead;
                bird.historyCount = 0;
                bird.historyHead = -1;
                continue;
            }
            if (bird.phase == Phase::Active &&
                (bird.position.x <= bounds.left || bird.position.x >= bounds.right ||
                 bird.position.y <= bounds.top || bird.position.y >= bounds.bottom ||
                 inExclusion(bird.position.x, bird.position.y))) {
                bird.phase = Phase::Entering;
                bird.phaseTime = 0.f;
            }
            const Color defaultColor = Settings{}.color;
            bird.baseColor.r = std::isfinite(bird.baseColor.r) ? clampFloat(bird.baseColor.r, 0.f, 1.f) : defaultColor.r;
            bird.baseColor.g = std::isfinite(bird.baseColor.g) ? clampFloat(bird.baseColor.g, 0.f, 1.f) : defaultColor.g;
            bird.baseColor.b = std::isfinite(bird.baseColor.b) ? clampFloat(bird.baseColor.b, 0.f, 1.f) : defaultColor.b;
            bird.color.r = std::isfinite(bird.color.r) ? clampFloat(bird.color.r, 0.f, 1.f) : bird.baseColor.r;
            bird.color.g = std::isfinite(bird.color.g) ? clampFloat(bird.color.g, 0.f, 1.f) : bird.baseColor.g;
            bird.color.b = std::isfinite(bird.color.b) ? clampFloat(bird.color.b, 0.f, 1.f) : bird.baseColor.b;
            if (!std::isfinite(bird.wingPhase))
                bird.wingPhase = static_cast<float>((static_cast<std::uint64_t>(bird.id) * 2654435761u) & 0xffffu) *
                                 (Tau / 65536.f);
            if (bird.phase == Phase::Leaving)
                setLeaveTarget(static_cast<int>(destination));
            activeSlots[static_cast<std::size_t>(activeCount++)] = static_cast<int>(destination);
            if (bird.id >= nextId) nextId = bird.id + 1;
            const std::size_t slot = destination;
            baseHue[slot] = hueFromColor(bird.baseColor);
            hue[slot] = hueFromColor(bird.color);
            previousHue[slot] = hue[slot];
            targetHue[slot] = baseHue[slot];
            ++destination;
        }
        for (std::size_t i = destination; i < MaxBirds; ++i) {
            birdSlots[i] = Bird{};
            birdSlots[i].phase = Phase::Dead;
            birdSlots[i].historyHead = -1;
            birdSlots[i].lastSample = -100.f;
        }
        if (nextId == 0) nextId = 1;
        lastHistoryTime = simulationTime;
        currentStats.active = currentStats.entering = currentStats.leaving = currentStats.dying = 0;
        recountStats();
        rebaseClock();
        buildNeighborGrid();
    }
    void step(float dt, const Input& input, bool paused) {
        if (paused || !std::isfinite(dt) || dt <= 0.f) return;
        const auto started = std::chrono::steady_clock::now();
        rebaseClock();
        const double startClock = simulationTime;
        const double endClock = startClock + static_cast<double>(dt);
        if (!std::isfinite(endClock)) return;
        simulationTime = endClock;
        currentStats = {};
        const double historyTimeBefore = lastHistoryTime;
        reconcile(dt);
        stepColors(dt);
        buildNeighborGrid();
        initializeMotion(dt, input, startClock, endClock);
        commitMotion(dt, startClock, endClock);
        const int activeBeforeReplenish = activeCount;
        replenish();
        if (lastHistoryTime > historyTimeBefore) {
            for (int a = activeBeforeReplenish; a < activeCount; ++a) {
                Bird& bird = birdSlots[static_cast<std::size_t>(activeSlots[static_cast<std::size_t>(a)])];
                appendTrail(bird, bird.position, static_cast<float>(lastHistoryTime), bird.color);
            }
        }
        neighborGridDirty = true;
        rebaseClock();
        recountStats();
        const auto ended = std::chrono::steady_clock::now();
        currentStats.simulationMs = std::chrono::duration<float, std::milli>(ended - started).count();
    }
};

Simulation::Simulation(std::uint32_t seed, unsigned workers)
    : impl_(std::make_unique<Impl>(seed, workers)) {}
Simulation::~Simulation() = default;

unsigned Simulation::workerCount() const noexcept {
    return impl_->configuredWorkers;
}

void Simulation::configure(const Settings& settings, Bounds bounds,
                           std::span<const Bounds> exclusions) {
    impl_->configure(settings, bounds, exclusions);
}

void Simulation::step(float dt, const Input& input, bool paused) {
    impl_->step(dt, input, paused);
}

void Simulation::reset(std::uint32_t seed) {
    impl_->reset(seed);
}

std::span<const Bird> Simulation::birds() const {
    return {impl_->birdSlots.data(), impl_->birdSlots.size()};
}

std::span<const Ghost> Simulation::ghosts() const {
    return {impl_->ghostSlots.data(), impl_->ghostSlots.size()};
}

const Settings& Simulation::settings() const {
    return impl_->settings;
}

Bounds Simulation::bounds() const {
    return impl_->bounds;
}

float Simulation::time() const {
    return static_cast<float>(impl_->simulationTime);
}

SimStats Simulation::stats() const {
    return impl_->currentStats;
}

void Simulation::restore(std::span<const Bird> birds, float time) {
    impl_->restore(birds, time);
}

int Simulation::neighbors(std::size_t index, std::span<int> output) const {
    if (index >= static_cast<std::size_t>(MaxBirds)) return 0;
    return impl_->queryNeighbors(static_cast<int>(index), output);
}

} // namespace flock
