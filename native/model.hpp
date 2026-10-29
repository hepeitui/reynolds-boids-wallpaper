#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace flock {
// Runtime capacity is configurable only for dedicated stress builds.
#ifdef FLOCK_MAX_BIRDS
#if FLOCK_MAX_BIRDS < 1 || FLOCK_MAX_BIRDS > 32768
#error "FLOCK_MAX_BIRDS must be in the range 1..32768."
#endif
constexpr int MaxBirds = FLOCK_MAX_BIRDS;
#else
constexpr int MaxBirds = 3000;
#endif
constexpr int MaxGhosts = MaxBirds * 2;
constexpr int HistoryCapacity = 40; // 0.5 seconds at up to 60 Hz, plus interpolation margin.
constexpr float PerceptionRadius = 58.f;
constexpr float SeparationRadius = 15.f;
struct Vec2 { float x=0, y=0; };
struct Color { float r=0.518f, g=0.847f, b=0.878f; };
struct Bounds { float left=0, top=0, right=1440, bottom=900; };
struct Settings {
    int count=1500;
    float size=6.58f, speed=87.f, speedError=.07f;
    float separation=1.38f, alignment=1.f, cohesion=.65f, edge=.175f;
    bool trails=true;
    float trailDuration=.28f;
    Color color{};
    float brightness=1.31f, contrast=1.2f;
    bool randomColor=false, avoidTaskbar=true;
    float marginLeft=0, marginRight=0, marginTop=0, marginBottom=0;
    float renderScale=1.f;
    int fps=60, trailHz=30;
};
enum class Phase : std::uint8_t { Entering, Active, Leaving, Dying, Dead };
struct TrailPoint { Vec2 position{}; float time=0; Color color{}; };
struct Bird {
    Vec2 position{}, velocity{};
    std::uint32_t id=0;
    Phase phase=Phase::Dead;
    float speedBias=0, phaseTime=0, boostUntil=0, wingPhase=0;
    Color baseColor{}, color{};
    std::array<TrailPoint, HistoryCapacity> history{};
    int historyHead=0, historyCount=0;
    float lastSample=-100;
};
struct Ghost {
    std::array<TrailPoint, HistoryCapacity> history{};
    int historyHead=0, historyCount=0;
    float retiredAt=0;
};
struct Input { Vec2 pointer{}; bool pointerActive=false; bool click=false; };
struct SimStats { int active=0, entering=0, leaving=0, dying=0; std::uint64_t candidates=0; float simulationMs=0; };
// All coordinates and speeds are logical pixels and logical pixels/second.
// Host passes drawable bounds after taskbar/manual-margin exclusion.
}
