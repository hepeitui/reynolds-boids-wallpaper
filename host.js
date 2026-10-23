(function (root) {
  "use strict";

  var Boids = (root.Boids = root.Boids || {});
  var callbacks = null;
  var pendingSettings = Object.create(null);
  var paused = false;
  var globalFps = null;
  var wallpaperDetected = Boolean(root.wallpaperEngine);
  var INVALID = {};

  function unwrap(value) {
    var depth = 0;
    while (
      value &&
      typeof value === "object" &&
      !Array.isArray(value) &&
      Object.prototype.hasOwnProperty.call(value, "value") &&
      depth < 4
    ) {
      value = value.value;
      depth += 1;
    }
    return value;
  }

  function parseBoolean(value) {
    value = unwrap(value);
    if (value === true || value === false) return value;
    if (typeof value === "number" && Number.isFinite(value)) return value !== 0;
    if (typeof value === "string") {
      var text = value.trim().toLowerCase();
      if (
        text === "true" ||
        text === "1" ||
        text === "yes" ||
        text === "on" ||
        text === "enabled"
      )
        return true;
      if (
        text === "false" ||
        text === "0" ||
        text === "no" ||
        text === "off" ||
        text === "disabled" ||
        text === ""
      )
        return false;
    }
    return INVALID;
  }

  function markWallpaper() {
    if (wallpaperDetected) return;
    wallpaperDetected = true;
    if (
      typeof root.dispatchEvent === "function" &&
      typeof root.Event === "function"
    ) {
      root.dispatchEvent(new root.Event("boids-host-detected"));
    }
  }

  function normalizeProperty(key, rawValue) {
    var value = unwrap(rawValue);
    if (key === "trails") {
      value = parseBoolean(value);
      if (value === INVALID) return INVALID;
    } else if (
      key === "color" &&
      Boids.Settings &&
      typeof Boids.Settings.normalizeColor === "function"
    ) {
      value = Boids.Settings.normalizeColor(value);
      if (typeof value !== "string") return INVALID;
    }
    return value;
  }

  function settingsPatch(properties) {
    var raw = Object.create(null);
    if (
      !properties ||
      typeof properties !== "object" ||
      Array.isArray(properties)
    )
      return {};
    Object.keys(properties).forEach(function (key) {
      var value = normalizeProperty(key, properties[key]);
      if (value !== INVALID) raw[key] = value;
    });
    if (Boids.Settings && typeof Boids.Settings.sanitizePatch === "function")
      return Boids.Settings.sanitizePatch(raw);
    return raw;
  }

  function applySettings(properties) {
    var patch = settingsPatch(properties);
    var keys = Object.keys(patch);
    if (!keys.length) return;
    if (!callbacks) {
      keys.forEach(function (key) {
        pendingSettings[key] = patch[key];
      });
      return;
    }
    if (typeof callbacks.settings === "function") callbacks.settings(patch);
  }

  function applyPause(value) {
    var next = parseBoolean(value);
    if (next === INVALID) return;
    paused = next;
    if (callbacks && typeof callbacks.pause === "function")
      callbacks.pause(paused);
  }

  function applyGlobalFps(value) {
    value = unwrap(value);
    var parsed =
      typeof value === "number"
        ? value
        : typeof value === "string" && value.trim() !== ""
          ? Number(value)
          : NaN;
    if (!Number.isFinite(parsed) || parsed < 0) return;
    globalFps = Math.min(240, parsed);
    if (callbacks && typeof callbacks.fps === "function")
      callbacks.fps(globalFps);
  }

  var api = {
    attach: function (nextCallbacks) {
      callbacks =
        nextCallbacks && typeof nextCallbacks === "object" ? nextCallbacks : {};
      if (root.wallpaperEngine) markWallpaper();
      var initial = settingsPatch(pendingSettings);
      pendingSettings = Object.create(null);
      if (
        Object.keys(initial).length &&
        typeof callbacks.settings === "function"
      )
        callbacks.settings(initial);
      if (typeof callbacks.pause === "function") callbacks.pause(paused);
      if (globalFps !== null && typeof callbacks.fps === "function")
        callbacks.fps(globalFps);
      var attachedCallbacks = callbacks;
      return function detach() {
        if (callbacks === attachedCallbacks) callbacks = null;
      };
    },
    get isWallpaper() {
      return wallpaperDetected;
    },
    get isPaused() {
      return paused;
    },
    get globalFps() {
      return globalFps;
    },
    workArea: function (width, height, enabled) {
      var area = { left: 0, top: 0, right: width, bottom: height };
      var screen = root.screen;
      // Ordinary browser previews are not display-sized wallpaper surfaces.
      if (!enabled || !screen ||
          Math.abs(screen.width - width) > 4 ||
          Math.abs(screen.height - height) > 4 ||
          !(screen.availWidth > 0 && screen.availHeight > 0))
        return area;
      var originX = Number.isFinite(screen.left) ? screen.left : root.screenX || 0;
      var originY = Number.isFinite(screen.top) ? screen.top : root.screenY || 0;
      var left = Number.isFinite(screen.availLeft) ? screen.availLeft - originX : 0;
      var top = Number.isFinite(screen.availTop) ? screen.availTop - originY : 0;
      area.left = Math.max(0, left);
      area.top = Math.max(0, top);
      area.right = Math.min(width, left + screen.availWidth);
      area.bottom = Math.min(height, top + screen.availHeight);
      // Some embedded hosts report a different monitor's work area.
      if (area.right <= area.left || area.bottom <= area.top)
        return { left: 0, top: 0, right: width, bottom: height };
      return area;
    },
  };

  Boids.Host = api;

  root.wallpaperPropertyListener = {
    applyUserProperties: function (properties) {
      markWallpaper();
      applySettings(properties);
    },
    applyGeneralProperties: function (properties) {
      markWallpaper();
      if (
        properties &&
        typeof properties === "object" &&
        Object.prototype.hasOwnProperty.call(properties, "fps")
      ) {
        applyGlobalFps(properties.fps);
      }
    },
    setPaused: function (value) {
      applyPause(value);
    },
  };
  if (wallpaperDetected) markWallpaper();
})(globalThis);
