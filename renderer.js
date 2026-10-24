(function (root) {
  "use strict";

  var Boids = root.Boids || (root.Boids = {});
  var MAX_BIRDS = 3000;
  var HISTORY_RING_LENGTH = 156;
  var MAX_TRAIL_SEGMENTS = 9000 * HISTORY_RING_LENGTH;
  var MAX_TIMER_QUERIES = 4;
  var TWO_PI = Math.PI * 2;
  var CONTEXT_ATTRIBUTES = {
    alpha: false,
    antialias: false,
    depth: false,
    stencil: false,
    premultipliedAlpha: false,
    preserveDrawingBuffer: false,
    powerPreference: "high-performance",
  };

  // Unit bird mesh facing +X: a forked tail, two swept wings and a bright body.
  // Each vertex is (local x, local y, luminance, wing flag).
  var BIRD_MESH = new Float32Array([
    // Tail feathers.
    -0.34, -0.035, 0.4, 0, -0.88, -0.22, 0.28, 0, -0.58, -0.01, 0.36, 0, -0.34,
    0.035, 0.4, 0, -0.58, 0.01, 0.36, 0, -0.88, 0.22, 0.28, 0,
    // Left wing.
    0.12, -0.045, 0.66, 1, -0.38, -0.96, 0.59, 1, -0.62, -0.52, 0.56, 1, 0.12,
    -0.045, 0.66, 1, -0.62, -0.52, 0.56, 1, -0.32, -0.16, 0.63, 1,
    // Right wing.
    0.12, 0.045, 0.66, 1, -0.62, 0.52, 0.56, 1, -0.38, 0.96, 0.59, 1, 0.12,
    0.045, 0.66, 1, -0.32, 0.16, 0.63, 1, -0.62, 0.52, 0.56, 1,
    // Tapered body and pointed head.
    -0.42, -0.06, 0.86, 0, 0.38, -0.025, 0.98, 0, -0.15, 0.0, 0.91, 0, -0.15,
    0.0, 0.91, 0, 0.38, -0.025, 0.98, 0, 0.64, 0.0, 1.0, 0, -0.42, 0.06, 0.86,
    0, -0.15, 0.0, 0.91, 0, 0.38, 0.025, 0.98, 0, -0.15, 0.0, 0.91, 0, 0.38,
    0.025, 0.98, 0, 0.64, 0.0, 1.0, 0,
  ]);
  var TRAIL_CORNERS = new Float32Array([0, -1, 0, 1, 1, -1, 1, -1, 0, 1, 1, 1]);
  var DEFAULT_COLOR = "#b9edf2";


  var BIRD_VERTEX_SHADER = `#version 300 es
precision highp float;
layout(location = 0) in vec2 a_local;
layout(location = 1) in float a_luminance;
layout(location = 2) in float a_wing;
layout(location = 3) in vec2 i_center;
layout(location = 4) in vec2 i_velocity;
layout(location = 5) in float i_phase;
layout(location = 6) in float i_deathProgress;
layout(location = 7) in float i_hue;
uniform vec2 u_viewport;
uniform float u_size;
uniform float u_flapPhase;
out float v_luminance;
out float v_depth;
out float v_deathProgress;
out float v_hue;
void main() {
  vec2 direction = i_velocity;
  float speed = length(direction);
  direction = speed > 0.0001 ? direction / speed : vec2(1.0, 0.0);

  float beat = u_flapPhase + i_phase;
  float flap = sin(beat);
  float dying = step(0.0, i_deathProgress);
  float progress = clamp(i_deathProgress, 0.0, 1.0);
  float unfold = 0.64 + 0.52 * smoothstep(0.0, 0.18, progress);
  float shrink = 1.0 - smoothstep(0.32, 1.0, progress);
  vec2 local = a_local;
  local.y *= mix(1.0, 0.84 + 0.12 * flap, a_wing);
  local.x += a_wing * abs(a_local.y) * 0.055 * cos(beat);
  local.y *= mix(1.0, 1.28, dying * smoothstep(0.0, 0.18, progress) * a_wing);

  // A restrained scale/light variation suggests depth without a 3-D scene.
  float depth = 0.91 + 0.12 * (0.5 + 0.5 * sin(i_phase * 1.73 + 0.8));
  local *= u_size * depth * mix(1.0, unfold * shrink, dying);
  vec2 oriented = vec2(
    local.x * direction.x - local.y * direction.y,
    local.x * direction.y + local.y * direction.x
  );
  vec2 point = i_center + oriented;
  vec2 clip = vec2(point.x / u_viewport.x * 2.0 - 1.0,
                   1.0 - point.y / u_viewport.y * 2.0);
  gl_Position = vec4(clip, 0.0, 1.0);
  v_luminance = a_luminance;
  v_depth = depth;
  v_deathProgress = i_deathProgress;
  v_hue = i_hue;
}`;

  var BIRD_FRAGMENT_SHADER = `#version 300 es
precision highp float;
in float v_luminance;
in float v_depth;
in float v_deathProgress;
in float v_hue;
uniform vec3 u_color;
uniform float u_brightness;
uniform float u_contrast;
uniform bool u_randomColor;
out vec4 fragColor;
vec3 brightHue(float hue) {
  vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
  return mix(vec3(1.0), rgb, 0.82);
}
void main() {
  vec3 base = u_randomColor ? brightHue(v_hue) : u_color;
  float tone = (v_luminance - 0.64) * 1.5;
  vec3 feather = base * clamp(1.0 + tone * u_contrast, 0.38, 1.0) * v_depth;
  float death = step(0.0, v_deathProgress);
  float flash = 1.0 - smoothstep(0.02, 0.32, clamp(v_deathProgress, 0.0, 1.0));
  feather = mix(feather, vec3(0.96, 1.0, 1.0), death * flash * 0.48);
  float fade = 1.0 - smoothstep(0.35, 1.0, clamp(v_deathProgress, 0.0, 1.0));
  float alpha = mix(0.96, 0.96 * fade, death);
  fragColor = vec4(feather * u_brightness, alpha);
}`;
  var TRAIL_VERTEX_SHADER = `#version 300 es
precision highp float;
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_start;
layout(location = 2) in vec4 a_end;
uniform vec2 u_viewport;
uniform float u_now;
uniform float u_duration;
uniform float u_width;
uniform vec2 u_pixelSize;
out float v_fade;
out float v_distance;
out float v_halfWidth;
out float v_hue;
void main() {
  float cutoff = u_now - u_duration;
  float delta = a_end.z - a_start.z;
  bool valid = delta > 0.000001 && a_end.z > cutoff && a_start.z < u_now;
  vec2 from = a_start.xy;
  vec2 to = a_end.xy;
  float visibleFrom = max(a_start.z, cutoff);
  float visibleTo = min(a_end.z, u_now);
  if (valid) {
    float invDelta = 1.0 / delta;
    from = mix(a_start.xy, a_end.xy, clamp((visibleFrom - a_start.z) * invDelta, 0.0, 1.0));
    to = mix(a_start.xy, a_end.xy, clamp((visibleTo - a_start.z) * invDelta, 0.0, 1.0));
  }
  float along = a_corner.x;
  float t = mix(visibleFrom, visibleTo, along);
  float hueDelta = mod(a_end.w - a_start.w + 0.5, 1.0) - 0.5;
  float hueAlong = delta > 0.000001
    ? clamp((t - a_start.z) / delta, 0.0, 1.0)
    : along;
  float age = clamp((t - cutoff) / u_duration, 0.0, 1.0);
  float fade = smoothstep(0.0, 0.38, age);
  vec2 tangent = to - from;
  float tangentLength = length(tangent);
  vec2 normal = tangentLength > 0.0001
    ? vec2(-tangent.y, tangent.x) / tangentLength
    : vec2(0.0, 1.0);
  float taper = 0.18 + 0.82 * fade;
  float halfWidth = u_width * taper * 0.5;
  // Rasterize the filter footprint too: a subpixel-width quad alone can miss every pixel centre.
  float padding = dot(abs(normal), u_pixelSize) * 0.5 + 0.001;
  float extent = halfWidth + padding;
  vec2 point = mix(from, to, along) + normal * (a_corner.y * extent);
  vec2 clip = vec2(point.x / u_viewport.x * 2.0 - 1.0,
                   1.0 - point.y / u_viewport.y * 2.0);
  gl_Position = valid ? vec4(clip, 0.0, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
  v_fade = valid ? fade : 0.0;
  v_distance = a_corner.y * extent;
  v_halfWidth = halfWidth;
  v_hue = a_start.w + hueDelta * hueAlong;
}`;

  var TRAIL_FRAGMENT_SHADER = `#version 300 es
precision highp float;
in float v_fade;
in float v_distance;
in float v_halfWidth;
in float v_hue;
uniform vec3 u_color;
uniform float u_brightness;
uniform float u_contrast;
uniform bool u_randomColor;
out vec4 fragColor;
vec3 brightHue(float hue) {
  vec3 rgb = clamp(abs(mod(hue * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
  return mix(vec3(1.0), rgb, 0.82);
}
void main() {
  float footprint = max(fwidth(v_distance), 0.0001);
  float overlap = min(v_halfWidth, v_distance + footprint * 0.5)
                - max(-v_halfWidth, v_distance - footprint * 0.5);
  float coverage = max(overlap, 0.0) / footprint;
  float alpha = clamp(0.78 * u_brightness * v_fade * coverage, 0.0, 0.92);
  if (alpha <= 0.001) discard;
  vec3 base = u_randomColor ? brightHue(v_hue) : u_color;
  float tone = mix(0.25, 1.0, v_fade);
  float shade = clamp(1.0 + (tone - 1.0) * u_contrast, 0.4, 1.0);
  fragColor = vec4(base * u_brightness * shade, alpha);
}`;

  function clamp(value, low, high) {
    return value < low ? low : value > high ? high : value;
  }

  function readHexDigit(code) {
    if (code >= 48 && code <= 57) return code - 48;
    if (code >= 65 && code <= 70) return code - 55;
    if (code >= 97 && code <= 102) return code - 87;
    return -1;
  }

  function setColorFromHex(value, target) {
    if (
      typeof value !== "string" ||
      value.length < 4 ||
      value.charCodeAt(0) !== 35
    )
      return false;
    var r;
    var g;
    var b;
    if (value.length === 4) {
      r = readHexDigit(value.charCodeAt(1));
      g = readHexDigit(value.charCodeAt(2));
      b = readHexDigit(value.charCodeAt(3));
      if (r < 0 || g < 0 || b < 0) return false;
      target[0] = (r * 17) / 255;
      target[1] = (g * 17) / 255;
      target[2] = (b * 17) / 255;
      return true;
    }
    if (value.length !== 7) return false;
    var r0 = readHexDigit(value.charCodeAt(1));
    var r1 = readHexDigit(value.charCodeAt(2));
    var g0 = readHexDigit(value.charCodeAt(3));
    var g1 = readHexDigit(value.charCodeAt(4));
    var b0 = readHexDigit(value.charCodeAt(5));
    var b1 = readHexDigit(value.charCodeAt(6));
    if (r0 < 0 || r1 < 0 || g0 < 0 || g1 < 0 || b0 < 0 || b1 < 0) return false;
    r = r0 * 16 + r1;
    g = g0 * 16 + g1;
    b = b0 * 16 + b1;
    target[0] = r / 255;
    target[1] = g / 255;
    target[2] = b / 255;
    return true;
  }

  function compileShader(gl, type, source, label) {
    var shader = gl.createShader(type);
    if (!shader) throw new Error("Unable to allocate " + label + " shader.");
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      var log =
        gl.getShaderInfoLog(shader) ||
        "No shader compiler details were provided.";
      gl.deleteShader(shader);
      throw new Error(label + " shader compilation failed: " + log);
    }
    return shader;
  }

  function createProgram(gl, vertexSource, fragmentSource, label) {
    var vertex = compileShader(
      gl,
      gl.VERTEX_SHADER,
      vertexSource,
      label + " vertex",
    );
    var fragment;
    try {
      fragment = compileShader(
        gl,
        gl.FRAGMENT_SHADER,
        fragmentSource,
        label + " fragment",
      );
    } catch (error) {
      gl.deleteShader(vertex);
      throw error;
    }
    var program = gl.createProgram();
    if (!program) {
      gl.deleteShader(vertex);
      gl.deleteShader(fragment);
      throw new Error("Unable to allocate " + label + " program.");
    }
    gl.attachShader(program, vertex);
    gl.attachShader(program, fragment);
    gl.linkProgram(program);
    var linked = gl.getProgramParameter(program, gl.LINK_STATUS);
    var log = linked
      ? ""
      : gl.getProgramInfoLog(program) ||
        "No program linker details were provided.";
    gl.detachShader(program, vertex);
    gl.detachShader(program, fragment);
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    if (!linked) {
      gl.deleteProgram(program);
      throw new Error(label + " program linking failed: " + log);
    }
    return program;
  }

  function requiredUniform(gl, program, name, label) {
    var location = gl.getUniformLocation(program, name);
    if (location === null)
      throw new Error("Required " + label + " uniform was not linked: " + name);
    return location;
  }

  function phaseFor(slot, generation) {
    var hash =
      (Math.imul(slot + 1, 374761393) +
        Math.imul(generation + 1, 668265263)) >>>
      0;
    hash = Math.imul(hash ^ (hash >>> 13), 1274126177) >>> 0;
    hash ^= hash >>> 16;
    return ((hash >>> 0) / 4294967296) * TWO_PI;
  }

  function Renderer(canvas, onStatus) {
    if (!canvas || typeof canvas.getContext !== "function") {
      throw new TypeError("Boids.Renderer requires a WebGL-capable canvas.");
    }
    this.canvas = canvas;
    this.onStatus = typeof onStatus === "function" ? onStatus : null;
    this.available = false;
    this.disposed = false;
    this.contextLost = false;
    this.gl = null;
    this._width = Math.max(1, canvas.clientWidth || canvas.width || 1);
    this._height = Math.max(1, canvas.clientHeight || canvas.height || 1);
    this._canvasLeft = 0;
    this._canvasTop = 0;
    this._canvasCssWidth = this._width;
    this._canvasCssHeight = this._height;
    this._dpr = root.devicePixelRatio || 1;
    this._scale = 1;
    this._birdData = null;
    this._trailData = null;
    this._phases = null;
    this._phaseGeneration = null;
    this._color = new Float32Array([185 / 255, 237 / 255, 242 / 255]);
    this._trailSegmentCount = 0;
    this._trailNow = 0;
    this._trailDuration = 1.2;
    this._trailHz = 30;
    this._cachedHistory = null;
    this._cachedHistoryRevision = -1;
    this._cachedHistoryDuration = 0;
    this._cachedHistoryHz = 0;
    this._cachedHistoricalCount = 0;
    this._birdProgram = null;
    this._trailProgram = null;
    this._birdVao = null;
    this._trailVao = null;
    this._birdMeshBuffer = null;
    this._birdInstanceBuffer = null;
    this._trailCornerBuffer = null;
    this._trailSegmentBuffer = null;
    this._birdUniforms = null;
    this._trailUniforms = null;
    this._timerExtension = null;
    this._timerSlots = null;
    this._timerCursor = 0;
    this._activeTimerIndex = -1;
    this._stats = {
      drawCalls: 0,
      triangles: 0,
      uploadBytes: 0,
      birdInstances: 0,
      dyingInstances: 0,
      trailSegments: 0,
      gpuMs: null,
      gpuSamples: 0,
    };
    this.stats = this._stats;

    this._trailCallback = this._appendTrail.bind(this);
    this._handleContextLost = this._onContextLost.bind(this);
    this._handleContextRestored = this._onContextRestored.bind(this);
    canvas.addEventListener("webglcontextlost", this._handleContextLost, false);
    canvas.addEventListener(
      "webglcontextrestored",
      this._handleContextRestored,
      false,
    );

    var gl = canvas.getContext("webgl2", CONTEXT_ATTRIBUTES);
    if (!gl) {
      this._status(
        "WebGL2 is unavailable. This wallpaper requires a browser with WebGL2 support.",
      );
      return;
    }
    this._initializeGL(gl);
    this.resize(this._width, this._height, this._dpr, this._scale);
    this._status("WebGL2 renderer ready.");
  }

  Renderer.prototype._status = function (message) {
    if (this.onStatus) this.onStatus(message);
  };

  Renderer.prototype._ensureCpuBuffers = function () {
    if (this._birdData) return;
    this._birdData = new Float32Array(MAX_BIRDS * 7);
    this._trailData = new Float32Array(MAX_TRAIL_SEGMENTS * 8);
    this._phases = new Float32Array(MAX_BIRDS);
    this._phaseGeneration = new Uint32Array(MAX_BIRDS);
    for (var i = 0; i < MAX_BIRDS; i += 1) {
      this._phases[i] = phaseFor(i, 0);
    }

  };
  Renderer.prototype._initializeGL = function (gl) {
    if (!gl) {
      this.available = false;
      this._status("WebGL2 could not be restored.");
      return;
    }
    this._ensureCpuBuffers();
    this.gl = gl;
    this._cachedHistory = null;
    this._cachedHistoryRevision = -1;
    try {
      this._birdProgram = createProgram(
        gl,
        BIRD_VERTEX_SHADER,
        BIRD_FRAGMENT_SHADER,
        "Bird",
      );
      this._trailProgram = createProgram(
        gl,
        TRAIL_VERTEX_SHADER,
        TRAIL_FRAGMENT_SHADER,
        "Trail",
      );

      this._birdUniforms = {
        viewport: requiredUniform(gl, this._birdProgram, "u_viewport", "bird"),
        size: requiredUniform(gl, this._birdProgram, "u_size", "bird"),
        phase: requiredUniform(gl, this._birdProgram, "u_flapPhase", "bird"),
        color: requiredUniform(gl, this._birdProgram, "u_color", "bird"),
        brightness: requiredUniform(gl, this._birdProgram, "u_brightness", "bird"),
        contrast: requiredUniform(gl, this._birdProgram, "u_contrast", "bird"),
        randomColor: requiredUniform(gl, this._birdProgram, "u_randomColor", "bird"),
      };
      this._trailUniforms = {
        viewport: requiredUniform(gl, this._trailProgram, "u_viewport", "trail"),
        now: requiredUniform(gl, this._trailProgram, "u_now", "trail"),
        duration: requiredUniform(gl, this._trailProgram, "u_duration", "trail"),
        width: requiredUniform(gl, this._trailProgram, "u_width", "trail"),
        pixelSize: requiredUniform(gl, this._trailProgram, "u_pixelSize", "trail"),
        color: requiredUniform(gl, this._trailProgram, "u_color", "trail"),
        brightness: requiredUniform(gl, this._trailProgram, "u_brightness", "trail"),
        contrast: requiredUniform(gl, this._trailProgram, "u_contrast", "trail"),
        randomColor: requiredUniform(gl, this._trailProgram, "u_randomColor", "trail"),
      };

      this._birdVao = gl.createVertexArray();
      this._trailVao = gl.createVertexArray();
      this._birdMeshBuffer = gl.createBuffer();
      this._birdInstanceBuffer = gl.createBuffer();
      this._trailCornerBuffer = gl.createBuffer();
      this._trailSegmentBuffer = gl.createBuffer();
      if (
        !this._birdVao ||
        !this._trailVao ||
        !this._birdMeshBuffer ||
        !this._birdInstanceBuffer ||
        !this._trailCornerBuffer ||
        !this._trailSegmentBuffer
      ) {
        throw new Error("Unable to allocate WebGL2 vertex arrays or buffers.");
      }

      gl.bindVertexArray(this._birdVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, this._birdMeshBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, BIRD_MESH, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 1, gl.FLOAT, false, 16, 8);
      gl.enableVertexAttribArray(2);
      gl.vertexAttribPointer(2, 1, gl.FLOAT, false, 16, 12);
      gl.bindBuffer(gl.ARRAY_BUFFER, this._birdInstanceBuffer);
      gl.bufferData(
        gl.ARRAY_BUFFER,
        this._birdData.byteLength,
        gl.DYNAMIC_DRAW,
      );
      gl.enableVertexAttribArray(3);
      gl.vertexAttribPointer(3, 2, gl.FLOAT, false, 28, 0);
      gl.vertexAttribDivisor(3, 1);
      gl.enableVertexAttribArray(4);
      gl.vertexAttribPointer(4, 2, gl.FLOAT, false, 28, 8);
      gl.vertexAttribDivisor(4, 1);
      gl.enableVertexAttribArray(5);
      gl.vertexAttribPointer(5, 1, gl.FLOAT, false, 28, 16);
      gl.vertexAttribDivisor(5, 1);
      gl.enableVertexAttribArray(6);
      gl.vertexAttribPointer(6, 1, gl.FLOAT, false, 28, 20);
      gl.vertexAttribDivisor(6, 1);
      gl.enableVertexAttribArray(7);
      gl.vertexAttribPointer(7, 1, gl.FLOAT, false, 28, 24);
      gl.vertexAttribDivisor(7, 1);

      gl.bindVertexArray(this._trailVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, this._trailCornerBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, TRAIL_CORNERS, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, this._trailSegmentBuffer);
      gl.bufferData(
        gl.ARRAY_BUFFER,
        this._trailData.byteLength,
        gl.DYNAMIC_DRAW,
      );
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 32, 0);
      gl.vertexAttribDivisor(1, 1);
      gl.enableVertexAttribArray(2);
      gl.vertexAttribPointer(2, 4, gl.FLOAT, false, 32, 16);
      gl.vertexAttribDivisor(2, 1);
      gl.bindVertexArray(null);
      gl.bindBuffer(gl.ARRAY_BUFFER, null);

      gl.clearColor(0.012, 0.034, 0.07, 1.0);
      gl.disable(gl.DEPTH_TEST);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);

      this._timerExtension = gl.getExtension("EXT_disjoint_timer_query_webgl2");
      this._timerSlots = [];
      if (this._timerExtension) {
        for (
          var queryIndex = 0;
          queryIndex < MAX_TIMER_QUERIES;
          queryIndex += 1
        ) {
          var query = gl.createQuery();
          if (!query) break;
          this._timerSlots.push({ query: query, active: false });
        }
        if (this._timerSlots.length === 0) this._timerExtension = null;
      }
      this._timerCursor = 0;
      this._activeTimerIndex = -1;
      this.available = true;
      this.contextLost = false;
      gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    } catch (error) {
      this.available = false;
      this._deleteGLResources(gl);
      this.gl = null;
      this._status("WebGL2 initialization failed: " + error.message);
      throw error;
    }
  };

  Renderer.prototype._deleteGLResources = function (gl) {
    if (gl) {
      if (this._timerSlots) {
        for (var i = 0; i < this._timerSlots.length; i += 1) {
          if (this._timerSlots[i].query)
            gl.deleteQuery(this._timerSlots[i].query);
        }
      }
      if (this._birdVao) gl.deleteVertexArray(this._birdVao);
      if (this._trailVao) gl.deleteVertexArray(this._trailVao);
      if (this._birdMeshBuffer) gl.deleteBuffer(this._birdMeshBuffer);
      if (this._birdInstanceBuffer) gl.deleteBuffer(this._birdInstanceBuffer);
      if (this._trailCornerBuffer) gl.deleteBuffer(this._trailCornerBuffer);
      if (this._trailSegmentBuffer) gl.deleteBuffer(this._trailSegmentBuffer);
      if (this._birdProgram) gl.deleteProgram(this._birdProgram);
      if (this._trailProgram) gl.deleteProgram(this._trailProgram);
    }
    this._timerSlots = null;
    this._timerExtension = null;
    this._birdVao = null;
    this._trailVao = null;
    this._birdMeshBuffer = null;
    this._birdInstanceBuffer = null;
    this._trailCornerBuffer = null;
    this._trailSegmentBuffer = null;
    this._birdProgram = null;
    this._trailProgram = null;
    this._birdUniforms = null;
    this._trailUniforms = null;
    this._activeTimerIndex = -1;
  };

  Renderer.prototype._onContextLost = function (event) {
    if (event && typeof event.preventDefault === "function")
      event.preventDefault();
    this.contextLost = true;
    this.available = false;
    this.gl = null;
    this._deleteGLResources(null);
    this._stats.drawCalls = 0;
    this._stats.triangles = 0;
    this._stats.uploadBytes = 0;
    this._stats.birdInstances = 0;
    this._stats.dyingInstances = 0;
    this._stats.trailSegments = 0;
    this._stats.gpuMs = null;
    this._status("WebGL context lost; waiting for restoration.");
  };

  Renderer.prototype._onContextRestored = function () {
    if (this.disposed) return;
    var gl = this.canvas.getContext("webgl2", CONTEXT_ATTRIBUTES);
    if (!gl) {
      this.available = false;
      this.contextLost = false;
      this._status("WebGL2 context restoration failed.");
      return;
    }
    this._initializeGL(gl);
    this.resize(this._width, this._height, this._dpr, this._scale);
    this._status("WebGL2 context restored.");
  };

  Renderer.prototype.resize = function (width, height, dpr, scale) {
    if (this.disposed) return;
    if (Number.isFinite(width) && width > 0) this._width = width;
    if (Number.isFinite(height) && height > 0) this._height = height;
    if (Number.isFinite(dpr) && dpr > 0) this._dpr = dpr;
    if (Number.isFinite(scale) && scale > 0)
      this._scale = clamp(scale, 0.5, 1.0);
    var pixelWidth = Math.max(
      1,
      Math.round(this._width * this._dpr * this._scale),
    );
    var pixelHeight = Math.max(
      1,
      Math.round(this._height * this._dpr * this._scale),
    );
    if (this.canvas.width !== pixelWidth) this.canvas.width = pixelWidth;
    if (this.canvas.height !== pixelHeight) this.canvas.height = pixelHeight;
    if (this.available && this.gl)
      this.gl.viewport(0, 0, pixelWidth, pixelHeight);
    if (typeof this.canvas.getBoundingClientRect === "function") {
      var rect = this.canvas.getBoundingClientRect();
      if (Number.isFinite(rect.left)) this._canvasLeft = rect.left;
      if (Number.isFinite(rect.top)) this._canvasTop = rect.top;
      this._canvasCssWidth =
        Number.isFinite(rect.width) && rect.width > 0 ? rect.width : this._width;
      this._canvasCssHeight =
        Number.isFinite(rect.height) && rect.height > 0 ? rect.height : this._height;
    } else {
      this._canvasCssWidth = this._width;
      this._canvasCssHeight = this._height;
    }
  };
  Renderer.prototype._scissorToBounds = function (bounds) {
    var canvasWidth = this.canvas.width;
    var canvasHeight = this.canvas.height;
    var left = 0;
    var top = 0;
    var right = canvasWidth;
    var bottom = canvasHeight;
    if (
      bounds &&
      Number.isFinite(bounds.left) &&
      Number.isFinite(bounds.top) &&
      Number.isFinite(bounds.right) &&
      Number.isFinite(bounds.bottom)
    ) {
      var scaleX = canvasWidth / (this._canvasCssWidth || this._width);
      var scaleY = canvasHeight / (this._canvasCssHeight || this._height);
      left = Math.floor((bounds.left - this._canvasLeft) * scaleX);
      top = Math.floor((bounds.top - this._canvasTop) * scaleY);
      right = Math.ceil((bounds.right - this._canvasLeft) * scaleX);
      bottom = Math.ceil((bounds.bottom - this._canvasTop) * scaleY);
      left = clamp(left, 0, canvasWidth);
      top = clamp(top, 0, canvasHeight);
      right = clamp(right, left, canvasWidth);
      bottom = clamp(bottom, top, canvasHeight);
    }
    this.gl.scissor(left, canvasHeight - bottom, right - left, bottom - top);
    this.gl.enable(this.gl.SCISSOR_TEST);
  };

  Renderer.prototype._collectBirds = function (sim) {
    if (
      !sim ||
      !sim.x ||
      !sim.y ||
      !sim.vx ||
      !sim.vy ||
      !sim.hue ||
      !sim.active
    ) {
      throw new TypeError(
        "render(sim, history, settings) requires simulation position, velocity, hue, and active arrays.",
      );
    }
    var capacity = sim.x.length;
    if (capacity > MAX_BIRDS)
      throw new RangeError(
        "Renderer supports at most " + MAX_BIRDS + " birds.",
      );
    var count = sim.count;
    if (
      !Number.isInteger(count) ||
      count < 0 ||
      count > sim.active.length ||
      count > MAX_BIRDS
    ) {
      throw new RangeError(
        "Simulation drawable count is outside renderer capacity.",
      );
    }
    var state = sim.state;
    var generations = sim.generation;
    var progress = sim.deathProgress;
    var data = this._birdData;
    var written = 0;
    var dying = 0;
    for (var i = 0; i < count; i += 1) {
      var slot = sim.active[i];
      if (slot < 0 || slot >= capacity || (state && state[slot] === 0))
        continue;
      var x = sim.x[slot];
      var y = sim.y[slot];
      var vx = sim.vx[slot];
      var vy = sim.vy[slot];
      var hue = sim.hue[slot];
      if (!Number.isFinite(x) || !Number.isFinite(y)) continue;
      if (!Number.isFinite(vx)) vx = 0;
      if (!Number.isFinite(vy)) vy = 0;
      if (!Number.isFinite(hue))
        throw new TypeError("Simulation bird hues must be finite.");
      var death = state && state[slot] === 4;
      if (death && (!progress || slot >= progress.length)) {
        throw new TypeError(
          "Dying birds require a simulation deathProgress array.",
        );
      }
      var generation = generations ? generations[slot] >>> 0 : 0;
      if (this._phaseGeneration[slot] !== generation) {
        this._phaseGeneration[slot] = generation;
        this._phases[slot] = phaseFor(slot, generation);
      }
      var offset = written * 7;
      data[offset] = x;
      data[offset + 1] = y;
      data[offset + 2] = vx;
      data[offset + 3] = vy;
      data[offset + 4] = this._phases[slot];
      data[offset + 5] = death
        ? Number.isFinite(progress[slot])
          ? clamp(progress[slot], 0, 1)
          : 0
        : -1;
      data[offset + 6] = hue;
      if (death) dying += 1;
      written += 1;
    }
    this._dyingInstanceCount = dying;
    return written;
  };

  Renderer.prototype._appendTrail = function (
    xs,
    ys,
    ts,
    base,
    head,
    count,
    ringLength,
    slot,
    generation,
    hues,
  ) {
    if (!xs || !ys || !ts || !hues)
      throw new TypeError(
        "History trail callback must provide x, y, timestamp, and hue arrays.",
      );
    if (
      !Number.isInteger(ringLength) ||
      ringLength <= 0 ||
      ringLength > HISTORY_RING_LENGTH
    ) {
      throw new RangeError(
        "History trail ring length exceeds renderer capacity.",
      );
    }
    if (count < 2) return;
    count = Math.min(count, ringLength);
    base = Math.trunc(base);
    head = Math.trunc(head);
    if (
      base < 0 ||
      base + ringLength > xs.length ||
      base + ringLength > ys.length ||
      base + ringLength > ts.length ||
      base + ringLength > hues.length
    ) {
      throw new RangeError("History trail base is outside its sample arrays.");
    }
    var first = head - count + 1;
    while (first < 0) first += ringLength;
    first %= ringLength;
    var previous = first;
    var now = this._trailNow;
    var cutoff = now - this._trailDuration;
    var data = this._trailData;
    var epoch = this._trailEpoch;
    var segments = this._trailSegmentCount;
    var nextSampleTime = ts[base + first] + 1 / this._trailHz;
    for (var sample = 1; sample < count; sample += 1) {
      var current = (first + sample) % ringLength;
      if (sample < count - 1 && ts[base + current] < nextSampleTime - 0.00001)
        continue;
      nextSampleTime += 1 / this._trailHz;
      var t0 = ts[base + previous];
      var t1 = ts[base + current];
      if (
        Number.isFinite(t0) &&
        Number.isFinite(t1) &&
        t1 > t0 &&
        t1 > cutoff &&
        t0 < now
      ) {
        var x0 = xs[base + previous];
        var y0 = ys[base + previous];
        var x1 = xs[base + current];
        var y1 = ys[base + current];
        var hue0 = hues[base + previous];
        var hue1 = hues[base + current];
        if (
          Number.isFinite(x0) &&
          Number.isFinite(y0) &&
          Number.isFinite(x1) &&
          Number.isFinite(y1) &&
          Number.isFinite(hue0) &&
          Number.isFinite(hue1)
        ) {
          if (segments >= MAX_TRAIL_SEGMENTS) {
            throw new RangeError(
              "History exceeds the renderer preallocated trail segment capacity.",
            );
          }
          var offset = segments * 8;
          data[offset] = x0;
          data[offset + 1] = y0;
          data[offset + 2] = t0 - epoch;
          data[offset + 3] = hue0;
          data[offset + 4] = x1;
          data[offset + 5] = y1;
          data[offset + 6] = t1 - epoch;
          data[offset + 7] = hue1;
          segments += 1;
        }
      }
      previous = current;
    }
    this._trailSegmentCount = segments;
  };

  Renderer.prototype._pollTimerQueries = function () {
    var gl = this.gl;
    var extension = this._timerExtension;
    var slots = this._timerSlots;
    if (!extension || !slots || slots.length === 0) {
      this._stats.gpuMs = null;
      return;
    }
    if (gl.getParameter(extension.GPU_DISJOINT_EXT)) {
      this._stats.gpuMs = null;
      for (var i = 0; i < slots.length; i += 1) {
        if (slots[i].active) {
          gl.deleteQuery(slots[i].query);
          slots[i].query = gl.createQuery();
          slots[i].active = false;
        }
      }
      return;
    }
    for (var j = 0; j < slots.length; j += 1) {
      var slot = slots[j];
      if (
        !slot.active ||
        !gl.getQueryParameter(slot.query, gl.QUERY_RESULT_AVAILABLE)
      )
        continue;
      var nanoseconds = gl.getQueryParameter(slot.query, gl.QUERY_RESULT);
      if (Number.isFinite(nanoseconds)) {
        this._stats.gpuMs = nanoseconds / 1000000;
        this._stats.gpuSamples++;
      }
      slot.active = false;
    }
  };

  Renderer.prototype._beginTimerQuery = function () {
    var slots = this._timerSlots;
    if (!this._timerExtension || !slots || slots.length === 0) return -1;
    for (var attempt = 0; attempt < slots.length; attempt += 1) {
      var index = (this._timerCursor + attempt) % slots.length;
      var slot = slots[index];
      if (slot.active || !slot.query) continue;
      this.gl.beginQuery(this._timerExtension.TIME_ELAPSED_EXT, slot.query);
      slot.active = true;
      this._timerCursor = (index + 1) % slots.length;
      this._activeTimerIndex = index;
      return index;
    }
    return -1;
  };

  Renderer.prototype._endTimerQuery = function (index) {
    if (index < 0) return;
    this.gl.endQuery(this._timerExtension.TIME_ELAPSED_EXT);
    this._activeTimerIndex = -1;
  };

  Renderer.prototype.render = function (sim, history, settings) {
    var stats = this._stats;
    stats.drawCalls = 0;
    stats.triangles = 0;
    stats.uploadBytes = 0;
    stats.birdInstances = 0;
    stats.dyingInstances = 0;
    if (this.disposed || !this.available || !this.gl) return stats;
    settings = settings || {};
    if (
      Number.isFinite(settings.renderScale) &&
      Math.abs(settings.renderScale - this._scale) > 0.001
    ) {
      this.resize(this._width, this._height, this._dpr, settings.renderScale);
    }

    var time = Number.isFinite(sim && sim.time) ? sim.time : 0;
    // Keep GPU floats local even after days of continuous uptime.
    var epoch = Math.floor(time / 1024) * 1024;
    var renderTime = time - epoch;
    var size = Number.isFinite(settings.size) ? clamp(settings.size, 1, 8) : 3;
    var brightness = Number.isFinite(settings.brightness)
      ? clamp(settings.brightness, 0.1, 2)
      : 1.15;
    var contrast = Number.isFinite(settings.contrast)
      ? clamp(settings.contrast, 0.5, 2)
      : 1.2;
    var duration = Number.isFinite(settings.trailDuration)
      ? clamp(settings.trailDuration, 0.1, 2.5)
      : 1.2;
    setColorFromHex(settings.color || DEFAULT_COLOR, this._color);
    var randomColor = settings.randomColor === true;

    var birdCount = this._collectBirds(sim);
    var dyingCount = this._dyingInstanceCount;
    var trailsEnabled = settings.trails !== false;
    var trailCount = 0;
    var trailUploadStart = 0;
    if (trailsEnabled) {
      if (
        !history ||
        typeof history.forEachTrail !== "function" ||
        !history.hue
      ) {
        throw new TypeError(
          "render(sim, history, settings) requires hue-aware history when trails are enabled.",
        );
      }
      this._trailNow = time;
      this._trailEpoch = epoch;
      this._trailDuration = duration;
      this._trailHz = Number.isFinite(settings.trailHz)
        ? clamp(settings.trailHz, 10, 60)
        : 30;
      var changed =
        this._cachedHistory !== history ||
        typeof history.revision !== "number" ||
        this._cachedHistoryRevision !== history.revision ||
        this._cachedHistoryDuration !== duration ||
        this._cachedHistoryHz !== this._trailHz ||
        this._cachedHistoryEpoch !== epoch;
      if (changed) {
        this._trailSegmentCount = 0;
        if (
          typeof history.hasVisible !== "function" ||
          history.hasVisible(time)
        )
          history.forEachTrail(sim, this._trailCallback);
        this._cachedHistoricalCount = this._trailSegmentCount;
        this._cachedHistory = history;
        this._cachedHistoryRevision = history.revision;
        this._cachedHistoryDuration = duration;
        this._cachedHistoryHz = this._trailHz;
        this._cachedHistoryEpoch = epoch;
      } else {
        this._trailSegmentCount = this._cachedHistoricalCount;
        trailUploadStart = this._cachedHistoricalCount * 8;
      }
      // Join the last fixed-rate sample to the current bird without changing history.
      for (var a = 0; a < sim.count; a += 1) {
        var slot = sim.active[a];
        if (!history.count[slot]) continue;
        var sampleIndex = slot * history.length + history.head[slot];
        var sampleTime = history.t[sampleIndex];
        if (time <= sampleTime + 0.000001) continue;
        if (this._trailSegmentCount >= MAX_TRAIL_SEGMENTS)
          throw new RangeError("Trail head capacity exceeded.");
        var p = this._trailSegmentCount++ * 8;
        this._trailData[p] = history.x[sampleIndex];
        this._trailData[p + 1] = history.y[sampleIndex];
        this._trailData[p + 2] = sampleTime - epoch;
        this._trailData[p + 3] = history.hue[sampleIndex];
        this._trailData[p + 4] = sim.x[slot];
        this._trailData[p + 5] = sim.y[slot];
        this._trailData[p + 6] = renderTime;
        this._trailData[p + 7] = sim.hue[slot];
      }
      trailCount = this._trailSegmentCount;
    }

    var gl = this.gl;
    this._pollTimerQueries();
    var timerIndex = this._beginTimerQuery();
    try {
      if (birdCount > 0) {
        gl.bindBuffer(gl.ARRAY_BUFFER, this._birdInstanceBuffer);
        gl.bufferSubData(
          gl.ARRAY_BUFFER,
          0,
          this._birdData,
          0,
          birdCount * 7,
        );
        stats.uploadBytes += birdCount * 7 * Float32Array.BYTES_PER_ELEMENT;
      }
      var trailUploadFloats = trailCount * 8 - trailUploadStart;
      if (trailUploadFloats > 0) {
        gl.bindBuffer(gl.ARRAY_BUFFER, this._trailSegmentBuffer);
        gl.bufferSubData(
          gl.ARRAY_BUFFER,
          trailUploadStart * Float32Array.BYTES_PER_ELEMENT,
          this._trailData,
          trailUploadStart,
          trailUploadFloats,
        );
        stats.uploadBytes += trailUploadFloats * Float32Array.BYTES_PER_ELEMENT;
      }
      gl.bindBuffer(gl.ARRAY_BUFFER, null);
      gl.disable(gl.SCISSOR_TEST);
      gl.clear(gl.COLOR_BUFFER_BIT);
      this._scissorToBounds(sim && sim.bounds);
      if (trailCount > 0) {
        var trailUniforms = this._trailUniforms;
        gl.useProgram(this._trailProgram);
        gl.uniform2f(trailUniforms.viewport, this._width, this._height);
        gl.uniform2f(
          trailUniforms.pixelSize,
          this._width / this.canvas.width,
          this._height / this.canvas.height,
        );
        gl.uniform1f(trailUniforms.now, renderTime);
        gl.uniform1f(trailUniforms.duration, duration);
        gl.uniform1f(trailUniforms.width, clamp(size * 0.2, 0.55, 1.8));
        gl.uniform3fv(trailUniforms.color, this._color);
        gl.uniform1f(trailUniforms.brightness, brightness);
        gl.uniform1i(trailUniforms.randomColor, randomColor ? 1 : 0);
        gl.uniform1f(trailUniforms.contrast, contrast);
        gl.bindVertexArray(this._trailVao);
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, trailCount);
        stats.drawCalls += 1;
        stats.triangles += trailCount * 2;
      }
      if (birdCount > 0) {
        var birdUniforms = this._birdUniforms;
        gl.useProgram(this._birdProgram);
        gl.uniform2f(birdUniforms.viewport, this._width, this._height);
        gl.uniform1f(birdUniforms.size, size);
        gl.uniform1f(birdUniforms.phase, (time * 9.4) % TWO_PI);
        gl.uniform3fv(birdUniforms.color, this._color);
        gl.uniform1f(birdUniforms.brightness, brightness);
        gl.uniform1i(birdUniforms.randomColor, randomColor ? 1 : 0);
        gl.uniform1f(birdUniforms.contrast, contrast);
        gl.bindVertexArray(this._birdVao);
        gl.drawArraysInstanced(
          gl.TRIANGLES,
          0,
          BIRD_MESH.length / 4,
          birdCount,
        );
        stats.drawCalls += 1;
        stats.triangles += (BIRD_MESH.length / 12) * birdCount;
      }
    } finally {
      gl.bindVertexArray(null);
      gl.disable(gl.SCISSOR_TEST);
      this._endTimerQuery(timerIndex);
    }
    stats.birdInstances = birdCount;
    stats.dyingInstances = dyingCount;
    stats.trailSegments = trailCount;
    return stats;
  };

  Renderer.prototype.dispose = function () {
    if (this.disposed) return;
    this.disposed = true;
    this.available = false;
    this.canvas.removeEventListener(
      "webglcontextlost",
      this._handleContextLost,
      false,
    );
    this.canvas.removeEventListener(
      "webglcontextrestored",
      this._handleContextRestored,
      false,
    );
    if (this.gl && !this.contextLost) this._deleteGLResources(this.gl);
    else this._deleteGLResources(null);
    this.gl = null;
    this._birdData = null;
    this._trailData = null;
    this._phases = null;
    this._phaseGeneration = null;
    this._color = null;
    this._cachedHistory = null;
    this._trailCallback = null;
    this._handleContextLost = null;
    this._handleContextRestored = null;
    this.onStatus = null;
  };

  Boids.Renderer = Renderer;
})(typeof globalThis !== "undefined" ? globalThis : window);
