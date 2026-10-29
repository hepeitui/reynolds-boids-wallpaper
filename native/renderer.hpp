#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "model.hpp"
#include <memory>
#include <string>
namespace flock {
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&)=delete;
    Renderer& operator=(const Renderer&)=delete;
    bool initialize(HWND window, int width, int height, std::string& error);
    bool resize(int width, int height, std::string& error);
    // Coordinates are client-area logical pixels. Bounds only constrain simulation.
    bool draw(std::span<const Bird> birds, std::span<const Ghost> ghosts,
              const Settings& settings, float simulationTime,
              float logicalWidth, float logicalHeight, std::string& error);
    bool capture(const std::wstring& path, std::string& error); // PNG of actual GPU backbuffer.
    std::string adapterName() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
