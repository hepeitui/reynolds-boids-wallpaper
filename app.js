(function () {
  "use strict";

  // Fixed 60Hz physics accumulator constants
  const FIXED_DT = 1.0 / 60.0;
  const MAX_CATCHUP_STEPS = 5;

  // State & Settings
  const state = {
    settings: {
      count: 1500,
      size: 6.58,
      speed: 87.0, // logical px/s, strictly aligned with native model.hpp
      speedError: 0.07,
      separation: 1.38,
      alignment: 1.0,
      cohesion: 0.65,
      edge: 0.175,
      trails: true,
      trailDuration: 0.28,
      color: "0.518 0.847 0.878",
      colorRgb: [0.518, 0.847, 0.878],
      brightness: 1.31,
      contrast: 1.2,
      randomColor: false,
      avoidTaskbar: true,
      taskbarPosition: "bottom",
      taskbarMargin: 48,
      marginLeft: 0,
      marginRight: 0,
      marginTop: 0,
      marginBottom: 0,
      renderScale: 1.0,
      fps: 60,
      trailHz: 30,
      showStats: false
    },
    paused: false,
    weMaxFps: null,
    width: window.innerWidth || 1920,
    height: window.innerHeight || 1080,
    dpr: window.devicePixelRatio || 1,
    pointer: {
      x: -10000,
      y: -10000,
      active: 0,
      pendingClick: 0
    },
    stats: {
      active: 0,
      entering: 0,
      leaving: 0,
      dying: 0,
      candidates: 0,
      renderedBirds: 0,
      renderedTrails: 0,
      simulationMs: 0.0,
      fps: 0,
      simTime: 0.0,
      simSteps: 0,
      submittedFrames: 0
    },
    inHostWE: false,
    wasmLoaded: false
  };

  // Pause source tracking
  let pausedByHost = false;
  let pausedByVisibility = typeof document !== "undefined" && document.hidden ? true : false;

  function isPaused() {
    return pausedByHost || pausedByVisibility;
  }

  function quantize(value, min, max, step, fallback) {
    if (typeof value !== "number" || !isFinite(value)) return fallback;
    const clamped = Math.max(min, Math.min(max, value));
    return Math.round((clamped - min) / step) * step + min;
  }

  function sanitizeSettings(s) {
    s.count = Math.round(quantize(s.count, 0, 3000, 1, 1500));
    s.size = quantize(s.size, 1.0, 16.0, 0.01, 6.58);
    s.speed = quantize(s.speed, 20.0, 240.0, 1.0, 87.0);
    s.speedError = quantize(s.speedError, 0.0, 0.5, 0.01, 0.07);
    s.separation = quantize(s.separation, 0.0, 4.0, 0.01, 1.38);
    s.alignment = quantize(s.alignment, 0.0, 4.0, 0.01, 1.0);
    s.cohesion = quantize(s.cohesion, 0.0, 4.0, 0.01, 0.65);
    s.edge = quantize(s.edge, 0.0, 0.35, 0.001, 0.175);
    s.trailDuration = quantize(s.trailDuration, 0.1, 0.5, 0.01, 0.28);
    s.brightness = quantize(s.brightness, 0.1, 2.0, 0.01, 1.31);
    s.contrast = quantize(s.contrast, 0.5, 2.0, 0.01, 1.2);
    s.taskbarMargin = quantize(s.taskbarMargin, 0, 200, 1, 48);
    s.marginLeft = quantize(s.marginLeft, 0, 200, 1, 0);
    s.marginRight = quantize(s.marginRight, 0, 200, 1, 0);
    s.marginTop = quantize(s.marginTop, 0, 200, 1, 0);
    s.marginBottom = quantize(s.marginBottom, 0, 200, 1, 0);
    s.renderScale = quantize(s.renderScale, 0.5, 1.0, 0.01, 1.0);
    s.fps = Math.round(quantize(s.fps, 15, 120, 1, 60));
    s.trailHz = Math.round(quantize(s.trailHz, 10, 60, 1, 30));
    if (Array.isArray(s.colorRgb)) {
      s.colorRgb[0] = Math.max(0, Math.min(1, s.colorRgb[0]));
      s.colorRgb[1] = Math.max(0, Math.min(1, s.colorRgb[1]));
      s.colorRgb[2] = Math.max(0, Math.min(1, s.colorRgb[2]));
    }
  }

  function getEffectiveFps() {
    let fps = state.settings.fps || 60;
    if (typeof state.weMaxFps === "number" && state.weMaxFps > 0) {
      fps = Math.min(fps, state.weMaxFps);
    }
    return Math.max(1, Math.min(120, fps));
  }

  // Immediate global registration for Wallpaper Engine
  window.wallpaperPropertyListener = {
    applyUserProperties: function (properties) {
      state.inHostWE = true;
      const s = state.settings;

      if (properties.count) s.count = parseInt(properties.count.value, 10);
      if (properties.size) s.size = parseFloat(properties.size.value);
      if (properties.speed) s.speed = parseFloat(properties.speed.value);
      if (properties.speedError) s.speedError = parseFloat(properties.speedError.value);
      if (properties.separation) s.separation = parseFloat(properties.separation.value);
      if (properties.alignment) s.alignment = parseFloat(properties.alignment.value);
      if (properties.cohesion) s.cohesion = parseFloat(properties.cohesion.value);
      if (properties.edge) s.edge = parseFloat(properties.edge.value);
      if (properties.trails) s.trails = !!properties.trails.value;
      if (properties.trailDuration) s.trailDuration = parseFloat(properties.trailDuration.value);
      if (properties.brightness) s.brightness = parseFloat(properties.brightness.value);
      if (properties.contrast) s.contrast = parseFloat(properties.contrast.value);
      if (properties.randomColor) s.randomColor = !!properties.randomColor.value;
      if (properties.avoidTaskbar) s.avoidTaskbar = !!properties.avoidTaskbar.value;
      if (properties.taskbarPosition) s.taskbarPosition = properties.taskbarPosition.value;
      if (properties.taskbarMargin) s.taskbarMargin = parseFloat(properties.taskbarMargin.value);
      if (properties.marginLeft) s.marginLeft = parseFloat(properties.marginLeft.value);
      if (properties.marginRight) s.marginRight = parseFloat(properties.marginRight.value);
      if (properties.marginTop) s.marginTop = parseFloat(properties.marginTop.value);
      if (properties.marginBottom) s.marginBottom = parseFloat(properties.marginBottom.value);
      if (properties.renderScale) s.renderScale = parseFloat(properties.renderScale.value);
      if (properties.fps) s.fps = parseInt(properties.fps.value, 10) || 60;
      if (properties.trailHz) s.trailHz = parseInt(properties.trailHz.value, 10) || 30;
      if (properties.showStats) s.showStats = !!properties.showStats.value;

      if (properties.color && properties.color.value) {
        s.color = properties.color.value;
        const parts = s.color.trim().split(/\s+/).map(Number);
        if (parts.length >= 3 && !parts.some(isNaN)) {
          s.colorRgb = [parts[0], parts[1], parts[2]];
        }
      }

      sanitizeSettings(s);
      applyHostWEMode();
      syncUIFromSettings();

      if (properties.renderScale) {
        handleResize();
      } else {
        pushConfigurationToWasm();
        if (isPaused()) {
          renderSingleFrame();
        } else {
          wake();
        }
      }
    },

    applyGeneralProperties: function (properties) {
      if (properties && typeof properties.fps !== "undefined") {
        const val = parseInt(properties.fps, 10);
        if (val > 0) {
          state.weMaxFps = val;
        }
      }
    },

    setPaused: function (isPausedVal) {
      pausedByHost = !!isPausedVal;
      updatePauseState();
    }
  };

  function applyHostWEMode() {
    if (!state.inHostWE) return;
    if (el.toggle) {
      el.toggle.classList.add("hidden");
    }
    if (el.drawer) {
      el.drawer.classList.add("hidden");
    }
  }

  // Expose read-only diagnostics for parent agent / test automation
  window.flockDiagnostics = function () {
    readStats();
    return {
      paused: state.paused,
      inHostWE: state.inHostWE,
      wasmLoaded: state.wasmLoaded,
      simTime: state.stats.simTime,
      simSteps: state.stats.simSteps,
      submittedFrames: state.stats.submittedFrames,
      activeBirds: state.stats.active,
      enteringBirds: state.stats.entering,
      leavingBirds: state.stats.leaving,
      dyingBirds: state.stats.dying,
      candidates: state.stats.candidates,
      renderedBirds: state.stats.renderedBirds,
      renderedTrails: state.stats.renderedTrails,
      simulationMs: state.stats.simulationMs,
      measuredFps: state.stats.fps,
      viewport: {
        width: state.width,
        height: state.height,
        dpr: state.dpr
      },
      settings: { ...state.settings }
    };
  };

  let wasm = null;
  let renderer = null;
  let canvas = null;

  // DOM Elements
  const el = {};

  function initDOM() {
    canvas = document.getElementById("boids-canvas");
    el.toggle = document.getElementById("toggle-settings");
    el.drawer = document.getElementById("settings-drawer");
    el.close = document.getElementById("close-drawer");
    el.reset = document.getElementById("btn-reset");
    el.stats = document.getElementById("stats-display");
    el.statsOverlay = document.getElementById("stats-overlay");

    el.count = document.getElementById("prop-count");
    el.valCount = document.getElementById("val-count");
    el.size = document.getElementById("prop-size");
    el.valSize = document.getElementById("val-size");
    el.speed = document.getElementById("prop-speed");
    el.valSpeed = document.getElementById("val-speed");
    el.speedError = document.getElementById("prop-speedError");
    el.valSpeedError = document.getElementById("val-speedError");
    el.separation = document.getElementById("prop-separation");
    el.valSeparation = document.getElementById("val-separation");
    el.alignment = document.getElementById("prop-alignment");
    el.valAlignment = document.getElementById("val-alignment");
    el.cohesion = document.getElementById("prop-cohesion");
    el.valCohesion = document.getElementById("val-cohesion");
    el.edge = document.getElementById("prop-edge");
    el.valEdge = document.getElementById("val-edge");
    el.trails = document.getElementById("prop-trails");
    el.trailDuration = document.getElementById("prop-trailDuration");
    el.valTrailDuration = document.getElementById("val-trailDuration");
    el.trailHz = document.getElementById("prop-trailHz");
    el.valTrailHz = document.getElementById("val-trailHz");
    el.brightness = document.getElementById("prop-brightness");
    el.valBrightness = document.getElementById("val-brightness");
    el.contrast = document.getElementById("prop-contrast");
    el.valContrast = document.getElementById("val-contrast");
    el.renderScale = document.getElementById("prop-renderScale");
    el.valRenderScale = document.getElementById("val-renderScale");
    el.fps = document.getElementById("prop-fps");
    el.valFps = document.getElementById("val-fps");
    el.randomColor = document.getElementById("prop-randomColor");
    el.color = document.getElementById("prop-color");
    el.avoidTaskbar = document.getElementById("prop-avoidTaskbar");
    el.taskbarPosition = document.getElementById("prop-taskbarPosition");
    el.taskbarMargin = document.getElementById("prop-taskbarMargin");
    el.valTaskbarMargin = document.getElementById("val-taskbarMargin");
    el.marginLeft = document.getElementById("prop-marginLeft");
    el.valMarginLeft = document.getElementById("val-marginLeft");
    el.marginRight = document.getElementById("prop-marginRight");
    el.valMarginRight = document.getElementById("val-marginRight");
    el.marginTop = document.getElementById("prop-marginTop");
    el.valMarginTop = document.getElementById("val-marginTop");
    el.marginBottom = document.getElementById("prop-marginBottom");
    el.valMarginBottom = document.getElementById("val-marginBottom");
    el.showStats = document.getElementById("prop-showStats");

    // Events
    if (el.toggle) {
      el.toggle.addEventListener("click", () => setDrawerVisible(true));
    }
    if (el.close) {
      el.close.addEventListener("click", () => setDrawerVisible(false));
    }
    if (el.reset) {
      el.reset.addEventListener("click", () => {
        if (wasm && wasm._boids_reset) {
          wasm._boids_reset(20261022);
          if (isPaused()) {
            renderSingleFrame();
          } else {
            wake();
          }
        }
      });
    }
    // Keyboard H shortcut for toggling drawer (only when not in WE host)
    window.addEventListener("keydown", (e) => {
      if (state.inHostWE) return;
      if (e.key === "h" || e.key === "H") {
        if (el.drawer) {
          setDrawerVisible(el.drawer.classList.contains("hidden"));
        }
      }
    });

    // Inputs binding helper
    const bindRange = (input, valSpan, key, fmt) => {
      if (!input) return;
      input.addEventListener("input", () => {
        const val = parseFloat(input.value);
        state.settings[key] = val;
        sanitizeSettings(state.settings);
        if (valSpan) {
          valSpan.textContent = fmt ? fmt(state.settings[key]) : state.settings[key];
        }
        if (key === "renderScale") {
          handleResize();
        } else {
          pushConfigurationToWasm();
          if (isPaused()) {
            renderSingleFrame();
          } else {
            wake();
          }
        }
      });
    };

    bindRange(el.count, el.valCount, "count", v => v);
    bindRange(el.size, el.valSize, "size", v => v.toFixed(2));
    bindRange(el.speed, el.valSpeed, "speed", v => Math.round(v));
    bindRange(el.speedError, el.valSpeedError, "speedError", v => v.toFixed(2));
    bindRange(el.separation, el.valSeparation, "separation", v => v.toFixed(2));
    bindRange(el.alignment, el.valAlignment, "alignment", v => v.toFixed(2));
    bindRange(el.cohesion, el.valCohesion, "cohesion", v => v.toFixed(2));
    bindRange(el.edge, el.valEdge, "edge", v => v.toFixed(3));
    bindRange(el.trailDuration, el.valTrailDuration, "trailDuration", v => v.toFixed(2) + "s");
    bindRange(el.trailHz, el.valTrailHz, "trailHz", v => Math.round(v));
    bindRange(el.brightness, el.valBrightness, "brightness", v => v.toFixed(2));
    bindRange(el.contrast, el.valContrast, "contrast", v => v.toFixed(2));
    bindRange(el.renderScale, el.valRenderScale, "renderScale", v => v.toFixed(2));
    bindRange(el.fps, el.valFps, "fps", v => Math.round(v));
    bindRange(el.taskbarMargin, el.valTaskbarMargin, "taskbarMargin", v => Math.round(v) + "px");
    bindRange(el.marginLeft, el.valMarginLeft, "marginLeft", v => Math.round(v) + "px");
    bindRange(el.marginRight, el.valMarginRight, "marginRight", v => Math.round(v) + "px");
    bindRange(el.marginTop, el.valMarginTop, "marginTop", v => Math.round(v) + "px");
    bindRange(el.marginBottom, el.valMarginBottom, "marginBottom", v => Math.round(v) + "px");

    if (el.trails) {
      el.trails.addEventListener("change", () => {
        state.settings.trails = el.trails.checked;
        pushConfigurationToWasm();
        if (isPaused()) renderSingleFrame();
        else wake();
      });
    }

    if (el.randomColor) {
      el.randomColor.addEventListener("change", () => {
        state.settings.randomColor = el.randomColor.checked;
        pushConfigurationToWasm();
        if (isPaused()) renderSingleFrame();
        else wake();
      });
    }

    if (el.avoidTaskbar) {
      el.avoidTaskbar.addEventListener("change", () => {
        state.settings.avoidTaskbar = el.avoidTaskbar.checked;
        pushConfigurationToWasm();
        if (isPaused()) renderSingleFrame();
        else wake();
      });
    }

    if (el.taskbarPosition) {
      el.taskbarPosition.addEventListener("change", () => {
        state.settings.taskbarPosition = el.taskbarPosition.value;
        pushConfigurationToWasm();
        if (isPaused()) renderSingleFrame();
        else wake();
      });
    }

    if (el.showStats) {
      el.showStats.addEventListener("change", () => {
        state.settings.showStats = el.showStats.checked;
        updateStatsUI();
      });
    }

    if (el.color) {
      el.color.addEventListener("input", () => {
        const hex = el.color.value;
        const r = parseInt(hex.slice(1, 3), 16) / 255.0;
        const g = parseInt(hex.slice(3, 5), 16) / 255.0;
        const b = parseInt(hex.slice(5, 7), 16) / 255.0;
        state.settings.colorRgb = [r, g, b];
        state.settings.color = `${r.toFixed(3)} ${g.toFixed(3)} ${b.toFixed(3)}`;
        pushConfigurationToWasm();
        if (isPaused()) renderSingleFrame();
        else wake();
      });
    }
    // Pointer events for canvas interaction with UI filtering
    function isEventInsideUI(e) {
      if (!e.target || !e.target.closest) return false;
      return !!e.target.closest("#settings-drawer, #toggle-settings, #stats-overlay");
    }

    window.addEventListener("pointermove", (e) => {
      if (isEventInsideUI(e)) {
        state.pointer.active = 0;
        return;
      }
      state.pointer.x = e.clientX;
      state.pointer.y = e.clientY;
      state.pointer.active = 1;
      wake();
    });

    window.addEventListener("pointerdown", (e) => {
      if (isEventInsideUI(e)) {
        state.pointer.active = 0;
        return;
      }
      state.pointer.x = e.clientX;
      state.pointer.y = e.clientY;
      state.pointer.active = 1;
      // Pending click persists until consumed by the physics simulation step
      state.pointer.pendingClick = 1;
      wake();
    });
    window.addEventListener("pointerup", (e) => {
      if (isEventInsideUI(e)) {
        state.pointer.active = 0;
        return;
      }
      // Rapid clicks must not drop pendingClick before the physics step runs
      state.pointer.active = 0;
    });

    window.addEventListener("pointerleave", () => {
      state.pointer.active = 0;
    });

    // Visibility change listener
    document.addEventListener("visibilitychange", () => {
      pausedByVisibility = document.hidden;
      updatePauseState();
    });

    window.addEventListener("resize", handleResize);
  }

  function setDrawerVisible(show) {
    if (state.inHostWE) return;
    if (show) {
      if (el.drawer) el.drawer.classList.remove("hidden");
      if (el.toggle) el.toggle.classList.add("hidden");
    } else {
      if (el.drawer) el.drawer.classList.add("hidden");
      if (el.toggle) el.toggle.classList.remove("hidden");
    }
  }

  function syncUIFromSettings() {
    const s = state.settings;
    if (el.count) {
      el.count.value = s.count;
      if (el.valCount) el.valCount.textContent = s.count;
      el.size.value = s.size;
      if (el.valSize) el.valSize.textContent = s.size.toFixed(2);
      el.speed.value = s.speed;
      if (el.valSpeed) el.valSpeed.textContent = Math.round(s.speed);
      if (el.speedError) {
        el.speedError.value = s.speedError;
        if (el.valSpeedError) el.valSpeedError.textContent = s.speedError.toFixed(2);
      }
      el.separation.value = s.separation;
      if (el.valSeparation) el.valSeparation.textContent = s.separation.toFixed(2);
      el.alignment.value = s.alignment;
      if (el.valAlignment) el.valAlignment.textContent = s.alignment.toFixed(2);
      el.cohesion.value = s.cohesion;
      if (el.valCohesion) el.valCohesion.textContent = s.cohesion.toFixed(2);
      el.edge.value = s.edge;
      if (el.valEdge) el.valEdge.textContent = s.edge.toFixed(3);
      el.trails.checked = s.trails;
      el.trailDuration.value = s.trailDuration;
      if (el.valTrailDuration) el.valTrailDuration.textContent = s.trailDuration.toFixed(2) + "s";
      if (el.trailHz) {
        el.trailHz.value = s.trailHz;
        if (el.valTrailHz) el.valTrailHz.textContent = Math.round(s.trailHz);
      }
      if (el.brightness) {
        el.brightness.value = s.brightness;
        if (el.valBrightness) el.valBrightness.textContent = s.brightness.toFixed(2);
      }
      if (el.contrast) {
        el.contrast.value = s.contrast;
        if (el.valContrast) el.valContrast.textContent = s.contrast.toFixed(2);
      }
      if (el.renderScale) {
        el.renderScale.value = s.renderScale;
        if (el.valRenderScale) el.valRenderScale.textContent = s.renderScale.toFixed(2);
      }
      if (el.fps) {
        el.fps.value = s.fps;
        if (el.valFps) el.valFps.textContent = Math.round(s.fps);
      }
      el.randomColor.checked = s.randomColor;
      el.avoidTaskbar.checked = s.avoidTaskbar;
      if (el.taskbarPosition) el.taskbarPosition.value = s.taskbarPosition;
      if (el.taskbarMargin) {
        el.taskbarMargin.value = s.taskbarMargin;
        if (el.valTaskbarMargin) el.valTaskbarMargin.textContent = Math.round(s.taskbarMargin) + "px";
      }
      if (el.marginLeft) {
        el.marginLeft.value = s.marginLeft;
        if (el.valMarginLeft) el.valMarginLeft.textContent = Math.round(s.marginLeft) + "px";
      }
      if (el.marginRight) {
        el.marginRight.value = s.marginRight;
        if (el.valMarginRight) el.valMarginRight.textContent = Math.round(s.marginRight) + "px";
      }
      if (el.marginTop) {
        el.marginTop.value = s.marginTop;
        if (el.valMarginTop) el.valMarginTop.textContent = Math.round(s.marginTop) + "px";
      }
      if (el.marginBottom) {
        el.marginBottom.value = s.marginBottom;
        if (el.valMarginBottom) el.valMarginBottom.textContent = Math.round(s.marginBottom) + "px";
      }
      if (el.showStats) {
        el.showStats.checked = s.showStats;
      }

      const toHex = (c) => {
        const h = Math.round(Math.min(1.0, Math.max(0.0, c)) * 255).toString(16);
        return h.length === 1 ? "0" + h : h;
      };
      if (el.color && s.colorRgb) {
        el.color.value = `#${toHex(s.colorRgb[0])}${toHex(s.colorRgb[1])}${toHex(s.colorRgb[2])}`;
      }
    }
    updateStatsUI();
  }

  function handleResize() {
    const width = window.innerWidth || 1920;
    const height = window.innerHeight || 1080;
    const dpr = window.devicePixelRatio || 1;

    state.width = width;
    state.height = height;
    state.dpr = dpr;

    const scale = Math.max(0.5, Math.min(1.0, state.settings.renderScale || 1.0));
    const newCanvasWidth = Math.max(1, Math.round(width * dpr * scale));
    const newCanvasHeight = Math.max(1, Math.round(height * dpr * scale));

    if (canvas && (canvas.width !== newCanvasWidth || canvas.height !== newCanvasHeight)) {
      canvas.width = newCanvasWidth;
      canvas.height = newCanvasHeight;
    }

    pushConfigurationToWasm();

    // Repaint or wake on resize
    if (isPaused()) {
      renderSingleFrame();
    } else {
      wake();
    }
  }
  function pushConfigurationToWasm() {
    if (!wasm || !wasm._boids_configure) return;
    const s = state.settings;
    const width = state.width;
    const height = state.height;

    // Base bounds accounting for user manual margins
    const boundLeft = Math.max(0, Math.min(width, s.marginLeft));
    const boundRight = Math.max(boundLeft, Math.min(width, width - s.marginRight));
    const boundTop = Math.max(0, Math.min(height, s.marginTop));
    const boundBottom = Math.max(boundTop, Math.min(height, height - s.marginBottom));

    let hasExcl = 0;
    let exLeft = 0, exTop = 0, exRight = 0, exBottom = 0;

    if (s.avoidTaskbar) {
      hasExcl = 1;
      const margin = s.taskbarMargin;
      if (s.taskbarPosition === "bottom") {
        exLeft = 0;
        exRight = width;
        exTop = Math.max(0, height - margin);
        exBottom = height;
      } else if (s.taskbarPosition === "top") {
        exLeft = 0;
        exRight = width;
        exTop = 0;
        exBottom = Math.min(height, margin);
      } else if (s.taskbarPosition === "left") {
        exLeft = 0;
        exRight = Math.min(width, margin);
        exTop = 0;
        exBottom = height;
      } else if (s.taskbarPosition === "right") {
        exLeft = Math.max(0, width - margin);
        exRight = width;
        exTop = 0;
        exBottom = height;
      }
    }

    wasm._boids_configure(
      s.count, s.size, s.speed, s.speedError,
      s.separation, s.alignment, s.cohesion, s.edge,
      s.trails ? 1 : 0, s.trailDuration,
      s.colorRgb[0], s.colorRgb[1], s.colorRgb[2],
      s.brightness, s.contrast,
      s.randomColor ? 1 : 0, s.avoidTaskbar ? 1 : 0,
      s.marginLeft, s.marginRight, s.marginTop, s.marginBottom,
      s.renderScale, s.fps, s.trailHz,
      boundLeft, boundTop, boundRight, boundBottom,
      hasExcl, exLeft, exTop, exRight, exBottom
    );
  }

  // Stats buffer pointers for wasm._boids_get_stats
  let pActive = 0, pEntering = 0, pLeaving = 0, pDying = 0, pCandidates = 0, pSimulationMs = 0;

  function initStatsBuffers() {
    if (!wasm || !wasm._malloc) return;
    pActive = wasm._malloc(4);
    pEntering = wasm._malloc(4);
    pLeaving = wasm._malloc(4);
    pDying = wasm._malloc(4);
    pCandidates = wasm._malloc(8); // 64-bit uint64_t
    pSimulationMs = wasm._malloc(4);
  }

  function readStats() {
    if (!wasm || !pActive || !wasm._boids_get_stats) return;
    wasm._boids_get_stats(pActive, pEntering, pLeaving, pDying, pCandidates, pSimulationMs);
    state.stats.active = wasm.HEAP32[pActive >> 2];
    state.stats.entering = wasm.HEAP32[pEntering >> 2];
    state.stats.leaving = wasm.HEAP32[pLeaving >> 2];
    state.stats.dying = wasm.HEAP32[pDying >> 2];

    // Read 64-bit unsigned candidates
    const low = wasm.HEAP32[pCandidates >> 2] >>> 0;
    const high = wasm.HEAP32[(pCandidates >> 2) + 1] >>> 0;
    state.stats.candidates = low + high * 4294967296;

    state.stats.simulationMs = wasm.HEAPF32[pSimulationMs >> 2];
    state.stats.simTime = wasm._boids_get_time ? wasm._boids_get_time() : 0.0;

    // Actual render instance counts from renderer or bridge exports
    if (renderer && typeof renderer.birdCount === "number") {
      state.stats.renderedBirds = renderer.birdCount;
    } else if (wasm._boids_get_bird_count) {
      state.stats.renderedBirds = wasm._boids_get_bird_count();
    }
    if (renderer && typeof renderer.trailCount === "number") {
      state.stats.renderedTrails = renderer.trailCount;
    } else if (wasm._boids_get_trail_count) {
      state.stats.renderedTrails = wasm._boids_get_trail_count();
    }
  }

  function updateStatsUI() {
    const text = `${state.stats.active} 鸟 | ${state.stats.simulationMs.toFixed(1)} ms | ${state.stats.fps} FPS`;
    if (el.stats) {
      el.stats.textContent = text;
    }
    if (el.statsOverlay) {
      if (state.settings.showStats) {
        el.statsOverlay.classList.remove("hidden");
        el.statsOverlay.textContent = `${state.stats.active} 鸟 | ${state.stats.simulationMs.toFixed(1)} ms | ${state.stats.fps} FPS | ${state.stats.candidates} cand`;
      } else {
        el.statsOverlay.classList.add("hidden");
      }
    }
  }

  // Physics Accumulator & Render Loop
  let accumulator = 0.0;
  let lastTime = 0;
  let lastDraw = 0;
  let frameCount = 0;
  let fpsTimer = 0;
  let rafId = null;

  function wake() {
    if (!rafId && !isPaused() && renderer && renderer.isAvailable) {
      rafId = requestAnimationFrame(loop);
    }
  }

  function renderSingleFrame() {
    if (!wasm || !renderer || !renderer.isAvailable) return;
    const simTime = wasm._boids_get_time ? wasm._boids_get_time() : 0.0;
    const success = renderer.render(state.settings, simTime, state.width, state.height);
    if (success) {
      state.stats.submittedFrames++;
    }
    readStats();
    updateStatsUI();
  }

  function loop(currentTime) {
    rafId = null;
    if (isPaused() || !renderer || !renderer.isAvailable) {
      lastTime = 0;
      lastDraw = 0;
      return;
    }

    const targetFps = getEffectiveFps();
    const interval = 1000.0 / targetFps;

    if (lastTime === 0) {
      lastTime = currentTime;
      lastDraw = currentTime - interval;
    }

    const deltaMs = Math.min(250.0, currentTime - lastTime);
    lastTime = currentTime;
    accumulator += deltaMs / 1000.0;

    // Cadence remainder tracking for high-refresh displays
    const drawDue = currentTime - lastDraw >= interval - 0.2;
    if (drawDue) {
      lastDraw += Math.max(1, Math.floor((currentTime - lastDraw + 0.2) / interval)) * interval;
      frameCount++;

      if (wasm && renderer && renderer.isAvailable) {
        const simTime = wasm._boids_get_time ? wasm._boids_get_time() : 0.0;
        const success = renderer.render(state.settings, simTime, state.width, state.height);
        if (success) {
          state.stats.submittedFrames++;
        }
      }
    }

    // Up to 5 catch-up steps; discard excess to prevent death spiral
    if (accumulator > MAX_CATCHUP_STEPS * FIXED_DT) {
      accumulator = MAX_CATCHUP_STEPS * FIXED_DT;
    }

    // Step physics at fixed 60Hz
    let stepsExecuted = 0;
    while (accumulator >= FIXED_DT && stepsExecuted < MAX_CATCHUP_STEPS) {
      accumulator -= FIXED_DT;
      const click = state.pointer.pendingClick;
      state.pointer.pendingClick = 0; // Consume click on first simulation step

      if (wasm && wasm._boids_step) {
        wasm._boids_step(
          FIXED_DT,
          state.pointer.x,
          state.pointer.y,
          state.pointer.active,
          click,
          0 // not paused in real simulation step
        );
      }
      state.stats.simSteps++;
      stepsExecuted++;
    }

    // FPS measurement timer
    fpsTimer += deltaMs;
    if (fpsTimer >= 500) {
      state.stats.fps = Math.round((frameCount * 1000) / fpsTimer);
      frameCount = 0;
      fpsTimer = 0;
      readStats();
      updateStatsUI();
    }

    // True idle check:
    // When configured count is 0 and all birds/ghosts have fully exited/died, stop RAF.
    const hasLivingBirds = (renderer ? renderer.birdCount : (wasm._boids_get_bird_count ? wasm._boids_get_bird_count() : 0)) > 0;
    const hasLivingTrails = (renderer ? renderer.trailCount : (wasm._boids_get_trail_count ? wasm._boids_get_trail_count() : 0)) > 0;
    const shouldStayAwake = state.settings.count > 0 || hasLivingBirds || hasLivingTrails || state.pointer.pendingClick;

    if (shouldStayAwake) {
      wake();
    } else {
      // Final clear render before entering true idle
      if (stepsExecuted > 0 && renderer && renderer.isAvailable) {
        const simTime = wasm._boids_get_time ? wasm._boids_get_time() : 0.0;
        if (renderer.render(state.settings, simTime, state.width, state.height)) {
          state.stats.submittedFrames++;
        }
      }
      lastTime = 0;
      lastDraw = 0;
      state.stats.fps = 0;
      readStats();
      updateStatsUI();
    }
  }

  function updatePauseState() {
    const paused = isPaused();
    if (state.paused === paused) return;
    state.paused = paused;

    if (paused) {
      if (rafId !== null) {
        cancelAnimationFrame(rafId);
        rafId = null;
      }
      accumulator = 0.0;
      lastTime = 0;
      lastDraw = 0;
    } else {
      // Clear accumulator and clock on resume to prevent catch-up burst
      accumulator = 0.0;
      lastTime = 0;
      lastDraw = 0;
      wake();
    }
  }

  async function start() {
    initDOM();
    handleResize();

    // Instantiate Emscripten Module
    try {
      if (typeof ReynoldsBoidsModule === "function") {
        wasm = await ReynoldsBoidsModule();
      } else if (typeof Module !== "undefined") {
        wasm = Module;
      } else {
        console.error("WASM Module definition not found!");
        return;
      }
    } catch (err) {
      console.error("Error loading WASM module:", err);
      return;
    }

    // Initialize Simulation
    const initialSeed = 20261022;
    const initSuccess = wasm._boids_init ? wasm._boids_init(initialSeed) : 1;
    if (!initSuccess) {
      console.error("Failed to initialize flock::Simulation in WASM!");
      return;
    }

    initStatsBuffers();

    if (typeof WebGLBoidsRenderer === "function") {
      renderer = new WebGLBoidsRenderer(canvas, wasm);

      // Context restored callback: perform single repaint if paused
      renderer.onContextRestored = () => {
        if (isPaused()) {
          renderSingleFrame();
        }
      };
    }

    state.wasmLoaded = true;

    pushConfigurationToWasm();
    lastTime = 0;
    lastDraw = 0;

    if (!isPaused()) {
      wake();
    } else {
      renderSingleFrame();
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start);
  } else {
    start();
  }
})();
