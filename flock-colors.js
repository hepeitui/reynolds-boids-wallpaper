(function (B) {
  "use strict";
  const TAU = Math.PI * 2;
  const LEADERS = 3;
  const LEADER_INFLUENCE = 0.65;
  const difference = (to, from) => to - from - Math.floor(to - from + 0.5);
  function identity(id) {
    let value = Math.imul(id ^ 0x9e3779b9, 0x21f0aaad);
    value = Math.imul(value ^ (value >>> 16), 0x735a2d97);
    return ((value ^ (value >>> 15)) >>> 0) / 4294967296;
  }

  class FlockColors {
    constructor(capacity) {
      this.capacity = capacity;
      this.flocks = new B.FlockGroups(capacity);
      this.size = new Int32Array(capacity);
      for (const key of ["sumX", "sumY", "directionX", "directionY"])
        this[key] = new Float64Array(capacity);
      this.headingSlot = new Int32Array(capacity);
      for (const key of ["birthHue", "target"])
        this[key] = new Float32Array(capacity);
      this.leaders = new Int32Array(capacity * LEADERS);
      this.leaderScore = new Float64Array(capacity * LEADERS);
      this.nextUpdate = 0;
    }
    spawn(sim, i) {
      const hue = identity(sim.id[i] ^ sim.initialSeed);
      this.birthHue[i] = this.target[i] = sim.hue[i] = sim.previousHue[i] = hue;
      this.nextUpdate = 0;
    }
    rebuild(sim) {
      const membership = this.flocks.update(sim);
      this.leaders.fill(-1);
      this.leaderScore.fill(-Infinity);
      for (let group = 0; group < this.capacity; group++) {
        this.size[group] = 0;
        this.sumX[group] = this.sumY[group] = 0;
        this.directionX[group] = this.directionY[group] = 0;
        this.headingSlot[group] = -1;
      }
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a];
        if (sim.state[i] !== 1 && sim.state[i] !== 2) continue;
        const group = membership[i];
        this.size[group]++;
        this.sumX[group] += sim.x[i];
        this.sumY[group] += sim.y[i];
        const speed = Math.hypot(sim.vx[i], sim.vy[i]);
        if (speed > 1e-6) {
          this.directionX[group] += sim.vx[i] / speed;
          this.directionY[group] += sim.vy[i] / speed;
          const first = this.headingSlot[group];
          if (first < 0 || sim.id[i] < sim.id[first]) this.headingSlot[group] = i;
        }
      }
      for (let group = 0; group < this.capacity; group++) {
        if (!this.size[group]) continue;
        this.sumX[group] /= this.size[group];
        this.sumY[group] /= this.size[group];
        // Opposing headings have no reliable mean. Use a stable moving member,
        // never a screen axis; a completely stationary group has no front.
        const first = this.headingSlot[group];
        if (first >= 0 &&
            Math.hypot(this.directionX[group], this.directionY[group]) < this.size[group] * 0.15) {
          const speed = Math.hypot(sim.vx[first], sim.vy[first]);
          this.directionX[group] = sim.vx[first] / speed;
          this.directionY[group] = sim.vy[first] / speed;
        }
      }
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a];
        if (sim.state[i] !== 1 && sim.state[i] !== 2) continue;
        const group = membership[i];
        // Peripheral birds may follow a flock, but a sparse outlier must not
        // replace its coherent front with a new color source.
        if (!this.flocks.core[i]) continue;
        if (this.headingSlot[group] < 0) continue;
        const score = (sim.x[i] - this.sumX[group]) * this.directionX[group] +
          (sim.y[i] - this.sumY[group]) * this.directionY[group];
        const start = group * LEADERS;
        for (let rank = 0; rank < LEADERS; rank++) {
          const p = start + rank, other = this.leaders[p];
          if (score < this.leaderScore[p] ||
              (score === this.leaderScore[p] && other >= 0 && sim.id[i] > sim.id[other])) continue;
          for (let end = start + LEADERS - 1; end > p; end--) {
            this.leaders[end] = this.leaders[end - 1];
            this.leaderScore[end] = this.leaderScore[end - 1];
          }
          this.leaders[p] = i;
          this.leaderScore[p] = score;
          break;
        }
      }
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a], own = this.birthHue[i];
        // Departures lose flock influence and gently return to their own color.
        this.target[i] = own;
        if (sim.state[i] !== 1 && sim.state[i] !== 2) continue;
        const start = membership[i] * LEADERS;
        let sine = 0, cosine = 0, first = -1;
        for (let rank = 0; rank < LEADERS; rank++) {
          const leader = this.leaders[start + rank];
          // Leaders are color sources: they return to their own birth color,
          // rather than following one another and converging to a shared hue.
          if (leader === i) {
            first = -1;
            break;
          }
          if (leader < 0) continue;
          if (first < 0) first = leader;
          const angle = sim.hue[leader] * TAU, weight = LEADERS - rank;
          sine += Math.sin(angle) * weight;
          cosine += Math.cos(angle) * weight;
        }
        if (first < 0) continue;
        const leaderHue = Math.hypot(sine, cosine) > 1e-5
          ? Math.atan2(sine, cosine) / TAU : sim.hue[first];
        // Followers retain 35% birth-color preference and follow current leaders.
        this.target[i] = own + difference(leaderHue, own) * LEADER_INFLUENCE;
      }
    }
    step(sim, dt) {
      if (sim.time >= this.nextUpdate) {
        this.rebuild(sim);
        this.nextUpdate = sim.time + 0.1;
      }
      const blend = 1 - Math.exp(-dt * 0.8);
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a];
        sim.previousHue[i] = sim.hue[i];
        if (sim.state[i] === 4) continue;
        sim.hue[i] += difference(this.target[i], sim.hue[i]) * blend;
        // Bound Float32 magnitudes for wallpapers running for days. Keep both
        // ends of this step on the same branch for history interpolation.
        if (sim.hue[i] < 0 || sim.hue[i] >= 1) {
          const turns = Math.floor(sim.hue[i]);
          sim.hue[i] -= turns;
          sim.previousHue[i] -= turns;
        }
      }
    }
  }
  B.FlockColors = FlockColors;
})((globalThis.Boids = globalThis.Boids || {}));
