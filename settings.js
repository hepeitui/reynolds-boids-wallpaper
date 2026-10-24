(function (root) {
  "use strict";

  var Boids = (root.Boids = root.Boids || {});
  var STORAGE_KEY = "reynolds-boids-wallpaper:settings:v1";
  var INVALID = {};

  var defaults = Object.freeze({
    count: 1500,
    size: 6.58,
    speed: 87,
    speedError: 0.07,
    separation: 1.38,
    alignment: 1,
    cohesion: 0.65,
    edge: 0.175,
    trails: true,
    trailDuration: 0.28,
    color: "#84d8e0",
    brightness: 1.31,
    contrast: 1.2,
    randomColor: false,
    avoidTaskbar: true,
    marginLeft: 0,
    marginRight: 0,
    marginTop: 0,
    marginBottom: 0,
    renderScale: 1,
    fps: 60,
    trailHz: 30,
  });

  var schema = Object.freeze({
    count: Object.freeze({
      kind: "number",
      label: "鸟群数量",
      description: "目标数量，变化时鸟群会逐渐进出画面。",
      min: 0,
      max: 3000,
      step: 1,
      unit: "只",
    }),
    size: Object.freeze({
      kind: "number",
      label: "鸟体大小",
      description: "以逻辑像素计。",
      min: 1,
      max: 16,
      step: 0.01,
      unit: "px",
    }),
    speed: Object.freeze({
      kind: "number",
      label: "飞行速度",
      description: "每秒移动的逻辑像素。",
      min: 20,
      max: 240,
      step: 1,
      unit: "px/s",
    }),
    speedError: Object.freeze({
      kind: "number",
      label: "个体速度差异",
      description: "每只鸟保持稳定的速度偏差。",
      min: 0,
      max: 0.5,
      step: 0.01,
      unit: "%",
    }),
    separation: Object.freeze({
      kind: "number",
      label: "分离",
      description: "避免彼此过近。",
      min: 0,
      max: 4,
      step: 0.01,
      unit: "×",
    }),
    alignment: Object.freeze({
      kind: "number",
      label: "对齐",
      description: "趋向附近鸟群的平均方向。",
      min: 0,
      max: 4,
      step: 0.01,
      unit: "×",
    }),
    cohesion: Object.freeze({
      kind: "number",
      label: "聚合",
      description: "趋向附近鸟群中心。",
      min: 0,
      max: 4,
      step: 0.01,
      unit: "×",
    }),
    edge: Object.freeze({
      kind: "number",
      label: "边缘回转区",
      description: "指数提前回转；仍触边则消失并补生。0 只关闭提前回转。",
      min: 0,
      max: 0.35,
      step: 0.001,
      unit: "%",
    }),
    trails: Object.freeze({
      kind: "boolean",
      label: "显示拖尾",
      description: "保留最近飞行轨迹；关闭时不清除历史。",
    }),
    trailDuration: Object.freeze({
      kind: "number",
      label: "拖尾时长",
      description: "从历史轨迹中显示的秒数。",
      min: 0.1,
      max: 0.5,
      step: 0.01,
      unit: "s",
    }),
    color: Object.freeze({
      kind: "color",
      label: "鸟群颜色",
      description: "即时更新鸟体与拖尾颜色。",
    }),
    brightness: Object.freeze({
      kind: "number",
      label: "亮度",
      description: "控制鸟群发光亮度，不改变颜色。",
      min: 0.1,
      max: 2,
      step: 0.01,
      unit: "×",
    }),
    contrast: Object.freeze({
      kind: "number",
      label: "对比度",
      description: "调整鸟体与拖尾的颜色对比度。",
      min: 0.5,
      max: 2,
      step: 0.01,
      unit: "×",
    }),
    randomColor: Object.freeze({
      kind: "boolean",
      label: "鸟群渐变配色",
      description: "按位置连续渐变，聚集的鸟颜色相近，拖尾沿轨迹平滑过渡。",
    }),
    avoidTaskbar: Object.freeze({
      kind: "boolean",
      label: "避开任务栏",
      description: "根据屏幕可用工作区避开任务栏；宿主未提供时请手动调四边距。",
    }),
    marginLeft: Object.freeze({
      kind: "number",
      label: "左边距",
      description: "从当前可用工作区左侧额外保留的距离。",
      min: 0,
      max: 200,
      step: 1,
      unit: "px",
    }),
    marginRight: Object.freeze({
      kind: "number",
      label: "右边距",
      description: "从当前可用工作区右侧额外保留的距离。",
      min: 0,
      max: 200,
      step: 1,
      unit: "px",
    }),
    marginTop: Object.freeze({
      kind: "number",
      label: "上边距",
      description: "从当前可用工作区顶部额外保留的距离。",
      min: 0,
      max: 200,
      step: 1,
      unit: "px",
    }),
    marginBottom: Object.freeze({
      kind: "number",
      label: "下边距",
      description: "从当前可用工作区底部额外保留的距离。",
      min: 0,
      max: 200,
      step: 1,
      unit: "px",
    }),
    renderScale: Object.freeze({
      kind: "number",
      label: "渲染比例",
      description: "降低内部绘制分辨率可减少 GPU 负载。",
      min: 0.5,
      max: 1,
      step: 0.01,
      unit: "%",
    }),
    fps: Object.freeze({
      kind: "number",
      label: "帧率上限",
      description: "与 Wallpaper Engine 全局限制共同生效。",
      min: 15,
      max: 120,
      step: 1,
      unit: "FPS",
    }),
    trailHz: Object.freeze({
      kind: "number",
      label: "拖尾采样率",
      description: "控制历史轨迹的采样密度。",
      min: 10,
      max: 60,
      step: 1,
      unit: "Hz",
    }),
  });

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
      var normalized = value.trim().toLowerCase();
      if (
        normalized === "true" ||
        normalized === "1" ||
        normalized === "yes" ||
        normalized === "on" ||
        normalized === "enabled"
      )
        return true;
      if (
        normalized === "false" ||
        normalized === "0" ||
        normalized === "no" ||
        normalized === "off" ||
        normalized === "disabled" ||
        normalized === ""
      )
        return false;
    }
    return INVALID;
  }

  function normalizeColor(value) {
    value = unwrap(value);
    if (typeof value === "string") {
      var text = value.trim();
      var hex = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(text);
      if (hex) {
        var digits = hex[1];
        if (digits.length === 3)
          digits =
            digits[0] +
            digits[0] +
            digits[1] +
            digits[1] +
            digits[2] +
            digits[2];
        return "#" + digits.toLowerCase();
      }
    }

    var channels;
    if (
      Array.isArray(value) ||
      (typeof ArrayBuffer !== "undefined" &&
        ArrayBuffer.isView &&
        ArrayBuffer.isView(value))
    ) {
      channels = [Number(value[0]), Number(value[1]), Number(value[2])];
    } else if (value && typeof value === "object") {
      channels = [
        Number(value.r !== undefined ? value.r : value.red),
        Number(value.g !== undefined ? value.g : value.green),
        Number(value.b !== undefined ? value.b : value.blue),
      ];
    } else if (typeof value === "string") {
      var parts = value.match(/[-+]?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?/gi);
      if (!parts || parts.length < 3) return INVALID;
      channels = [Number(parts[0]), Number(parts[1]), Number(parts[2])];
    } else {
      return INVALID;
    }

    if (
      !Number.isFinite(channels[0]) ||
      !Number.isFinite(channels[1]) ||
      !Number.isFinite(channels[2])
    )
      return INVALID;
    var normalizedFloats =
      channels[0] >= 0 &&
      channels[0] <= 1 &&
      channels[1] >= 0 &&
      channels[1] <= 1 &&
      channels[2] >= 0 &&
      channels[2] <= 1;
    var scale = normalizedFloats ? 255 : 1;
    var result = "#";
    for (var i = 0; i < 3; i += 1) {
      var channel = Math.round(Math.max(0, Math.min(255, channels[i] * scale)));
      result += channel.toString(16).padStart(2, "0");
    }
    return result;
  }

  function decimalPlaces(value) {
    var text = String(value);
    var point = text.indexOf(".");
    return point === -1 ? 0 : text.length - point - 1;
  }

  function sanitizeValue(key, raw, fallback) {
    var definition = schema[key];
    if (!definition) return fallback === undefined ? INVALID : fallback;
    var value = unwrap(raw);
    var safeFallback = fallback === undefined ? defaults[key] : fallback;

    if (definition.kind === "number") {
      var numeric =
        typeof value === "number"
          ? value
          : typeof value === "string" && value.trim() !== ""
            ? Number(value)
            : NaN;
      if (!Number.isFinite(numeric)) return safeFallback;
      var bounded = Math.max(definition.min, Math.min(definition.max, numeric));
      var step = definition.step;
      var quantized =
        definition.min + Math.round((bounded - definition.min) / step) * step;
      var precision = Math.min(
        12,
        Math.max(decimalPlaces(step), decimalPlaces(definition.min)),
      );
      quantized = Number(quantized.toFixed(precision));
      return Math.max(definition.min, Math.min(definition.max, quantized));
    }

    if (definition.kind === "boolean") {
      var booleanValue = parseBoolean(value);
      return booleanValue === INVALID ? safeFallback : booleanValue;
    }

    if (definition.kind === "color") {
      var color = normalizeColor(value);
      return color === INVALID ? safeFallback : color;
    }

    if (definition.kind === "enum") {
      for (var index = 0; index < definition.values.length; index += 1) {
        if (String(value) === String(definition.values[index]))
          return definition.values[index];
      }
      return safeFallback;
    }

    return safeFallback;
  }

  function sanitizePatch(patch) {
    var result = {};
    if (!patch || typeof patch !== "object" || Array.isArray(patch))
      return result;
    Object.keys(defaults).forEach(function (key) {
      if (!Object.prototype.hasOwnProperty.call(patch, key)) return;
      var value = unwrap(patch[key]);
      var definition = schema[key];
      var valid;
      if (definition.kind === "number") {
        valid =
          (typeof value === "number" && Number.isFinite(value)) ||
          (typeof value === "string" &&
            value.trim() !== "" &&
            Number.isFinite(Number(value)));
      } else if (definition.kind === "boolean") {
        valid = parseBoolean(value) !== INVALID;
      } else if (definition.kind === "color") {
        valid = normalizeColor(value) !== INVALID;
      } else {
        valid = definition.values.some(function (choice) {
          return String(value) === String(choice);
        });
      }
      if (valid) result[key] = sanitizeValue(key, value, defaults[key]);
    });
    return result;
  }

  function sanitize(settings) {
    var result = {};
    Object.keys(defaults).forEach(function (key) {
      var value =
        settings &&
        typeof settings === "object" &&
        Object.prototype.hasOwnProperty.call(settings, key)
          ? settings[key]
          : defaults[key];
      result[key] = sanitizeValue(key, value, defaults[key]);
    });
    return result;
  }

  function merge(current, patch) {
    var result = sanitize(current);
    var cleanPatch = sanitizePatch(patch);
    Object.keys(cleanPatch).forEach(function (key) {
      result[key] = cleanPatch[key];
    });
    return result;
  }

  function localStorageOrNull() {
    try {
      return root.localStorage || null;
    } catch (error) {
      return null;
    }
  }

  function load(storage) {
    var store = storage === undefined ? localStorageOrNull() : storage;
    if (!store || typeof store.getItem !== "function")
      return sanitize(defaults);
    try {
      var serialized = store.getItem(STORAGE_KEY);
      if (!serialized) return sanitize(defaults);
      var saved = JSON.parse(serialized);
      return merge(defaults, saved);
    } catch (error) {
      return sanitize(defaults);
    }
  }

  function save(settings, storage) {
    var store = storage === undefined ? localStorageOrNull() : storage;
    var next = merge(load(store), settings);
    if (!store || typeof store.setItem !== "function") return next;
    try {
      store.setItem(STORAGE_KEY, JSON.stringify(next));
    } catch (error) {
      /* Storage can be disabled or full. */
    }
    return next;
  }

  Boids.Settings = Object.freeze({
    key: STORAGE_KEY,
    defaults: defaults,
    schema: schema,
    normalizeColor: normalizeColor,
    sanitizeValue: sanitizeValue,
    sanitizePatch: sanitizePatch,
    sanitize: sanitize,
    merge: merge,
    load: load,
    save: save,
  });
})(globalThis);
