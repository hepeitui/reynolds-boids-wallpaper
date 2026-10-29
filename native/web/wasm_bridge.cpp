#include "../simulation.hpp"
#include "../model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

constexpr float Pi = 3.14159265358979323846f;

struct BirdVertex {
    float x, y, luminance, wing;
};

// Faceted bird silhouette matching renderer.cpp
constexpr std::array<BirdVertex, 30> BirdMesh{{
    {-0.34f, -0.035f, 0.40f, 0.0f}, {-0.88f, -0.22f, 0.28f, 0.0f}, {-0.58f, -0.01f, 0.36f, 0.0f},
    {-0.34f,  0.035f, 0.40f, 0.0f}, {-0.58f,  0.01f, 0.36f, 0.0f}, {-0.88f,  0.22f, 0.28f, 0.0f},
    { 0.12f, -0.045f, 0.66f, 1.0f}, {-0.38f, -0.96f, 0.59f, 1.0f}, {-0.62f, -0.52f, 0.56f, 1.0f},
    { 0.12f, -0.045f, 0.66f, 1.0f}, {-0.62f, -0.52f, 0.56f, 1.0f}, {-0.32f, -0.16f, 0.63f, 1.0f},
    { 0.12f,  0.045f, 0.66f, 1.0f}, {-0.62f,  0.52f, 0.56f, 1.0f}, {-0.38f,  0.96f, 0.59f, 1.0f},
    { 0.12f,  0.045f, 0.66f, 1.0f}, {-0.32f,  0.16f, 0.63f, 1.0f}, {-0.62f,  0.52f, 0.56f, 1.0f},
    {-0.42f, -0.06f, 0.86f, 0.0f}, { 0.38f, -0.025f, 0.98f, 0.0f}, {-0.15f, 0.0f, 0.91f, 0.0f},
    {-0.15f,  0.0f, 0.91f, 0.0f}, { 0.38f, -0.025f, 0.98f, 0.0f}, { 0.64f, 0.0f, 1.0f, 0.0f},
    {-0.42f,  0.06f, 0.86f, 0.0f}, {-0.15f,  0.0f, 0.91f, 0.0f}, { 0.38f, 0.025f, 0.98f, 0.0f},
    {-0.15f,  0.0f, 0.91f, 0.0f}, { 0.38f,  0.025f, 0.98f, 0.0f}, { 0.64f, 0.0f, 1.0f, 0.0f}
}};

constexpr float TrailCorners[12] = {
    0.0f, -1.0f,
    0.0f,  1.0f,
    1.0f, -1.0f,
    1.0f, -1.0f,
    0.0f,  1.0f,
    1.0f,  1.0f
};

struct BirdInstance {
    float centerX, centerY;
    float velocityX, velocityY;
    float phase;
    float deathProgress;
    float alpha;
    float colorR, colorG, colorB;
};

struct TrailInstance {
    float startX, startY, startTime;
    float startR, startG, startB;
    float endX, endY, endTime;
    float endR, endG, endB;
};

inline float bounded(float value, float low, float high, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

inline bool finiteColor(flock::Color color) noexcept {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b);
}

inline flock::Color normalizedColor(flock::Color color, flock::Color fallback = {}) noexcept {
    if (!finiteColor(color)) color = fallback;
    return {
        std::clamp(color.r, 0.0f, 1.0f),
        std::clamp(color.g, 0.0f, 1.0f),
        std::clamp(color.b, 0.0f, 1.0f)
    };
}

inline flock::TrailPoint interpolateTrail(const flock::TrailPoint& a, const flock::TrailPoint& b, float time) noexcept {
    const float amount = b.time > a.time ? std::clamp((time - a.time) / (b.time - a.time), 0.0f, 1.0f) : 0.0f;
    flock::TrailPoint point;
    point.time = time;
    point.position.x = a.position.x + (b.position.x - a.position.x) * amount;
    point.position.y = a.position.y + (b.position.y - a.position.y) * amount;
    point.color = {
        a.color.r + (b.color.r - a.color.r) * amount,
        a.color.g + (b.color.g - a.color.g) * amount,
        a.color.b + (b.color.b - a.color.b) * amount
    };
    return point;
}

constexpr std::size_t MaxTrailSegments =
    (static_cast<std::size_t>(flock::MaxBirds) + static_cast<std::size_t>(flock::MaxGhosts)) *
    flock::HistoryCapacity;

std::unique_ptr<flock::Simulation> g_simulation;
std::vector<BirdInstance> g_birdInstances;
std::vector<TrailInstance> g_trailInstances;
int g_birdCount = 0;
int g_trailCount = 0;

