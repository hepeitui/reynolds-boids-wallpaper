(function (root) {
  "use strict";

  const BirdVsSource = `#version 300 es
precision highp float;

layout(location = 0) in vec2 aLocal;
layout(location = 1) in float aLuminance;
layout(location = 2) in float aWing;

layout(location = 3) in vec2 aCenter;
layout(location = 4) in vec2 aVelocity;
layout(location = 5) in float aPhase;
layout(location = 6) in float aDeathProgress;
layout(location = 7) in float aAlpha;
layout(location = 8) in vec3 aBirdColor;

uniform vec2 uViewport;
uniform float uSize;
uniform float uTime;

out float vLuminance;
out float vDepth;
out float vDeathProgress;
out float vAlpha;
out vec3 vBirdColor;

float smoothValue(float edge0, float edge1, float value) {
    float t = clamp((value - edge0) / max(edge1 - edge0, 0.00001), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    float speed = length(aVelocity);
    vec2 direction = speed > 0.0001 ? aVelocity / speed : vec2(1.0, 0.0);
    float beat = uTime * 7.0 + aPhase;
    float flap = sin(beat);
    float dying = aDeathProgress >= 0.0 ? 1.0 : 0.0;
    float progress = clamp(aDeathProgress, 0.0, 1.0);
    float unfold = 0.64 + 0.52 * smoothValue(0.0, 0.18, progress);
    float shrink = 1.0 - smoothValue(0.32, 1.0, progress);
    vec2 local = aLocal;
    local.y *= mix(1.0, 0.84 + 0.12 * flap, aWing);
    local.x += aWing * abs(aLocal.y) * 0.055 * cos(beat);
    local.y *= mix(1.0, 1.28, dying * smoothValue(0.0, 0.18, progress) * aWing);
    float depth = 0.91 + 0.12 * (0.5 + 0.5 * sin(aPhase * 1.73 + 0.8));
    local *= uSize * depth * mix(1.0, unfold * shrink, dying);
    vec2 rotated = vec2(
        local.x * direction.x - local.y * direction.y,
        local.x * direction.y + local.y * direction.x
    );
    vec2 birdPosition = aCenter + rotated;
    vec2 clip = vec2(
        birdPosition.x / max(uViewport.x, 1.0) * 2.0 - 1.0,
        1.0 - birdPosition.y / max(uViewport.y, 1.0) * 2.0
    );
    gl_Position = vec4(clip, 0.0, 1.0);
    vLuminance = aLuminance;
    vDepth = depth;
    vDeathProgress = aDeathProgress;
    vAlpha = aAlpha;
    vBirdColor = aBirdColor;
}
`;

  const BirdFsSource = `#version 300 es
precision highp float;

in float vLuminance;
in float vDepth;
in float vDeathProgress;
in float vAlpha;
in vec3 vBirdColor;

uniform vec3 uColor;
uniform float uBrightness;
uniform float uContrast;
uniform int uRandomColor;

out vec4 fragColor;

float smoothValue(float edge0, float edge1, float value) {
    float t = clamp((value - edge0) / max(edge1 - edge0, 0.00001), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    vec3 base = (uRandomColor != 0) ? clamp(vBirdColor, 0.0, 1.0) : uColor;
    float tone = (vLuminance - 0.64) * 1.5;
    vec3 feather = base * clamp(1.0 + tone * uContrast, 0.38, 1.0) * vDepth;
    float dying = vDeathProgress >= 0.0 ? 1.0 : 0.0;
    float progress = clamp(vDeathProgress, 0.0, 1.0);
    float flash = 1.0 - smoothValue(0.02, 0.32, progress);
    feather = mix(feather, vec3(0.96, 1.0, 1.0), dying * flash * 0.48);
    float fade = 1.0 - smoothValue(0.35, 1.0, progress);
    float alpha = 0.96 * vAlpha * mix(1.0, fade, dying);
    fragColor = vec4(feather * uBrightness, alpha);
}
`;

  const TrailVsSource = `#version 300 es
precision highp float;

layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aStart;
layout(location = 2) in vec3 aStartColor;
layout(location = 3) in vec3 aEnd;
layout(location = 4) in vec3 aEndColor;

uniform vec2 uViewport;
uniform float uNow;
uniform float uDuration;
uniform float uWidth;

out float vFade;
out float vDistance;
out float vHalfWidth;
out vec3 vColor;

float smoothValue(float edge0, float edge1, float value) {
    float t = clamp((value - edge0) / max(edge1 - edge0, 0.00001), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    float cutoff = uNow - uDuration;
    float deltaTime = aEnd.z - aStart.z;
    bool valid = (deltaTime > 0.000001) && (aEnd.z > cutoff) && (aStart.z < uNow);
    float visibleFrom = max(aStart.z, cutoff);
    float visibleTo = min(aEnd.z, uNow);
    float invDelta = deltaTime > 0.000001 ? 1.0 / deltaTime : 0.0;
    float fromT = clamp((visibleFrom - aStart.z) * invDelta, 0.0, 1.0);
    float toT = clamp((visibleTo - aStart.z) * invDelta, 0.0, 1.0);
    vec2 from = mix(aStart.xy, aEnd.xy, fromT);
    vec2 to = mix(aStart.xy, aEnd.xy, toT);
    float along = aCorner.x;
    float pointTime = mix(visibleFrom, visibleTo, along);
    float age = clamp((pointTime - cutoff) / max(uDuration, 0.0001), 0.0, 1.0);
    float fade = smoothValue(0.0, 0.38, age);
    vec2 tangent = to - from;
    float tangentLength = length(tangent);
    vec2 normal = tangentLength > 0.0001 ? vec2(-tangent.y, tangent.x) / tangentLength : vec2(0.0, 1.0);
    float taper = 0.18 + 0.82 * fade;
    float halfWidth = uWidth * taper * 0.5;
    vec2 trailPosition = mix(from, to, along) + normal * (aCorner.y * halfWidth);
    vec2 clip = vec2(
        trailPosition.x / max(uViewport.x, 1.0) * 2.0 - 1.0,
        1.0 - trailPosition.y / max(uViewport.y, 1.0) * 2.0
    );
    gl_Position = valid ? vec4(clip, 0.0, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
    vFade = valid ? fade : 0.0;
    vDistance = aCorner.y * halfWidth;
    vHalfWidth = halfWidth;
    vColor = mix(aStartColor, aEndColor, mix(fromT, toT, along));
}
`;

  const TrailFsSource = `#version 300 es
precision highp float;

in float vFade;
in float vDistance;
in float vHalfWidth;
in vec3 vColor;

uniform vec3 uColor;
uniform float uBrightness;
uniform float uContrast;
uniform int uRandomColor;

out vec4 fragColor;

void main() {
    float footprint = max(fwidth(vDistance), 0.0001);
    float overlap = min(vHalfWidth, vDistance + footprint * 0.5)
                  - max(-vHalfWidth, vDistance - footprint * 0.5);
    float coverage = max(overlap, 0.0) / footprint;
    float alpha = clamp(0.78 * uBrightness * vFade * coverage, 0.0, 0.92);
    if (alpha <= 0.001) discard;
    vec3 base = (uRandomColor != 0) ? clamp(vColor, 0.0, 1.0) : uColor;
    float tone = mix(0.25, 1.0, vFade);
    float shade = clamp(1.0 + (tone - 1.0) * uContrast, 0.4, 1.0);
    fragColor = vec4(base * uBrightness * shade, alpha);
}
`;

  function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const error = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      throw new Error("Shader compilation error: " + error);
    }
    return shader;
  }

  function createProgram(gl, vsSource, fsSource) {
    const vs = createShader(gl, gl.VERTEX_SHADER, vsSource);
    const fs = createShader(gl, gl.FRAGMENT_SHADER, fsSource);
    const program = gl.createProgram();
    gl.attachShader(program, vs);
    gl.attachShader(program, fs);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const error = gl.getProgramInfoLog(program);
      gl.deleteProgram(program);
      throw new Error("Program link error: " + error);
    }
    gl.deleteShader(vs);
    gl.deleteShader(fs);
    return program;
  }

  class WebGLBoidsRenderer {
    constructor(canvas, wasmModule) {
      if (!canvas) {
        throw new Error("Canvas is required for WebGLBoidsRenderer.");
      }
      this.canvas = canvas;
      this.wasm = wasmModule;
      this._contextLost = false;
      this.birdCount = 0;
      this.trailCount = 0;
      this.onContextRestored = null;
      this.onContextLost = null;

      this._handleContextLost = (e) => {
        e.preventDefault();
        this._contextLost = true;
        this.birdCount = 0;
        this.trailCount = 0;
        if (typeof this.onContextLost === "function") {
          try {
            this.onContextLost(e);
          } catch (err) {
            console.error("Error in onContextLost callback:", err);
          }
        }
      };

      this._handleContextRestored = (e) => {
        this._contextLost = false;
        try {
          this.initResources();
        } catch (err) {
          this._contextLost = true;
          console.error("Failed to re-initialize WebGL resources after context restored:", err);
          return;
        }

        if (typeof this.onContextRestored === "function") {
          try {
            this.onContextRestored();
          } catch (err) {
            console.error("Error in onContextRestored callback:", err);
          }
        }

        try {
          this.canvas.dispatchEvent(new CustomEvent("rendererrestored", { bubbles: true, detail: { renderer: this } }));
        } catch (err) {
          // ignore event dispatch errors
        }
      };

      this.canvas.addEventListener("webglcontextlost", this._handleContextLost, false);
      this.canvas.addEventListener("webglcontextrestored", this._handleContextRestored, false);

      const gl = canvas.getContext("webgl2", {
        alpha: false,
        depth: false,
        stencil: false,
        antialias: false,
        premultipliedAlpha: false,
        preserveDrawingBuffer: false,
        powerPreference: "high-performance"
      });
      if (!gl) {
        throw new Error("WebGL 2 is required for Reynolds Boids WASM.");
      }
      this.gl = gl;

      this.initResources();
    }

    get isContextLost() {
      return this._contextLost || !this.gl || this.gl.isContextLost();
    }

    get isAvailable() {
      return !this.isContextLost;
    }

    deleteResources() {
      const gl = this.gl;
      if (!gl) return;
      const isLost = gl.isContextLost();
      if (!isLost) {
        if (this.birdProgram) gl.deleteProgram(this.birdProgram);
        if (this.trailProgram) gl.deleteProgram(this.trailProgram);
        if (this.birdVao) gl.deleteVertexArray(this.birdVao);
        if (this.trailVao) gl.deleteVertexArray(this.trailVao);
        if (this.birdMeshBuffer) gl.deleteBuffer(this.birdMeshBuffer);
        if (this.birdInstanceBuffer) gl.deleteBuffer(this.birdInstanceBuffer);
        if (this.trailCornerBuffer) gl.deleteBuffer(this.trailCornerBuffer);
        if (this.trailInstanceBuffer) gl.deleteBuffer(this.trailInstanceBuffer);
      }
      this.birdProgram = null;
      this.birdUniforms = null;
      this.birdVao = null;
      this.birdMeshBuffer = null;
      this.birdInstanceBuffer = null;
      this.trailProgram = null;
      this.trailUniforms = null;
      this.trailVao = null;
      this.trailCornerBuffer = null;
      this.trailInstanceBuffer = null;
    }

    initResources() {
      const gl = this.gl;
      const wasm = this.wasm;
      if (!gl || !wasm) return;

      // Enable alpha blending matching Direct3D 11 setup
      gl.enable(gl.BLEND);
      gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      gl.blendEquationSeparate(gl.FUNC_ADD, gl.FUNC_ADD);
      gl.disable(gl.CULL_FACE);
      gl.disable(gl.DEPTH_TEST);

      // --- Bird Pipeline ---
      this.birdProgram = createProgram(gl, BirdVsSource, BirdFsSource);
      this.birdUniforms = {
        viewport: gl.getUniformLocation(this.birdProgram, "uViewport"),
        size: gl.getUniformLocation(this.birdProgram, "uSize"),
        time: gl.getUniformLocation(this.birdProgram, "uTime"),
        color: gl.getUniformLocation(this.birdProgram, "uColor"),
        brightness: gl.getUniformLocation(this.birdProgram, "uBrightness"),
        contrast: gl.getUniformLocation(this.birdProgram, "uContrast"),
        randomColor: gl.getUniformLocation(this.birdProgram, "uRandomColor")
      };

      this.birdVao = gl.createVertexArray();
      gl.bindVertexArray(this.birdVao);

      // Mesh buffer from WASM (30 vertices, 4 floats each: x, y, luminance, wing)
      const birdMeshPtr = wasm._boids_get_bird_mesh();
      const birdMeshVertexCount = wasm._boids_get_bird_mesh_vertex_count();
      this.birdMeshVertexCount = birdMeshVertexCount;
      const birdMeshData = wasm.HEAPF32.subarray(
        birdMeshPtr >> 2,
        (birdMeshPtr >> 2) + birdMeshVertexCount * 4
      );

      this.birdMeshBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.birdMeshBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, birdMeshData, gl.STATIC_DRAW);

      // Vertex layout for BirdMesh (stride: 16 bytes)
      gl.enableVertexAttribArray(0); // local pos (x, y)
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 16, 0);
      gl.enableVertexAttribArray(1); // luminance
      gl.vertexAttribPointer(1, 1, gl.FLOAT, false, 16, 8);
      gl.enableVertexAttribArray(2); // wing
      gl.vertexAttribPointer(2, 1, gl.FLOAT, false, 16, 12);

      // Bird Instance Buffer (stride: 40 bytes = 10 floats)
      this.birdInstanceBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.birdInstanceBuffer);

      // Attrib 3: center (2 floats)
      gl.enableVertexAttribArray(3);
      gl.vertexAttribPointer(3, 2, gl.FLOAT, false, 40, 0);
      gl.vertexAttribDivisor(3, 1);

      // Attrib 4: velocity (2 floats)
      gl.enableVertexAttribArray(4);
      gl.vertexAttribPointer(4, 2, gl.FLOAT, false, 40, 8);
      gl.vertexAttribDivisor(4, 1);

      // Attrib 5: phase (1 float)
      gl.enableVertexAttribArray(5);
      gl.vertexAttribPointer(5, 1, gl.FLOAT, false, 40, 16);
      gl.vertexAttribDivisor(5, 1);

      // Attrib 6: deathProgress (1 float)
      gl.enableVertexAttribArray(6);
      gl.vertexAttribPointer(6, 1, gl.FLOAT, false, 40, 20);
      gl.vertexAttribDivisor(6, 1);

      // Attrib 7: alpha (1 float)
      gl.enableVertexAttribArray(7);
      gl.vertexAttribPointer(7, 1, gl.FLOAT, false, 40, 24);
      gl.vertexAttribDivisor(7, 1);

      // Attrib 8: birdColor (3 floats)
      gl.enableVertexAttribArray(8);
      gl.vertexAttribPointer(8, 3, gl.FLOAT, false, 40, 28);
      gl.vertexAttribDivisor(8, 1);

      // --- Trail Pipeline ---
      this.trailProgram = createProgram(gl, TrailVsSource, TrailFsSource);
      this.trailUniforms = {
        viewport: gl.getUniformLocation(this.trailProgram, "uViewport"),
        now: gl.getUniformLocation(this.trailProgram, "uNow"),
        duration: gl.getUniformLocation(this.trailProgram, "uDuration"),
        width: gl.getUniformLocation(this.trailProgram, "uWidth"),
        color: gl.getUniformLocation(this.trailProgram, "uColor"),
        brightness: gl.getUniformLocation(this.trailProgram, "uBrightness"),
        contrast: gl.getUniformLocation(this.trailProgram, "uContrast"),
        randomColor: gl.getUniformLocation(this.trailProgram, "uRandomColor")
      };

      this.trailVao = gl.createVertexArray();
      gl.bindVertexArray(this.trailVao);

      // Trail Corners from WASM (6 vertices, 2 floats each)
      const trailCornerPtr = wasm._boids_get_trail_corners();
      const trailCornerVertexCount = wasm._boids_get_trail_corner_vertex_count();
      this.trailCornerVertexCount = trailCornerVertexCount;
      const trailCornerData = wasm.HEAPF32.subarray(
        trailCornerPtr >> 2,
        (trailCornerPtr >> 2) + trailCornerVertexCount * 2
      );

      this.trailCornerBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.trailCornerBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, trailCornerData, gl.STATIC_DRAW);

      // Vertex layout for corners (stride: 8 bytes)
      gl.enableVertexAttribArray(0); // corner
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);

      // Trail Instance Buffer (stride: 48 bytes = 12 floats)
      this.trailInstanceBuffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, this.trailInstanceBuffer);

      // Attrib 1: start (startX, startY, startTime)
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 48, 0);
      gl.vertexAttribDivisor(1, 1);

      // Attrib 2: startColor (startR, startG, startB)
      gl.enableVertexAttribArray(2);
      gl.vertexAttribPointer(2, 3, gl.FLOAT, false, 48, 12);
      gl.vertexAttribDivisor(2, 1);

      // Attrib 3: end (endX, endY, endTime)
      gl.enableVertexAttribArray(3);
      gl.vertexAttribPointer(3, 3, gl.FLOAT, false, 48, 24);
      gl.vertexAttribDivisor(3, 1);

      // Attrib 4: endColor (endR, endG, endB)
      gl.enableVertexAttribArray(4);
      gl.vertexAttribPointer(4, 3, gl.FLOAT, false, 48, 36);
      gl.vertexAttribDivisor(4, 1);

      gl.bindVertexArray(null);
    }

    render(settings, simulationTime, logicalWidth, logicalHeight) {
      if (!this.isAvailable) {
        return false;
      }

      const gl = this.gl;
      const wasm = this.wasm;

      gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);

      // Background color matching D3D11 host
      gl.clearColor(0.012, 0.034, 0.07, 1.0);
      gl.clear(gl.COLOR_BUFFER_BIT);

      // Guard zero-dimension or zero logical size
      if (logicalWidth <= 0 || logicalHeight <= 0) {
        this.birdCount = 0;
        this.trailCount = 0;
        return false;
      }

      // Ask WASM to evaluate bird and trail instances
      wasm._boids_prepare_render_frame(simulationTime, logicalWidth, logicalHeight);

      const birdCount = wasm._boids_get_bird_count();
      const trailCount = wasm._boids_get_trail_count();
      this.birdCount = birdCount;
      this.trailCount = trailCount;

      const size = Math.max(1.0, Math.min(16.0, settings.size || 6.58));
      const brightness = Math.max(0.1, Math.min(2.0, settings.brightness !== undefined ? settings.brightness : 1.31));
      const contrast = Math.max(0.5, Math.min(2.0, settings.contrast !== undefined ? settings.contrast : 1.2));
      const duration = Math.max(0.1, Math.min(0.5, settings.trailDuration !== undefined ? settings.trailDuration : 0.28));
      const randomColorInt = settings.randomColor ? 1 : 0;
      const color = settings.colorRgb || [0.518, 0.847, 0.878];

      // 1. Draw Trails
      if (settings.trails && trailCount > 0) {
        const trailPtr = wasm._boids_get_trail_instances();
        const trailFloats = trailCount * 12;
        const trailData = wasm.HEAPF32.subarray(trailPtr >> 2, (trailPtr >> 2) + trailFloats);

        gl.useProgram(this.trailProgram);
        gl.uniform2f(this.trailUniforms.viewport, logicalWidth, logicalHeight);
        gl.uniform1f(this.trailUniforms.now, simulationTime);
        gl.uniform1f(this.trailUniforms.duration, duration);
        const trailWidth = Math.max(0.55, Math.min(1.8, size * 0.2));
        gl.uniform1f(this.trailUniforms.width, trailWidth);
        gl.uniform3f(this.trailUniforms.color, color[0], color[1], color[2]);
        gl.uniform1f(this.trailUniforms.brightness, brightness);
        gl.uniform1f(this.trailUniforms.contrast, contrast);
        gl.uniform1i(this.trailUniforms.randomColor, randomColorInt);

        gl.bindVertexArray(this.trailVao);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.trailInstanceBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, trailData, gl.DYNAMIC_DRAW);

        gl.drawArraysInstanced(gl.TRIANGLES, 0, this.trailCornerVertexCount, trailCount);
      }

      // 2. Draw Birds
      if (birdCount > 0) {
        const birdPtr = wasm._boids_get_bird_instances();
        const birdFloats = birdCount * 10;
        const birdData = wasm.HEAPF32.subarray(birdPtr >> 2, (birdPtr >> 2) + birdFloats);

        gl.useProgram(this.birdProgram);
        gl.uniform2f(this.birdUniforms.viewport, logicalWidth, logicalHeight);
        gl.uniform1f(this.birdUniforms.size, size);
        gl.uniform1f(this.birdUniforms.time, simulationTime);
        gl.uniform3f(this.birdUniforms.color, color[0], color[1], color[2]);
        gl.uniform1f(this.birdUniforms.brightness, brightness);
        gl.uniform1f(this.birdUniforms.contrast, contrast);
        gl.uniform1i(this.birdUniforms.randomColor, randomColorInt);

        gl.bindVertexArray(this.birdVao);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.birdInstanceBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, birdData, gl.DYNAMIC_DRAW);

        gl.drawArraysInstanced(gl.TRIANGLES, 0, this.birdMeshVertexCount, birdCount);
      }

      gl.bindVertexArray(null);
      return true;
    }

    destroy() {
      if (this.canvas) {
        if (this._handleContextLost) {
          this.canvas.removeEventListener("webglcontextlost", this._handleContextLost, false);
        }
        if (this._handleContextRestored) {
          this.canvas.removeEventListener("webglcontextrestored", this._handleContextRestored, false);
        }
      }
      this.deleteResources();
      this.gl = null;
      this.canvas = null;
      this.wasm = null;
    }
  }

  root.WebGLBoidsRenderer = WebGLBoidsRenderer;
})(globalThis);
