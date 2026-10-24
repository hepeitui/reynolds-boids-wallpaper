(function (root) {
  "use strict";

  var Boids = (root.Boids = root.Boids || {});
  var schema = Boids.Settings && Boids.Settings.schema;
  var FPS_KEYS = ["fps", "frameRate", "renderedFps"];
  var COUNT_KEYS = [
    "activeCount",
    "currentCount",
    "liveCount",
    "birdCount",
    "count",
    "birdInstances",
  ];
  var FRAME_KEYS = [
    "frameTimeMs",
    "frameMs",
    "frameDurationMs",
    "frameIntervalMs",
  ];
  var SIM_KEYS = ["simulationMs", "stepMs", "updateMs"];
  var RENDER_KEYS = ["renderMs", "drawMs"];
  var GPU_KEYS = ["gpuMs"];
  var METRIC_KEYS = ["fps", "count", "frame", "simulation", "render", "gpu"];

  function make(tag, className, text) {
    var element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== undefined) element.textContent = text;
    return element;
  }

  function finiteNumber(value) {
    if (typeof value !== "number" || !Number.isFinite(value) || value < 0)
      return null;
    return value;
  }

  function firstMetric(sources, keys) {
    for (var sourceIndex = 0; sourceIndex < sources.length; sourceIndex += 1) {
      var source = sources[sourceIndex];
      if (!source || typeof source !== "object") continue;
      for (var keyIndex = 0; keyIndex < keys.length; keyIndex += 1) {
        var key = keys[keyIndex];
        if (Object.prototype.hasOwnProperty.call(source, key)) {
          var value = finiteNumber(source[key]);
          if (value !== null) return value;
        }
      }
    }
    return null;
  }

  function formatMetric(value, digits) {
    if (value === null) return "—";
    return value.toFixed(digits);
  }

  function displayValue(key, value) {
    var definition = schema[key];
    var amount = Number(value);
    if (definition.unit === "%") amount *= 100;
    var formatted = String(
      Number(amount.toFixed(definition.unit === "%" ? 1 : 2)),
    );
    return formatted + (definition.unit || "");
  }

  function shouldHideControls() {
    var search =
      root.location && root.location.search ? root.location.search : "";
    var query = new URLSearchParams(search);
    if (query.get("settings") === "1") return false;
    if (query.get("desktop") === "1") return true;
    return Boolean(Boids.Host && Boids.Host.isWallpaper);
  }

  function isProblemStatus(text) {
    return /(不支持|不可用|失败|错误|丢失|error|failed|unsupported|unavailable|not\s+supported|not\s+available|context\s+lost|could\s+not|unable\s+to|lost)/i.test(
      text,
    );
  }

  function createUI(onChange, onPause, onReset, onExport) {
    if (!schema || !Boids.Settings)
      throw new Error("Boids.Settings must be loaded before Boids.createUI().");
    var controls = document.getElementById("controls");
    if (!controls) throw new Error("Missing #controls settings container.");

    var settings = Boids.Settings.sanitize(Boids.Settings.defaults);
    var paused = false;
    var controlsByKey = Object.create(null);
    var persistTimer = null;
    var panel = make("section", "settings-panel");
    panel.setAttribute("aria-label", "群鸟壁纸设置");
    var header = make("header", "settings-header");
    var titleBlock = make("div", "settings-title");
    titleBlock.appendChild(make("h1", "", "群飞设置"));
    titleBlock.appendChild(make("p", "", "实时画面与运行状态"));
    var collapseButton = make("button", "icon-button", "收起");
    collapseButton.type = "button";
    collapseButton.setAttribute("aria-label", "收起设置面板");
    collapseButton.setAttribute("aria-expanded", "true");
    header.appendChild(titleBlock);
    header.appendChild(collapseButton);
    panel.appendChild(header);

    var statusNote = make("p", "runtime-note", "正在初始化 WebGL2 画面…");
    statusNote.setAttribute("role", "status");
    statusNote.setAttribute("aria-live", "polite");
    panel.appendChild(statusNote);

    var stats = make("section", "runtime-metrics");
    stats.setAttribute("aria-label", "实时运行指标");
    var metrics = {};
    var metricSources = [null, null, null, null, null];
    var countSources = [null, null, null, null];
    var metricValues = {
      fps: null,
      count: null,
      frame: null,
      simulation: null,
      render: null,
      gpu: null,
    };
    [
      ["fps", "帧率", "FPS", 1],
      ["count", "当前鸟数", "只", 0],
      ["frame", "帧耗时", "ms", 2],
      ["simulation", "模拟耗时", "ms", 2],
      ["render", "绘制耗时", "ms", 2],
      ["gpu", "GPU 耗时", "ms", 2],
    ].forEach(function (entry) {
      var cell = make("div", "metric");
      var value = make("output", "metric-value", "—");
      value.setAttribute("aria-label", entry[1] + "：暂无测量");
      cell.appendChild(make("span", "metric-label", entry[1]));
      cell.appendChild(value);
      stats.appendChild(cell);
      metrics[entry[0]] = {
        output: value,
        label: entry[1],
        unit: entry[2],
        digits: entry[3],
      };
    });
    var metricsNote = make("p", "metrics-note", "— 表示运行时未提供该测量。");
    stats.appendChild(metricsNote);
    panel.appendChild(stats);

    var actionRow = make("div", "action-row");
    var pauseButton = make("button", "action-button action-primary", "暂停");
    pauseButton.type = "button";
    pauseButton.setAttribute("aria-pressed", "false");
    var resetButton = make("button", "action-button", "重置鸟群");
    resetButton.type = "button";
    var exportButton = make("button", "action-button", "导出设置");
    exportButton.type = "button";
    actionRow.appendChild(pauseButton);
    actionRow.appendChild(resetButton);
    actionRow.appendChild(exportButton);
    panel.appendChild(actionRow);

    var form = make("div", "settings-content");
    form.id = "settings-content";
    panel.appendChild(form);

    var restoreButton = make("button", "restore-controls", "打开设置");
    restoreButton.type = "button";
    restoreButton.setAttribute("aria-label", "展开鸟群设置");
    restoreButton.setAttribute("aria-expanded", "false");
    restoreButton.hidden = true;
    controls.classList.add("controls-shell");
    controls.replaceChildren(panel, restoreButton);

    var statusOverlay = document.getElementById("runtime-status");
    if (!statusOverlay) {
      statusOverlay = make("div", "runtime-status", "");
      statusOverlay.id = "runtime-status";
      statusOverlay.setAttribute("role", "status");
      statusOverlay.setAttribute("aria-live", "assertive");
      statusOverlay.hidden = true;
      document.body.appendChild(statusOverlay);
    }

    function fieldGroup(key, section) {
      var definition = schema[key];
      var field = make("div", "setting-field");
      var labelId = "setting-label-" + key;
      var descriptionId = "setting-description-" + key;
      var label = make("label", "setting-label", definition.label);
      label.id = labelId;
      var description = make(
        "p",
        "setting-description",
        definition.description,
      );
      description.id = descriptionId;
      field.appendChild(label);
      field.appendChild(description);

      if (definition.kind === "number") {
        var pair = make("div", "control-pair");
        var range = document.createElement("input");
        range.type = "range";
        range.id = "setting-" + key;
        range.min = String(definition.min);
        range.max = String(definition.max);
        range.step = String(definition.step);
        range.setAttribute("aria-describedby", descriptionId);
        range.setAttribute(
          "aria-valuetext",
          definition.label + " " + displayValue(key, settings[key]),
        );
        label.htmlFor = range.id;

        var number = document.createElement("input");
        number.type = "number";
        number.className = "number-input";
        number.min = String(definition.min);
        number.max = String(definition.max);
        number.step = String(definition.step);
        number.inputMode = "decimal";
        number.setAttribute("aria-label", definition.label + "数值");
        number.setAttribute("aria-describedby", descriptionId);
        pair.appendChild(range);
        pair.appendChild(number);
        field.appendChild(pair);
        var shownValue = make(
          "output",
          "setting-value",
          displayValue(key, settings[key]),
        );
        shownValue.setAttribute("aria-live", "off");
        field.appendChild(shownValue);

        controlsByKey[key] = {
          kind: "number",
          range: range,
          input: number,
          output: shownValue,
        };
        range.addEventListener("input", function () {
          commit(key, range.value);
        });
        number.addEventListener("change", function () {
          commit(key, number.value);
        });
        number.addEventListener("keydown", function (event) {
          if (event.key === "Enter") {
            event.preventDefault();
            commit(key, number.value);
          }
        });
      } else if (definition.kind === "boolean") {
        var toggleRow = make("label", "toggle-row");
        var toggleLabel = make("span", "toggle-label", "启用");
        var toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.id = "setting-" + key;
        toggle.setAttribute("aria-describedby", descriptionId);
        toggleRow.htmlFor = toggle.id;
        toggleRow.appendChild(toggleLabel);
        toggleRow.appendChild(toggle);
        field.appendChild(toggleRow);
        controlsByKey[key] = { kind: "boolean", input: toggle };
        toggle.addEventListener("change", function () {
          commit(key, toggle.checked);
        });
      } else if (definition.kind === "color") {
        var colorInput = document.createElement("input");
        colorInput.type = "color";
        colorInput.id = "setting-" + key;
        colorInput.className = "color-input";
        colorInput.setAttribute("aria-describedby", descriptionId);
        label.htmlFor = colorInput.id;
        field.appendChild(colorInput);
        controlsByKey[key] = { kind: "color", input: colorInput };
        colorInput.addEventListener("input", function () {
          commit(key, colorInput.value);
        });
      } else if (definition.kind === "enum") {
        var select = document.createElement("select");
        select.id = "setting-" + key;
        select.className = "select-input";
        select.setAttribute("aria-describedby", descriptionId);
        label.htmlFor = select.id;
        Object.keys(definition.options).forEach(function (value) {
          var option = document.createElement("option");
          option.value = value;
          option.textContent = definition.options[value];
          select.appendChild(option);
        });
        field.appendChild(select);
        controlsByKey[key] = { kind: "enum", input: select };
        select.addEventListener("change", function () {
          commit(key, select.value);
        });
      }
      section.appendChild(field);
    }

    function addSection(title, keys) {
      var section = make("section", "settings-section");
      section.appendChild(make("h2", "section-title", title));
      keys.forEach(function (key) {
        fieldGroup(key, section);
      });
      form.appendChild(section);
      return section;
    }

    function addAdvanced(title, description, keys) {
      var details = make("details", "advanced-section");
      var summary = make("summary", "advanced-summary", title);
      details.appendChild(summary);
      details.appendChild(make("p", "advanced-description", description));
      keys.forEach(function (key) {
        fieldGroup(key, details);
      });
      form.appendChild(details);
    }

    addSection("鸟群运动", ["count", "size", "speed", "speedError"]);
    addSection("群体规则", ["separation", "alignment", "cohesion", "edge"]);
    addSection("颜色与亮度", [
      "color",
      "brightness",
      "contrast",
      "randomColor",
    ]);
    addSection("飞行拖尾", ["trails", "trailDuration", "trailHz"]);
    addSection("活动区域", [
      "avoidTaskbar",
      "marginTop",
      "marginBottom",
      "marginLeft",
      "marginRight",
    ]);
    addAdvanced("渲染性能", "帧率限制同时受 Wallpaper Engine 全局上限影响。", [
      "renderScale",
      "fps",
    ]);

    function updateControl(key) {
      var control = controlsByKey[key];
      if (!control) return;
      var value = settings[key];
      if (control.kind === "number") {
        control.range.value = String(value);
        control.input.value = String(value);
        control.range.setAttribute(
          "aria-valuetext",
          schema[key].label + " " + displayValue(key, value),
        );
        control.output.textContent = displayValue(key, value);
      } else if (control.kind === "boolean") {
        control.input.checked = value;
      } else {
        control.input.value = String(value);
      }
    }

    function updateAllControls() {
      Object.keys(controlsByKey).forEach(updateControl);
    }

    function scheduleSave() {
      if (persistTimer !== null) root.clearTimeout(persistTimer);
      persistTimer = root.setTimeout(function () {
        persistTimer = null;
        Boids.Settings.save(settings);
      }, 120);
    }

    function flushSave() {
      if (persistTimer !== null) {
        root.clearTimeout(persistTimer);
        persistTimer = null;
      }
      Boids.Settings.save(settings);
    }

    function commit(key, value) {
      var patch = {};
      patch[key] = value;
      var cleanPatch = Boids.Settings.sanitizePatch(patch);
      if (!Object.prototype.hasOwnProperty.call(cleanPatch, key)) {
        updateControl(key);
        return;
      }
      settings = Boids.Settings.merge(settings, cleanPatch);
      updateControl(key);
      scheduleSave();
      if (typeof onChange === "function") onChange(cleanPatch);
    }

    function setPaused(value) {
      var normalized = Boids.Settings.sanitizeValue("trails", value, false);
      paused = normalized;
      pauseButton.textContent = paused ? "继续" : "暂停";
      pauseButton.setAttribute("aria-label", paused ? "继续播放" : "暂停播放");
      pauseButton.setAttribute("aria-pressed", paused ? "true" : "false");
    }

    function syncVisibility() {
      controls.hidden = shouldHideControls();
      controls.setAttribute("aria-hidden", controls.hidden ? "true" : "false");
    }

    function updateMetrics(source) {
      metricSources[0] = source;
      metricSources[1] = source && source.timing;
      metricSources[2] = source && source.simulation;
      metricSources[3] = source && source.sim;
      metricSources[4] = source && source.renderer;
      countSources[0] = source && source.simulation;
      countSources[1] = source && source.sim;
      countSources[2] = source;
      countSources[3] = source && source.renderer;
      metricValues.fps = firstMetric(metricSources, FPS_KEYS);
      metricValues.count = firstMetric(countSources, COUNT_KEYS);
      metricValues.frame = firstMetric(metricSources, FRAME_KEYS);
      metricValues.simulation = firstMetric(metricSources, SIM_KEYS);
      metricValues.render = firstMetric(metricSources, RENDER_KEYS);
      metricValues.gpu = firstMetric(metricSources, GPU_KEYS);
      for (var index = 0; index < METRIC_KEYS.length; index += 1) {
        var key = METRIC_KEYS[index];
        var metric = metrics[key];
        var value = metricValues[key];
        var text = formatMetric(value, metric.digits);
        metric.output.textContent =
          value === null ? "—" : text + " " + metric.unit;
        metric.output.setAttribute(
          "aria-label",
          metric.label +
            "：" +
            (value === null ? "暂无测量" : text + " " + metric.unit),
        );
      }
    }

    collapseButton.addEventListener("click", function () {
      panel.hidden = true;
      restoreButton.hidden = false;
      collapseButton.setAttribute("aria-expanded", "false");
      restoreButton.setAttribute("aria-expanded", "false");
      controls.setAttribute("data-collapsed", "true");
      restoreButton.focus();
    });
    restoreButton.addEventListener("click", function () {
      panel.hidden = false;
      restoreButton.hidden = true;
      collapseButton.setAttribute("aria-expanded", "true");
      restoreButton.setAttribute("aria-expanded", "true");
      controls.setAttribute("data-collapsed", "false");
      collapseButton.focus();
    });
    pauseButton.disabled = typeof onPause !== "function";
    pauseButton.addEventListener("click", function () {
      var next = !paused;
      setPaused(next);
      if (typeof onPause === "function") onPause(next);
    });
    resetButton.disabled = typeof onReset !== "function";
    resetButton.addEventListener("click", function () {
      if (typeof onReset === "function") onReset();
    });
    exportButton.addEventListener("click", function () {
      var snapshot = Boids.Settings.sanitize(settings);
      if (typeof onExport === "function") {
        onExport(snapshot);
        return;
      }
      var blob = new Blob(
        [JSON.stringify({ version: 1, settings: snapshot }, null, 2)],
        { type: "application/json" },
      );
      var link = make("a");
      link.href = URL.createObjectURL(blob);
      link.download = "boids-settings.json";
      link.click();
      root.setTimeout(function () {
        URL.revokeObjectURL(link.href);
      }, 0);
      status("设置文件已导出。");
    });

    function status(text) {
      var message = text === undefined || text === null ? "" : String(text);
      statusNote.textContent = message;
      if (isProblemStatus(message)) {
        statusOverlay.textContent = "画面状态：" + message;
        statusOverlay.hidden = false;
      } else {
        statusOverlay.textContent = "";
        statusOverlay.hidden = true;
      }
    }

    if (typeof root.addEventListener === "function")
      root.addEventListener("boids-host-detected", syncVisibility);
    if (typeof root.addEventListener === "function")
      root.addEventListener("pagehide", flushSave);
    if (typeof document.addEventListener === "function") {
      document.addEventListener("visibilitychange", function () {
        if (document.visibilityState === "hidden") flushSave();
      });
    }
    updateAllControls();
    setPaused(false);
    syncVisibility();

    return {
      sync: function (nextSettings) {
        settings = Boids.Settings.merge(settings, nextSettings);
        updateAllControls();
      },
      status: status,
      metrics: updateMetrics,
      setPaused: setPaused,
    };
  }

  Boids.createUI = createUI;
})(globalThis);