bool appendSegment(const flock::TrailPoint& start, const flock::TrailPoint& end,
                   float renderNow, float pathEnd, float duration) {
    if (!(end.time > start.time)) return true;
    if (static_cast<std::size_t>(g_trailCount) >= g_trailInstances.size()) return false;
    const float cutoff = renderNow - duration;
    const float fromTime = std::max(start.time, cutoff);
    const float toTime = std::min(end.time, pathEnd);
    if (!(toTime > fromTime) || end.time <= cutoff || start.time >= pathEnd) return true;
    const flock::TrailPoint clippedStart = fromTime > start.time ? interpolateTrail(start, end, fromTime) : start;
    const flock::TrailPoint clippedEnd = toTime < end.time ? interpolateTrail(start, end, toTime) : end;
    TrailInstance& instance = g_trailInstances[static_cast<std::size_t>(g_trailCount++)];
    instance.startX = clippedStart.position.x;
    instance.startY = clippedStart.position.y;
    instance.startTime = clippedStart.time;
    instance.startR = clippedStart.color.r;
    instance.startG = clippedStart.color.g;
    instance.startB = clippedStart.color.b;
    instance.endX = clippedEnd.position.x;
    instance.endY = clippedEnd.position.y;
    instance.endTime = clippedEnd.time;
    instance.endR = clippedEnd.color.r;
    instance.endG = clippedEnd.color.g;
    instance.endB = clippedEnd.color.b;
    return true;
}

