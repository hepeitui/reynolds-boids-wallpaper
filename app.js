(function (B) {
  "use strict";
  let settings = B.Settings.load(),
    sim = new B.Simulation(settings, innerWidth, innerHeight),
    history = new B.History();
  let manualPause = false,
    hostPause = false,
    hostFps = 60,
    raf = 0,
    last = 0,
    lastDraw = 0,
    accumulator = 0,
    lastUI = 0,
    submitted = 0,
    droppedTime = 0,
    simulationSteps = 0,
    catchUpSteps = 0,
    previousSubmit = 0,
    uiWindowStart = 0,
    uiFrames = 0;
  let capture = null,
    pendingCpu = 0;
  const canvas = document.getElementById("scene");
  const ui = B.createUI(configure, pause, reset, downloadReport);
  const renderer = new B.Renderer(canvas, (text) => {
    ui.status(text);
    queueMicrotask(() => {
      if (renderer.available) redraw();
    });
  });
  function updateWorkArea() {
    const area = B.Host.workArea(innerWidth, innerHeight, settings.avoidTaskbar);
    const previous = sim.workArea;
    if (sim.width === innerWidth && sim.height === innerHeight &&
        previous.left === area.left && previous.top === area.top &&
        previous.right === area.right && previous.bottom === area.bottom)
      return false;
    sim.resize(innerWidth, innerHeight, area);
    return true;
  }
  function resize() {
    sim.resize(innerWidth, innerHeight,
      B.Host.workArea(innerWidth, innerHeight, settings.avoidTaskbar));
    renderer.resize(
      innerWidth,
      innerHeight,
      devicePixelRatio || 1,
      settings.renderScale,
    );
    redraw();
  }
  function paused() {
    return manualPause || hostPause || document.hidden;
  }
  function wake() {
    if (!raf && !paused() && renderer.available)
      raf = requestAnimationFrame(frame);
  }
  function redraw() {
    if (renderer.available) renderer.render(sim, history, settings);
    ui.sync(settings);
    ui.setPaused(manualPause);
    wake();
  }
  function configure(patch) {
    settings = B.Settings.merge(settings, patch);
    sim.configure(settings);
    updateWorkArea();
    B.Settings.save(settings);
    renderer.resize(
      innerWidth,
      innerHeight,
      devicePixelRatio || 1,
      settings.renderScale,
    );
    redraw();
  }
  function reset() {
    sim = new B.Simulation(settings, innerWidth, innerHeight);
    sim.resize(innerWidth, innerHeight,
      B.Host.workArea(innerWidth, innerHeight, settings.avoidTaskbar));
    history = new B.History();
    last = 0;
    lastDraw = 0;
    accumulator = 0;
    redraw();
  }
  function pause(value) {
    manualPause = typeof value === "boolean" ? value : !manualPause;
    syncPause();
  }
  function syncPause() {
    if (raf) cancelAnimationFrame(raf);
    raf = 0;
    last = 0;
    lastDraw = 0;
    accumulator = 0;
    ui.setPaused(manualPause);
    if (paused()) {
      if (renderer.available) renderer.render(sim, history, settings);
    } else wake();
  }
  function frame(now) {
    raf = 0;
    if (paused() || !renderer.available) {
      last = 0;
      return;
    }
    const interval = 1000 / Math.min(settings.fps, hostFps);
    if (last === 0) {
      last = now;
      lastDraw = now - interval;
    }
    const elapsed = Math.min(0.1, (now - last) / 1000);
    last = now;
    accumulator += elapsed;
    // Publish the completed snapshot first, then integrate the next snapshot.
    // This overlaps GPU work with CPU steering instead of varying submit time
    // by zero, one or several fixed simulation steps on high-refresh displays.
    const drawDue = now - lastDraw >= interval - 0.2;
    if (drawDue) {
      // Retain cadence remainder on high-refresh displays, never busy-wait.
      lastDraw +=
        Math.max(1, Math.floor((now - lastDraw + 0.2) / interval)) * interval;
      const begin = performance.now();
      renderer.render(sim, history, settings);
      const submitEnd = performance.now(),
        submitMs = submitEnd - begin;
      submitted++;
      uiFrames++;
      const actualGap = previousSubmit ? submitEnd - previousSubmit : interval;
      previousSubmit = submitEnd;
      if (capture && capture.count < capture.gap.length) {
        const p = capture.count++;
        capture.gap[p] = capture.last ? submitEnd - capture.last : 0;
        capture.last = submitEnd;
        capture.cpu[p] = pendingCpu;
        capture.submit[p] = submitMs;
        capture.uploadBytes += renderer.stats.uploadBytes;
        if (
          renderer.stats.gpuMs !== null &&
          renderer.stats.gpuSamples !== capture.lastGpu
        ) {
          capture.gpu[capture.gpuCount++] = renderer.stats.gpuMs;
          capture.lastGpu = renderer.stats.gpuSamples;
        }
      }
      pendingCpu = 0;
      if (now - lastUI >= 500) {
        const fps = uiWindowStart
          ? (uiFrames * 1000) / (submitEnd - uiWindowStart)
          : 1000 / actualGap;
        ui.metrics({
          ...sim.stats,
          ...renderer.stats,
          fps,
          activeCount: sim.count,
          frameTimeMs: actualGap,
          renderMs: submitMs,
          hostFps,
        });
        lastUI = now;
        uiWindowStart = submitEnd;
        uiFrames = 0;
      }
    }
    let steps = 0;
    // Group fixed steps after a submission; an intermediate high-Hz callback
    // must not occupy the CPU just before the next eligible paint.
    while (drawDue && accumulator >= 1 / 60 && steps < 4) {
      const begin = performance.now();
      sim.step(1 / 60);
      simulationSteps++;
      history.record(sim);
      pendingCpu += performance.now() - begin;
      accumulator -= 1 / 60;
      steps++;
    }
    catchUpSteps += Math.max(0, steps - 1);
    if (drawDue && accumulator >= 1 / 60) {
      droppedTime += accumulator;
      accumulator = 0;
    }
    if (
      sim.count ||
      settings.count ||
      history.hasVisible(sim.time) ||
      sim.pulse.until > sim.time
    )
      wake();
    else {
      // The last integration may have expired the final ghost after this frame's
      // scheduled draw. Clear once at the new time before entering true idle.
      if (steps > 0) {
        renderer.render(sim, history, settings);
        submitted++;
      }
      last = 0;
      lastDraw = 0;
      ui.metrics({
        ...sim.stats,
        ...renderer.stats,
        fps: 0,
        activeCount: 0,
        hostFps,
      });
    }
  }
  function distribution(values, n, skip = 0) {
    if (n <= skip) return null;
    const a = Array.from(values.subarray(skip, n)).sort((a, b) => a - b);
    const at = (q) => a[Math.min(a.length - 1, Math.floor((a.length - 1) * q))];
    return {
      samples: a.length,
      p50: at(0.5),
      p95: at(0.95),
      p99: at(0.99),
      max: a[a.length - 1],
      mean: a.reduce((a, b) => a + b, 0) / a.length,
    };
  }
  function report() {
    const gl = canvas.getContext("webgl2"),
      ext = gl && gl.getExtension("WEBGL_debug_renderer_info");
    let timing = null;
    if (capture) {
      let slow = 0,
        total = 0,
        minWindowFps = Infinity,
        windowStart = 1,
        windowTime = 0;
      for (let n = 1; n < capture.count; n++) {
        const gap = capture.gap[n];
        total += gap;
        if (gap > 25) slow++;
        windowTime += gap;
        if (windowTime >= 10000) {
          minWindowFps = Math.min(
            minWindowFps,
            ((n - windowStart + 1) * 1000) / windowTime,
          );
          windowStart = n + 1;
          windowTime = 0;
        }
      }
      timing = {
        seconds: total / 1000,
        frames: capture.count,
        averageFps: total ? (1000 * (capture.count - 1)) / total : null,
        min10SecondFps: Number.isFinite(minWindowFps) ? minWindowFps : null,
        over25msRatio: capture.count > 1 ? slow / (capture.count - 1) : null,
        intervalMs: distribution(capture.gap, capture.count, 1),
        simulationMs: distribution(capture.cpu, capture.count),
        submitMs: distribution(capture.submit, capture.count),
        gpuMs: distribution(capture.gpu, capture.gpuCount),
        uploadBytesTotal: capture.uploadBytes,
        uploadMiBPerSecond:
          total > 0 ? (capture.uploadBytes * 1000) / total / 1048576 : null,
      };
    }
    if (timing) {
      timing.measurementVersion = 2;
      timing.intervalSource =
        "performance.now() after WebGL submission, not requestAnimationFrame timestamp";
      timing.gpuScope =
        "GPU upload, clear and draw commands; CPU preparation excluded";
    }
    return {
      timestamp: new Date().toISOString(),
      seed: sim.initialSeed,
      settings: { ...settings },
      simulationTime: sim.time,
      alive: sim.count,
      submitted,
      droppedSimulationSeconds: droppedTime,
      simulationSteps,
      catchUpSteps,
      stepAccounting:
        "Cumulative since page startup and not reset by reset(); simulationSteps counts sim.step calls and catchUpSteps sums max(0, steps per RAF frame - 1).",
      viewport: {
        width: innerWidth,
        height: innerHeight,
        dpr: devicePixelRatio,
        pixels: [canvas.width, canvas.height],
        screen: [screen.width, screen.height],
      },
      environment: {
        userAgent: navigator.userAgent,
        renderer: gl
          ? gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER)
          : null,
        host: B.Host.isWallpaper,
        hostFps,
        visibility: document.visibilityState,
        memory: performance.memory
          ? {
              used: performance.memory.usedJSHeapSize,
              total: performance.memory.totalJSHeapSize,
            }
          : null,
      },
      simulation: { ...sim.stats },
      neighbors: { ...sim.neighbors.metrics },
      renderer: { ...renderer.stats },
      history: { ghostOverflows: history.overflows },
      timing,
      limits:
        "Browser submissions are not desktop presentation proof. No direct electrical power measurement.",
    };
  }
  function startCapture() {
    pendingCpu = 0;
    capture = {
      count: 0,
      last: 0,
      gpuCount: 0,
      lastGpu: renderer.stats.gpuSamples,
      uploadBytes: 0,
      gap: new Float32Array(120000),
      cpu: new Float32Array(120000),
      submit: new Float32Array(120000),
      gpu: new Float32Array(120000),
    };
    return report();
  }
  function stopCapture() {
    const result = report();
    capture = null;
    return result;
  }
  function downloadReport() {
    const blob = new Blob([JSON.stringify(report(), null, 2)], {
        type: "application/json",
      }),
      url = URL.createObjectURL(blob),
      a = document.createElement("a");
    a.href = url;
    a.download = "boids-report.json";
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }
  B.Host.attach({
    settings: configure,
    pause(value) {
      hostPause = value;
      syncPause();
    },
    fps(value) {
      hostFps = Math.max(1, Number(value) || 60);
      lastDraw = 0;
      wake();
    },
  });
  canvas.addEventListener("pointermove", (e) => {
    sim.pointer.x = e.clientX;
    sim.pointer.y = e.clientY;
    sim.pointer.active = true;
    wake();
  });
  canvas.addEventListener("pointerleave", () => {
    sim.pointer.active = false;
  });
  canvas.addEventListener("pointerdown", (e) => {
    sim.click(e.clientX, e.clientY);
    wake();
  });
  addEventListener("resize", resize);
  addEventListener("focus", () => { if (updateWorkArea()) redraw(); });
  setInterval(() => { if (updateWorkArea()) redraw(); }, 2000);
  document.addEventListener("visibilitychange", syncPause);
  addEventListener("keydown", (e) => {
    if (
      e.code === "Space" &&
      !/INPUT|SELECT|TEXTAREA|BUTTON/.test(document.activeElement.tagName)
    ) {
      e.preventDefault();
      pause();
    }
  });
  window.boids = {
    get sim() {
      return sim;
    },
    get history() {
      return history;
    },
    renderer,
    get settings() {
      return { ...settings };
    },
    configure,
    pause,
    reset,
    quality(samples = 128) {
      return sim.neighbors.quality(sim, samples);
    },
    report,
    startCapture,
    stopCapture,
    get running() {
      return !!raf;
    },
  };
  resize();
  ui.status(
    renderer.available
      ? "WebGL2 · 本地实时模拟"
      : "WebGL2 不可用，请检查显卡加速设置",
  );
})((globalThis.Boids = globalThis.Boids || {}));
