(function (B) {
  "use strict";
  class History {
    constructor(capacity = 3000) {
      this.capacity = capacity;
      this.length = 156;
      this.ghostCapacity = 6000;
      for (const k of ["x", "y"])
        this[k] = new Float32Array(capacity * this.length);
      this.t = new Float64Array(capacity * this.length);
      this.head = new Int16Array(capacity);
      this.head.fill(-1);
      this.count = new Uint8Array(capacity);
      this.lastGeneration = new Uint32Array(capacity);
      this.gx = new Float32Array(this.ghostCapacity * this.length);
      this.gy = new Float32Array(this.gx.length);
      this.gt = new Float64Array(this.gx.length);
      this.gh = new Int16Array(this.ghostCapacity);
      this.gc = new Uint8Array(this.ghostCapacity);
      this.until = new Float64Array(this.ghostCapacity);
      this.ghostSlot = new Uint32Array(this.ghostCapacity);
      this.ghostGeneration = new Uint32Array(this.ghostCapacity);
      this.ghostCursor = 0;
      this.lastSample = -Infinity;
      this.overflows = 0;
      this.revision = 0;
    }
    retire(i, time) {
      if (!this.count[i]) return;
      let g = this.ghostCursor;
      let attempts = 0;
      while (this.until[g] > time && attempts++ < this.ghostCapacity)
        g = (g + 1) % this.ghostCapacity;
      if (this.until[g] > time) this.overflows++;
      const a = i * this.length,
        b = g * this.length;
      this.gx.set(this.x.subarray(a, a + this.length), b);
      this.gy.set(this.y.subarray(a, a + this.length), b);
      this.gt.set(this.t.subarray(a, a + this.length), b);
      this.gh[g] = this.head[i];
      this.gc[g] = this.count[i];
      this.ghostSlot[g] = i;
      this.ghostGeneration[g] = this.lastGeneration[i];
      this.until[g] = this.t[a + this.head[i]] + 2.5;
      this.ghostCursor = (g + 1) % this.ghostCapacity;
      this.count[i] = 0;
      this.head[i] = -1;
      this.revision++;
    }
    append(i, x, y, time) {
      const head = (this.head[i] + 1) % this.length,
        position = i * this.length + head;
      this.x[position] = x;
      this.y[position] = y;
      this.t[position] = time;
      this.head[i] = head;
      this.count[i] = Math.min(this.length, this.count[i] + 1);
    }
    finish(sim, i, generation, fallbackTime) {
      if (!this.count[i]) return;
      let time = fallbackTime;
      if (
        sim.deathStarted &&
        sim.deathGeneration &&
        sim.deathStarted[i] >= 0 &&
        sim.deathGeneration[i] === generation
      ) {
        time = sim.deathStarted[i];
        const x = sim.deathX ? sim.deathX[i] : sim.x[i],
          y = sim.deathY ? sim.deathY[i] : sim.y[i],
          base = i * this.length,
          head = this.head[i],
          lastTime = this.t[base + head];
        if (time > lastTime) this.append(i, x, y, time);
        else if (time === lastTime) {
          this.x[base + head] = x;
          this.y[base + head] = y;
        }
      }
      this.retire(i, fallbackTime);
    }
    record(sim) {
      const target = Number.isFinite(this.lastSample)
        ? this.lastSample + 1 / 60
        : sim.time;
      const sample = sim.time >= target - 1e-7;
      const alpha = Math.max(0, Math.min(1, 1 - (sim.time - target) * 60));
      for (let i = 0; i < this.capacity; i++) {
        const state = sim.state[i],
          generation = sim.generation[i];
        if (
          this.count[i] &&
          (state === 0 || state === 4 || this.lastGeneration[i] !== generation)
        )
          this.finish(sim, i, this.lastGeneration[i], sim.time);
        if (!state || state === 4 || !sample) continue;
        if (this.lastGeneration[i] !== generation) {
          this.count[i] = 0;
          this.head[i] = -1;
          this.lastGeneration[i] = generation;
        }
        const interpolate = this.count[i] > 0 && sim.nx && sim.ny;
        this.append(
          i,
          interpolate ? sim.nx[i] + (sim.x[i] - sim.nx[i]) * alpha : sim.x[i],
          interpolate ? sim.ny[i] + (sim.y[i] - sim.ny[i]) * alpha : sim.y[i],
          target,
        );
      }
      if (sample) {
        this.lastSample = target;
        this.revision++;
      }
    }
    forEachTrail(sim, callback) {
      for (let i = 0; i < this.capacity; i++)
        if (this.count[i] > 1)
          callback(
            this.x,
            this.y,
            this.t,
            i * this.length,
            this.head[i],
            this.count[i],
            this.length,
            i,
            this.lastGeneration[i],
          );
      for (let g = 0; g < this.ghostCapacity; g++)
        if (this.gc[g] > 1 && this.until[g] > sim.time)
          callback(
            this.gx,
            this.gy,
            this.gt,
            g * this.length,
            this.gh[g],
            this.gc[g],
            this.length,
            this.ghostSlot[g],
            this.ghostGeneration[g],
          );
    }
    hasVisible(time) {
      for (let i = 0; i < this.capacity; i++)
        if (
          this.count[i] &&
          this.t[i * this.length + this.head[i]] + 2.5 > time
        )
          return true;
      for (let g = 0; g < this.ghostCapacity; g++)
        if (this.until[g] > time) return true;
      return false;
    }
  }
  B.History = History;
})((globalThis.Boids = globalThis.Boids || {}));