bool appendHistory(const std::array<flock::TrailPoint, flock::HistoryCapacity>& history, int head, int count,
                   float renderNow, float pathEnd, float duration, float sampleInterval,
                   const flock::TrailPoint* liveEndpoint) {
    count = std::clamp(count, 0, flock::HistoryCapacity);
    if (count < 2 && !liveEndpoint) return true;
    flock::TrailPoint previousRaw{};
    bool havePreviousRaw = false;
    flock::TrailPoint previousKept{};
    bool havePreviousKept = false;
    float nextSampleTime = -std::numeric_limits<float>::infinity();
    const float cutoff = renderNow - duration;
    bool overflow = false;

    auto acceptPoint = [&](flock::TrailPoint point, bool forceKeep) {
        if (!std::isfinite(point.time) || !std::isfinite(point.position.x) || !std::isfinite(point.position.y)) {
            havePreviousRaw = false;
            havePreviousKept = false;
            return;
        }
        if (!finiteColor(point.color)) point.color = {};
        if (havePreviousRaw && !(point.time > previousRaw.time)) {
            if (forceKeep && havePreviousKept && point.time > previousKept.time) {
                if (!appendSegment(previousKept, point, renderNow, pathEnd, duration)) {
                    overflow = true;
                    return;
                }
                previousKept = point;
                nextSampleTime = point.time + sampleInterval;
            }
            previousRaw = point;
            havePreviousRaw = true;
            return;
        }
        if (point.time > pathEnd) {
            if (havePreviousRaw && pathEnd > previousRaw.time && previousRaw.time < pathEnd)
                point = interpolateTrail(previousRaw, point, pathEnd);
            else
                return;
            forceKeep = true;
        }
        if (point.time < cutoff) {
            previousRaw = point;
            havePreviousRaw = true;
            return;
        }
        if (!havePreviousKept) {
            flock::TrailPoint first = point;
            if (havePreviousRaw && previousRaw.time < cutoff && point.time > cutoff)
                first = interpolateTrail(previousRaw, point, cutoff);
            previousKept = first;
            havePreviousKept = true;
            nextSampleTime = first.time + sampleInterval;
        } else if (forceKeep || point.time >= nextSampleTime) {
            if (!appendSegment(previousKept, point, renderNow, pathEnd, duration)) {
                overflow = true;
                return;
            }
            previousKept = point;
            nextSampleTime = point.time + sampleInterval;
        }
        previousRaw = point;
        havePreviousRaw = true;
    };

    if (count > 0) {
        const int normalizedHead = ((head % flock::HistoryCapacity) + flock::HistoryCapacity) % flock::HistoryCapacity;
        const int first = (normalizedHead - count + 1 + flock::HistoryCapacity * 2) % flock::HistoryCapacity;
        for (int sample = 0; sample < count && !overflow; ++sample) {
            const int index = (first + sample) % flock::HistoryCapacity;
            acceptPoint(history[static_cast<std::size_t>(index)], sample == count - 1 && !liveEndpoint);
            if (havePreviousRaw && previousRaw.time >= pathEnd) break;
        }
    }
    if (liveEndpoint && !overflow) acceptPoint(*liveEndpoint, true);
    return !overflow;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
int boids_init(uint32_t seed) {
    try {
        // Enforce 1 worker thread (strictly serial execution, no pthread / SharedArrayBuffer)
        g_simulation = std::make_unique<flock::Simulation>(seed, 1);
        g_birdInstances.resize(static_cast<std::size_t>(flock::MaxBirds));
        g_trailInstances.resize(MaxTrailSegments);
        g_birdCount = 0;
        g_trailCount = 0;
        return 1;
    } catch (...) {
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE
void boids_destroy() {
    g_simulation.reset();
    g_birdInstances.clear();
    g_trailInstances.clear();
    g_birdCount = 0;
    g_trailCount = 0;
}

EMSCRIPTEN_KEEPALIVE
void boids_reset(uint32_t seed) {
    if (g_simulation) {
        g_simulation->reset(seed);
    }
}

EMSCRIPTEN_KEEPALIVE
void boids_configure(
    int count, float size, float speed, float speedError,
    float separation, float alignment, float cohesion, float edge,
    int trails, float trailDuration,
    float colorR, float colorG, float colorB,
    float brightness, float contrast,
    int randomColor, int avoidTaskbar,
    float marginLeft, float marginRight, float marginTop, float marginBottom,
    float renderScale, int fps, int trailHz,
    float boundLeft, float boundTop, float boundRight, float boundBottom,
    int hasExclusion, float exclLeft, float exclTop, float exclRight, float exclBottom
) {
    if (!g_simulation) return;

    flock::Settings settings;
    settings.count = count;
    settings.size = size;
    settings.speed = speed;
    settings.speedError = speedError;
    settings.separation = separation;
    settings.alignment = alignment;
    settings.cohesion = cohesion;
    settings.edge = edge;
    settings.trails = (trails != 0);
    settings.trailDuration = trailDuration;
    settings.color = {colorR, colorG, colorB};
    settings.brightness = brightness;
    settings.contrast = contrast;
    settings.randomColor = (randomColor != 0);
    settings.avoidTaskbar = (avoidTaskbar != 0);
    settings.marginLeft = marginLeft;
    settings.marginRight = marginRight;
    settings.marginTop = marginTop;
    settings.marginBottom = marginBottom;
    settings.renderScale = renderScale;
    settings.fps = fps;
    settings.trailHz = trailHz;

    flock::Bounds bounds{boundLeft, boundTop, boundRight, boundBottom};

    if (hasExclusion) {
        flock::Bounds exclusions[1] = {{exclLeft, exclTop, exclRight, exclBottom}};
        g_simulation->configure(settings, bounds, exclusions);
    } else {
        g_simulation->configure(settings, bounds, {});
    }
}

EMSCRIPTEN_KEEPALIVE
void boids_step(float dt, float pointerX, float pointerY, int pointerActive, int click, int paused) {
    if (!g_simulation) return;

    flock::Input input{};
    input.pointer = {pointerX, pointerY};
    input.pointerActive = (pointerActive != 0);
    input.click = (click != 0);

    g_simulation->step(dt, input, (paused != 0));
}

EMSCRIPTEN_KEEPALIVE
float boids_get_time() {
    return g_simulation ? g_simulation->time() : 0.0f;
}

EMSCRIPTEN_KEEPALIVE
void boids_get_stats(int* active, int* entering, int* leaving, int* dying,
                     uint64_t* candidates, float* simulationMs) {
    if (!g_simulation) {
        if (active) *active = 0;
        if (entering) *entering = 0;
        if (leaving) *leaving = 0;
        if (dying) *dying = 0;
        if (candidates) *candidates = 0;
        if (simulationMs) *simulationMs = 0.0f;
        return;
    }
    const flock::SimStats stats = g_simulation->stats();
    if (active) *active = stats.active;
    if (entering) *entering = stats.entering;
    if (leaving) *leaving = stats.leaving;
    if (dying) *dying = stats.dying;
    if (candidates) *candidates = stats.candidates;
    if (simulationMs) *simulationMs = stats.simulationMs;
}

EMSCRIPTEN_KEEPALIVE
int boids_prepare_render_frame(float simulationTime, float logicalWidth, float logicalHeight) {
    g_birdCount = 0;
    g_trailCount = 0;
    if (!g_simulation) return -1;
    if (!std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) ||
        logicalWidth <= 0.0f || logicalHeight <= 0.0f) {
        return 0;
    }
    if (!std::isfinite(simulationTime)) {
        simulationTime = g_simulation->time();
    }

    const flock::Settings& settings = g_simulation->settings();
    const float duration = bounded(settings.trailDuration, 0.1f, 0.5f, 0.28f);
    const float trailHz = bounded(static_cast<float>(settings.trailHz), 10.0f, 60.0f, 30.0f);
    const float sampleInterval = 1.0f / trailHz;
    const flock::Color settingColor = normalizedColor(settings.color);
    const bool trailsEnabled = settings.trails;

    const std::span<const flock::Bird> birds = g_simulation->birds();
    const std::span<const flock::Ghost> ghosts = g_simulation->ghosts();

    for (const flock::Bird& bird : birds) {
        if (bird.phase == flock::Phase::Dead) continue;
        if (!std::isfinite(bird.position.x) || !std::isfinite(bird.position.y)) continue;
        if (static_cast<std::size_t>(g_birdCount) >= g_birdInstances.size()) break;

        const float vx = std::isfinite(bird.velocity.x) ? bird.velocity.x : 0.0f;
        const float vy = std::isfinite(bird.velocity.y) ? bird.velocity.y : 0.0f;
        const std::uint32_t phaseHash = (bird.id + 1u) * 374761393u;
        const float fallbackPhase = static_cast<float>((phaseHash ^ (phaseHash >> 13)) & 0xffffu) *
                                    (2.0f * Pi / 65536.0f);
        const float phase = std::isfinite(bird.wingPhase) ? bird.wingPhase : fallbackPhase;

        float deathProgress = -1.0f;
        float phaseAlpha = 1.0f;
        float pathEnd = simulationTime;

        if (bird.phase == flock::Phase::Dying) {
            const float elapsed = std::max(0.0f, std::isfinite(bird.phaseTime) ? bird.phaseTime : 0.0f);
            deathProgress = std::clamp(elapsed / 0.28f, 0.0f, 1.0f);
            pathEnd = simulationTime - elapsed;
        } else if (bird.phase == flock::Phase::Entering) {
            const float elapsed = std::max(0.0f, std::isfinite(bird.phaseTime) ? bird.phaseTime : 0.0f);
            phaseAlpha = std::clamp(elapsed / 0.45f, 0.0f, 1.0f);
            phaseAlpha = phaseAlpha * phaseAlpha * (3.0f - 2.0f * phaseAlpha);
        } else if (bird.phase == flock::Phase::Leaving) {
            float timeToViewportEdge = std::numeric_limits<float>::infinity();
            if (vx > 1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (logicalWidth - bird.position.x) / vx);
            else if (vx < -1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (0.0f - bird.position.x) / vx);
            if (vy > 1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (logicalHeight - bird.position.y) / vy);
            else if (vy < -1.0e-4f)
                timeToViewportEdge = std::min(timeToViewportEdge, (0.0f - bird.position.y) / vy);
            if (std::isfinite(timeToViewportEdge)) {
                const float progress = std::clamp(1.0f - timeToViewportEdge / 0.42f, 0.0f, 1.0f);
                phaseAlpha = 1.0f - progress * progress * (3.0f - 2.0f * progress);
            }
        }

        const flock::Color birdColor = normalizedColor(bird.color, settingColor);
        g_birdInstances[static_cast<std::size_t>(g_birdCount++)] = {
            bird.position.x, bird.position.y,
            vx, vy,
            phase,
            deathProgress,
            phaseAlpha,
            birdColor.r, birdColor.g, birdColor.b
        };

        if (trailsEnabled && bird.historyCount > 0) {
            flock::TrailPoint endpoint{};
            endpoint.position = bird.position;
            endpoint.time = pathEnd;
            endpoint.color = birdColor;
            appendHistory(bird.history, bird.historyHead, bird.historyCount,
                          simulationTime, pathEnd, duration, sampleInterval, &endpoint);
        }
    }

    if (trailsEnabled) {
        for (const flock::Ghost& ghost : ghosts) {
            if (ghost.historyCount <= 1 || !std::isfinite(ghost.retiredAt)) continue;
            const float pathEnd = std::min(simulationTime, ghost.retiredAt);
            if (pathEnd <= simulationTime - duration) continue;
            appendHistory(ghost.history, ghost.historyHead, ghost.historyCount,
                          simulationTime, pathEnd, duration, sampleInterval, nullptr);
        }
    }

    return 0;
}

EMSCRIPTEN_KEEPALIVE
int boids_get_bird_count() {
    return g_birdCount;
}

EMSCRIPTEN_KEEPALIVE
const float* boids_get_bird_instances() {
    return reinterpret_cast<const float*>(g_birdInstances.data());
}

EMSCRIPTEN_KEEPALIVE
int boids_get_trail_count() {
    return g_trailCount;
}

EMSCRIPTEN_KEEPALIVE
const float* boids_get_trail_instances() {
    return reinterpret_cast<const float*>(g_trailInstances.data());
}

EMSCRIPTEN_KEEPALIVE
const float* boids_get_bird_mesh() {
    return reinterpret_cast<const float*>(BirdMesh.data());
}

EMSCRIPTEN_KEEPALIVE
int boids_get_bird_mesh_vertex_count() {
    return static_cast<int>(BirdMesh.size());
}

EMSCRIPTEN_KEEPALIVE
const float* boids_get_trail_corners() {
    return TrailCorners;
}

EMSCRIPTEN_KEEPALIVE
int boids_get_trail_corner_vertex_count() {
    return 6;
}

} // extern "C"
