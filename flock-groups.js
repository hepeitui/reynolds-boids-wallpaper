(function (B) {
  "use strict";
  const CELL = 40;
  const RADIUS_SQ = 58 * 58;
  const MIN_NEIGHBORS = 4;
  const SECTORS = 16;
  const STATIONARY = SECTORS;
  const TAU = Math.PI * 2;
  const MIN_ALIGNMENT = Math.SQRT1_2;
  const OFFSETS = new Int8Array([0, -1, 1, -2, 2]);
  const SECTOR_OFFSETS = new Int8Array([0, -1, 1, -2, 2, -3, 3]);

  // Density-connected, similarly directed cores define a flock. Border birds
  // belong to one nearby core, but never join two cores through a sparse chain.
  // A 40px / 22.5-degree cell is an exact geometric and directional clique;
  // collapsing its core points avoids quadratic work inside dense flocks.
  class FlockGroups {
    constructor(capacity) {
      let buckets = 1;
      while (buckets < capacity * 2) buckets *= 2;
      this.mask = buckets - 1;
      this.buckets = new Int32Array(buckets);
      for (const key of ["cellX", "cellY", "sector", "head", "load", "coreHead", "parent", "slotCell", "next", "nextCore", "membership"])
        this[key] = new Int32Array(capacity);
      this.core = new Uint8Array(capacity);
      this.unitX = new Float64Array(capacity);
      this.unitY = new Float64Array(capacity);
      this.angle = new Float64Array(capacity);
      this.angleMin = new Float64Array(capacity);
      this.angleMax = new Float64Array(capacity);
      this.cells = 0;
    }
    hash(x, y, sector) {
      return (Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ Math.imul(sector, 83492791)) & this.mask;
    }
    lookup(x, y, sector) {
      let bucket = this.hash(x, y, sector);
      while (this.buckets[bucket] !== -1) {
        const c = this.buckets[bucket];
        if (this.cellX[c] === x && this.cellY[c] === y && this.sector[c] === sector) return c;
        bucket = (bucket + 1) & this.mask;
      }
      return -1;
    }
    root(c) {
      while (c !== this.parent[c]) {
        this.parent[c] = this.parent[this.parent[c]];
        c = this.parent[c];
      }
      return c;
    }
    aligned(i, j) {
      if (this.unitX[i] === 0 && this.unitY[i] === 0)
        return this.unitX[j] === 0 && this.unitY[j] === 0;
      return this.unitX[i] * this.unitX[j] + this.unitY[i] * this.unitY[j] >= MIN_ALIGNMENT - 1e-12;
    }
    build(sim) {
      this.buckets.fill(-1);
      this.membership.fill(-1);
      this.core.fill(0);
      this.cells = 0;
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a];
        if (sim.state[i] !== 1 && sim.state[i] !== 2) continue;
        const speed = Math.hypot(sim.vx[i], sim.vy[i]);
        const moving = speed > 1e-6;
        this.unitX[i] = moving ? sim.vx[i] / speed : 0;
        this.unitY[i] = moving ? sim.vy[i] / speed : 0;
        this.angle[i] = moving ? Math.atan2(sim.vy[i], sim.vx[i]) : 0;
        const x = Math.floor(sim.x[i] / CELL), y = Math.floor(sim.y[i] / CELL);
        const sector = moving
          ? Math.floor(((this.angle[i] + TAU) % TAU) * SECTORS / TAU) & (SECTORS - 1)
          : STATIONARY;
        let bucket = this.hash(x, y, sector);
        while (this.buckets[bucket] !== -1) {
          const c = this.buckets[bucket];
          if (this.cellX[c] === x && this.cellY[c] === y && this.sector[c] === sector) break;
          bucket = (bucket + 1) & this.mask;
        }
        let c = this.buckets[bucket];
        if (c === -1) {
          c = this.cells++;
          this.buckets[bucket] = c;
          this.cellX[c] = x;
          this.cellY[c] = y;
          this.sector[c] = sector;
          this.head[c] = this.coreHead[c] = -1;
          this.load[c] = 0;
          this.parent[c] = c;
        }
        this.slotCell[i] = c;
        this.next[i] = this.head[c];
        this.head[c] = i;
        this.load[c]++;
      }
    }
    isCore(sim, i) {
      const source = this.slotCell[i], sector = this.sector[source];
      let count = this.load[source] - 1;
      if (count >= MIN_NEIGHBORS) return true;
      const x = sim.x[i], y = sim.y[i];
      for (let oy = 0; oy < OFFSETS.length; oy++) {
        const cy = this.cellY[source] + OFFSETS[oy];
        const gapY = Math.max(cy * CELL - y, y - (cy + 1) * CELL, 0);
        for (let ox = 0; ox < OFFSETS.length; ox++) {
          const cx = this.cellX[source] + OFFSETS[ox];
          const gapX = Math.max(cx * CELL - x, x - (cx + 1) * CELL, 0);
          if (gapX * gapX + gapY * gapY > RADIUS_SQ) continue;
          const sectors = sector === STATIONARY ? 1 : SECTOR_OFFSETS.length;
          for (let os = 0; os < sectors; os++) {
            const s = sector === STATIONARY ? STATIONARY : (sector + SECTOR_OFFSETS[os] + SECTORS) & (SECTORS - 1);
            const c = this.lookup(cx, cy, s);
            if (c < 0 || c === source) continue;
            for (let j = this.head[c]; j !== -1; j = this.next[j]) {
              const dx = sim.x[j] - x, dy = sim.y[j] - y;
              if (dx * dx + dy * dy <= RADIUS_SQ && this.aligned(i, j) && ++count >= MIN_NEIGHBORS)
                return true;
            }
          }
        }
      }
      return false;
    }
    connectCores(sim) {
      for (let c = 0; c < this.cells; c++) {
        const first = this.coreHead[c];
        if (first < 0) continue;
        const anchor = this.angle[first];
        let min = anchor, max = anchor;
        for (let i = this.nextCore[first]; i !== -1; i = this.nextCore[i]) {
          let angle = this.angle[i];
          angle += TAU * Math.round((anchor - angle) / TAU);
          min = Math.min(min, angle);
          max = Math.max(max, angle);
        }
        this.angleMin[c] = min;
        this.angleMax[c] = max;
      }
      for (let c = 0; c < this.cells; c++) {
        if (this.coreHead[c] < 0) continue;
        const sector = this.sector[c];
        for (let oy = 0; oy < OFFSETS.length; oy++) {
          const dy = OFFSETS[oy], gapY = Math.max(0, Math.abs(dy) - 1) * CELL;
          for (let ox = 0; ox < OFFSETS.length; ox++) {
            const dx = OFFSETS[ox], gapX = Math.max(0, Math.abs(dx) - 1) * CELL;
            if (gapX * gapX + gapY * gapY > RADIUS_SQ) continue;
            const sectors = sector === STATIONARY ? 1 : SECTOR_OFFSETS.length;
            for (let os = 0; os < sectors; os++) {
              const s = sector === STATIONARY ? STATIONARY : (sector + SECTOR_OFFSETS[os] + SECTORS) & (SECTORS - 1);
              const other = this.lookup(this.cellX[c] + dx, this.cellY[c] + dy, s);
              if (other <= c || this.coreHead[other] < 0) continue;
              const left = this.root(c), right = this.root(other);
              if (left === right) continue;
              // Local alignment alone still percolates through gradually
              // turning birds. Require the complete core flock to stay coherent.
              const shift = TAU * Math.round(
                (this.angleMin[left] + this.angleMax[left] -
                 this.angleMin[right] - this.angleMax[right]) / (2 * TAU));
              const min = Math.min(this.angleMin[left], this.angleMin[right] + shift);
              const max = Math.max(this.angleMax[left], this.angleMax[right] + shift);
              if (max - min > Math.PI / 2 + 1e-12) continue;
              let connected = false;
              for (let i = this.coreHead[c]; i !== -1 && !connected; i = this.nextCore[i]) {
                for (let j = this.coreHead[other]; j !== -1; j = this.nextCore[j]) {
                  const x = sim.x[i] - sim.x[j], y = sim.y[i] - sim.y[j];
                  if (x * x + y * y <= RADIUS_SQ && this.aligned(i, j)) {
                    connected = true;
                    break;
                  }
                }
              }
              if (connected) {
                const root = Math.min(left, right);
                this.parent[Math.max(left, right)] = root;
                this.angleMin[root] = min;
                this.angleMax[root] = max;
              }
            }
          }
        }
      }
    }
    attachBorder(sim, i) {
      const source = this.slotCell[i], sector = this.sector[source];
      const x = sim.x[i], y = sim.y[i];
      let nearest = -1, distance = RADIUS_SQ;
      for (let oy = 0; oy < OFFSETS.length; oy++) {
        const cy = this.cellY[source] + OFFSETS[oy];
        const gapY = Math.max(cy * CELL - y, y - (cy + 1) * CELL, 0);
        for (let ox = 0; ox < OFFSETS.length; ox++) {
          const cx = this.cellX[source] + OFFSETS[ox];
          const gapX = Math.max(cx * CELL - x, x - (cx + 1) * CELL, 0);
          if (gapX * gapX + gapY * gapY > distance) continue;
          const sectors = sector === STATIONARY ? 1 : SECTOR_OFFSETS.length;
          for (let os = 0; os < sectors; os++) {
            const s = sector === STATIONARY ? STATIONARY : (sector + SECTOR_OFFSETS[os] + SECTORS) & (SECTORS - 1);
            const c = this.lookup(cx, cy, s);
            if (c < 0) continue;
            for (let j = this.coreHead[c]; j !== -1; j = this.nextCore[j]) {
              const dx = sim.x[j] - x, dy = sim.y[j] - y, squared = dx * dx + dy * dy;
              if (squared > distance || !this.aligned(i, j)) continue;
              if (squared < distance || nearest < 0 || sim.id[j] < sim.id[nearest]) {
                nearest = j;
                distance = squared;
              }
            }
          }
        }
      }
      // Noise owns a unique label; it cannot borrow leaders via another border.
      return nearest < 0 ? i : this.coreHead[this.root(this.slotCell[nearest])];
    }
    update(sim) {
      this.build(sim);
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a];
        if (sim.state[i] !== 1 && sim.state[i] !== 2) continue;
        if (!this.isCore(sim, i)) continue;
        const c = this.slotCell[i];
        this.core[i] = 1;
        this.nextCore[i] = this.coreHead[c];
        this.coreHead[c] = i;
      }
      this.connectCores(sim);
      for (let a = 0; a < sim.count; a++) {
        const i = sim.active[a];
        if (sim.state[i] !== 1 && sim.state[i] !== 2) continue;
        this.membership[i] = this.core[i]
          ? this.coreHead[this.root(this.slotCell[i])]
          : this.attachBorder(sim, i);
      }
      return this.membership;
    }
  }
  B.FlockGroups = FlockGroups;
})((globalThis.Boids = globalThis.Boids || {}));
