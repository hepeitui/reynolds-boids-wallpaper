(function (B) {
  "use strict";
  const TAU = Math.PI * 2;
  function edgeForce(distance, band) {
    if (band <= 0 || distance >= band) return 0;
    return (
      Math.expm1(2 * Math.min(2, Math.max(0, 1 - distance / band))) /
      Math.expm1(2)
    );
  }
  const clamp = (value, min, max) => Math.max(min, Math.min(max, value));
  function boundaryContact(x, y, nx, ny, width, height) {
    if (x <= 0 || x >= width || y <= 0 || y >= height) return 0;
    const dx = nx - x,
      dy = ny - y;
    let contact = Infinity;
    if (dx < 0 && nx <= 0) contact = Math.min(contact, -x / dx);
    else if (dx > 0 && nx >= width)
      contact = Math.min(contact, (width - x) / dx);
    if (dy < 0 && ny <= 0) contact = Math.min(contact, -y / dy);
    else if (dy > 0 && ny >= height)
      contact = Math.min(contact, (height - y) / dy);
    return contact <= 1 ? contact : -1;
  }
  function livePopulation(sim, population) {
    let count = 0,
      dying = 0;
    for (let a = 0; a < sim.count; a++) {
      const state = sim.state[sim.active[a]];
      if (state === 4) dying++;
      else if (state !== 3) count++;
    }
    population.count = count;
    population.dying = dying;
  }
  function addBudgetedBirths(sim, desired, target, dying) {
    while (
      desired + dying < target &&
      sim.count < sim.capacity &&
      sim.spawnCredit >= 1
    ) {
      sim.spawn();
      sim.spawnCredit--;
      desired++;
    }
    return desired;
  }
  class Simulation {
    constructor(settings, width, height, seed = 20261022) {
      this.capacity = 3000;
      this.settings = settings;
      this.width = width;
      this.height = height;
      this.seed = seed >>> 0;
      this.workArea = { left: 0, top: 0, right: width, bottom: height };
      this.bounds = { left: 0, top: 0, right: width, bottom: height };
      this.updateBounds();
      this.initialSeed = this.seed;
      this.nextId = 1;
      this.time = 0;
      this.deathDuration = 0.28;
      this._population = { count: 0, dying: 0 };
      this.count = 0;
      for (const key of [
        "x",
        "y",
        "vx",
        "vy",
        "nx",
        "ny",
        "nvx",
        "nvy",
        "error",
        "boost",
        "boostUntil",
        "exitX",
        "exitY",
      ])
        this[key] = new Float32Array(this.capacity);
      this._unitX = new Float64Array(this.capacity);
      this._unitY = new Float64Array(this.capacity);
      this.id = new Uint32Array(this.capacity);
      this.generation = new Uint32Array(this.capacity);
      this.state = new Uint8Array(this.capacity);
      this.deathProgress = new Float32Array(this.capacity);
      this.deathStarted = new Float64Array(this.capacity);
      this.deathStarted.fill(-1);
      this.deathX = new Float32Array(this.capacity);
      this.deathY = new Float32Array(this.capacity);
      this.deathGeneration = new Uint32Array(this.capacity);
      this.active = new Int32Array(this.capacity);
      this.neighbors = new B.Neighbors(this.capacity);
      this.pointer = { x: 0, y: 0, active: false };
      this.pulse = { x: 0, y: 0, until: -1 };
      this.spawnCredit = 0;
      this.stats = {
        alive: 0,
        entering: 0,
        leaving: 0,
        simulationMs: 0,
        removed: 0,
        born: 0,
        dying: 0,
        boundaryDeaths: 0,
      };
    }
    random() {
      let x = this.seed;
      x ^= x << 13;
      x ^= x >>> 17;
      x ^= x << 5;
      this.seed = x >>> 0;
      return this.seed / 4294967296;
    }
    configure(settings) {
      const old = this.settings;
      this.settings = settings;
      if (
        old.k !== settings.k ||
        old.mode !== settings.mode ||
        old.graphHz !== settings.graphHz
      )
        this.neighbors.invalidate();
      this.updateBounds();
    }
    updateBounds() {
      const s = this.settings, area = this.workArea, b = this.bounds;
      // Keep a usable interior even on a small preview or with opposing large margins.
      const left = Math.max(0, area.left), top = Math.max(0, area.top);
      const right = Math.min(this.width, area.right), bottom = Math.min(this.height, area.bottom);
      const sx = Math.min(1, Math.max(0, right - left - 32) /
        Math.max(1, (s.marginLeft || 0) + (s.marginRight || 0)));
      const sy = Math.min(1, Math.max(0, bottom - top - 32) /
        Math.max(1, (s.marginTop || 0) + (s.marginBottom || 0)));
      b.left = left + (s.marginLeft || 0) * sx;
      b.right = right - (s.marginRight || 0) * sx;
      b.top = top + (s.marginTop || 0) * sy;
      b.bottom = bottom - (s.marginBottom || 0) * sy;
      // A setting/display change must not kill a whole strip of existing birds.
      for (let a = 0; a < this.count; a++) {
        const i = this.active[a];
        if (this.state[i] === 2 &&
            (this.x[i] <= b.left || this.x[i] >= b.right ||
             this.y[i] <= b.top || this.y[i] >= b.bottom))
          this.state[i] = 1;
      }
    }
    resize(w, h, workArea) {
      this.width = Math.max(1, w);
      this.height = Math.max(1, h);
      this.workArea = workArea || { left: 0, top: 0, right: this.width, bottom: this.height };
      this.updateBounds();
      // A previously off-screen exit can become interior after enlarging a display.
      // Retarget departures without moving birds or replacing their identities.
      for (let a = 0; a < this.count; a++) {
        const i = this.active[a];
        if (this.state[i] === 3) this.leave(i);
      }
      this.neighbors.invalidate();
    }
    click(x, y) {
      this.pulse.x = x;
      this.pulse.y = y;
      this.pulse.until = this.time + 1.2;
      for (let a = 0; a < this.count; a++) {
        const i = this.active[a];
        if ((this.x[i] - x) ** 2 + (this.y[i] - y) ** 2 < 180 ** 2)
          this.boostUntil[i] = this.time + 2;
      }
      this.neighbors.invalidate();
    }
    spawn() {
      let i = 0;
      while (i < this.capacity && this.state[i]) i++;
      if (i === this.capacity) return;
      const side = Math.floor(this.random() * 4),
        b = this.bounds,
        w = b.right - b.left,
        h = b.bottom - b.top;
      this.x[i] = side === 0 ? b.left - 24 : side === 1 ? b.right + 24 : b.left + this.random() * w;
      this.y[i] = side === 2 ? b.top - 24 : side === 3 ? b.bottom + 24 : b.top + this.random() * h;
      const angle = Math.atan2(
        b.top + h * (0.25 + this.random() * 0.5) - this.y[i],
        b.left + w * (0.25 + this.random() * 0.5) - this.x[i],
      );
      this.error[i] = this.random() * 2 - 1;
      const speed =
        this.settings.speed * (1 + this.error[i] * this.settings.speedError);
      this.vx[i] = Math.cos(angle) * speed;
      this.vy[i] = Math.sin(angle) * speed;
      this.id[i] = this.nextId++;
      this.generation[i]++;
      this.state[i] = 1;
      this.boost[i] = 0;
      this.deathProgress[i] = 0;
      // Retain contact metadata so history can retire the prior generation after slot reuse.
      this.boostUntil[i] = -1;
      this.active[this.count++] = i;
      this.stats.born++;
    }
    leave(i) {
      const x = this.x[i],
        y = this.y[i],
        w = this.width,
        h = this.height;
      const d = Math.min(x, w - x, y, h - y);
      this.exitX[i] = x;
      this.exitY[i] = y;
      if (d === x) this.exitX[i] = -80;
      else if (d === w - x) this.exitX[i] = w + 80;
      else if (d === y) this.exitY[i] = -80;
      else this.exitY[i] = h + 80;
      this.state[i] = 3;
    }
    replenish() {
      livePopulation(this, this._population);
      addBudgetedBirths(
        this,
        this._population.count,
        this.settings.count,
        this._population.dying,
      );
    }
    _startDeath(i, x, y, startedAt) {
      this.state[i] = 4;
      this.deathStarted[i] = startedAt;
      this.deathGeneration[i] = this.generation[i];
      this.deathX[i] = x;
      this.deathY[i] = y;
      this.deathProgress[i] = clamp(
        (this.time - startedAt) / this.deathDuration,
        0,
        1,
      );
      this.nx[i] = x;
      this.ny[i] = y;
      this.neighbors.invalidate();
      this.stats.boundaryDeaths++;
    }
    reconcile(dt) {
      livePopulation(this, this._population);
      let desired = this._population.count;
      const dying = this._population.dying;
      const target = this.settings.count;
      // No queued operations: every step converges against the latest target.
      if (desired < target)
        for (let a = 0; a < this.count && desired < target; a++) {
          const i = this.active[a];
          if (this.state[i] === 3) {
            this.state[i] = 1;
            desired++;
          }
        }
      if (desired > target) {
        let budget = Math.max(1, Math.ceil(450 * dt));
        for (
          let a = this.count - 1;
          a >= 0 && desired > target && budget > 0;
          a--
        ) {
          const i = this.active[a];
          if (this.state[i] !== 3 && this.state[i] !== 4) {
            this.leave(i);
            desired--;
            budget--;
          }
        }
      }
      this.spawnCredit = Math.min(30, this.spawnCredit + 450 * dt);
      addBudgetedBirths(this, desired, target, dying);
    }
    step(dt) {
      const start = performance.now(),
        stepStart = this.time;
      this.time += dt;
      this.reconcile(dt);
      const s = this.settings,
        w = this.width,
        h = this.height,
        b = this.bounds,
        bw = b.right - b.left,
        bh = b.bottom - b.top,
        // A fast bird needs room for its finite turning radius, not just a percentage.
        turnRoom = s.speed * (1 + s.speedError) / 2.4 * 2,
        bandX = s.edge > 0 ? Math.max(bw * s.edge, turnRoom) : 0,
        bandY = s.edge > 0 ? Math.max(bh * s.edge, turnRoom) : 0;
      this.neighbors.update(this, dt);
      let entering = 0,
        leaving = 0;
      for (let a = 0; a < this.count; a++) {
        const i = this.active[a];
        if (this.state[i] === 4) {
          this._unitX[i] = 0;
          this._unitY[i] = 0;
          continue;
        }
        const norm = Math.hypot(this.vx[i], this.vy[i]) || 1;
        this._unitX[i] = this.vx[i] / norm;
        this._unitY[i] = this.vy[i] / norm;
      }
      for (let a = 0; a < this.count; a++) {
        const i = this.active[a],
          x = this.x[i],
          y = this.y[i],
          ux = this._unitX[i],
          uy = this._unitY[i];
        const state = this.state[i];
        if (state === 4) {
          if (this.deathStarted[i] < 0) {
            this.deathStarted[i] = stepStart;
            this.deathGeneration[i] = this.generation[i];
            this.deathX[i] = x;
            this.deathY[i] = y;
          }
          this.deathProgress[i] = clamp(
            (this.time - this.deathStarted[i]) / this.deathDuration,
            0,
            1,
          );
          this.nx[i] = x;
          this.ny[i] = y;
          // Keep the impact heading for rendering, but never integrate its position.
          this.nvx[i] = this.vx[i];
          this.nvy[i] = this.vy[i];
          continue;
        }
        let fx = 0,
          fy = 0;
        if (this.state[i] === 3) {
          fx = (this.exitX[i] - x) * 0.06;
          fy = (this.exitY[i] - y) * 0.06;
          leaving++;
        } else {
          let cx = 0,
            cy = 0,
            ax = 0,
            ay = 0,
            sx = 0,
            sy = 0;
          let n;
          if (s.mode === "radius") {
            const sums = this.neighbors.aggregateRadius(
              this,
              i,
              this._unitX,
              this._unitY,
            );
            n = sums[0];
            cx = sums[1];
            cy = sums[2];
            ax = sums[3];
            ay = sums[4];
            sx = sums[5];
            sy = sums[6];
          } else {
            n = this.neighbors.query(this, i);
            for (let q = 0; q < n; q++) {
              const j = this.neighbors.result[q];
              cx += this.x[j] - x;
              cy += this.y[j] - y;
              ax += this._unitX[j];
              ay += this._unitY[j];
            }
            if (s.separation > 0) {
              const sn = this.neighbors.querySeparation(this, i);
              for (let q = 0; q < sn; q++) {
                const j = this.neighbors.result[q],
                  dx = this.x[j] - x,
                  dy = this.y[j] - y,
                  d = this.neighbors.dist[q];
                if (d > 1e-4) {
                  const f = (1 - d / 15) / d;
                  sx -= dx * f;
                  sy -= dy * f;
                }
              }
            }
          }
          if (n) {
            fx += (s.cohesion * cx) / n / 58 + s.alignment * (ax / n - ux);
            fy += (s.cohesion * cy) / n / 58 + s.alignment * (ay / n - uy);
          }
          fx += sx * s.separation * 2;
          fy += sy * s.separation * 2;
          const pushX = edgeForce(x - b.left, bandX) - edgeForce(b.right - x, bandX),
            pushY = edgeForce(y - b.top, bandY) - edgeForce(b.bottom - y, bandY);
          fx += 6 * pushX;
          fy += 6 * pushY;
          // Break the head-on symmetry: pure braking cannot rotate a unit heading.
          const urgency = Math.max(0, -ux * pushX - uy * pushY);
          const turn = (this.id[i] & 1) ? 1 : -1;
          fx += -uy * urgency * turn * 3;
          fy += ux * urgency * turn * 3;
          if (state === 1) {
            entering++;
            fx += ((b.left + bw * 0.5 - x) / bw) * 2.2;
            fy += ((b.top + bh * 0.5 - y) / bh) * 2.2;
            if (x > b.left + 2 && x < b.right - 2 && y > b.top + 2 && y < b.bottom - 2) this.state[i] = 2;
          }
        }
        if (this.pointer.active) {
          const dx = x - this.pointer.x,
            dy = y - this.pointer.y,
            d = Math.hypot(dx, dy);
          if (d < 100 && d > 0.001) {
            fx += (dx / d) * (1 - d / 100) * 2;
            fy += (dy / d) * (1 - d / 100) * 2;
          }
        }
        if (this.pulse.until > this.time) {
          const dx = x - this.pulse.x,
            dy = y - this.pulse.y,
            d = Math.hypot(dx, dy);
          if (d < 180 && d > 0.001) {
            const f =
              ((this.pulse.until - this.time) / 1.2) * (1 - d / 180) * 8;
            fx += (dx / d) * f;
            fy += (dy / d) * f;
          }
        }
        const desiredAngle = Math.atan2(uy + fy * 2.8 * dt, ux + fx * 2.8 * dt),
          angle = Math.atan2(uy, ux);
        let delta = ((desiredAngle - angle + Math.PI * 3) % TAU) - Math.PI;
        const maxTurn = 2.4 * dt;
        delta = Math.max(-maxTurn, Math.min(maxTurn, delta));
        const boostTarget = this.boostUntil[i] > this.time ? 0.1 : 0;
        this.boost[i] += (boostTarget - this.boost[i]) * Math.min(1, dt * 6);
        const speed =
          s.speed * (1 + this.error[i] * s.speedError) * (1 + this.boost[i]);
        this.nvx[i] = Math.cos(angle + delta) * speed;
        this.nvy[i] = Math.sin(angle + delta) * speed;
        const nextX = x + this.nvx[i] * dt,
          nextY = y + this.nvy[i] * dt;
        this.nx[i] = nextX;
        this.ny[i] = nextY;
        if (this.state[i] === 2) {
          const fraction = boundaryContact(x - b.left, y - b.top,
            nextX - b.left, nextY - b.top, bw, bh);
          if (fraction >= 0) {
            const outside = x < b.left || x > b.right || y < b.top || y > b.bottom;
            const contactX = clamp(
                outside ? x : x + (nextX - x) * fraction,
                b.left,
                b.right,
              ),
              contactY = clamp(outside ? y : y + (nextY - y) * fraction, b.top, b.bottom);
            this._startDeath(i, contactX, contactY, stepStart + fraction * dt);
          }
        }
      }
      let t = this.x;
      this.x = this.nx;
      this.nx = t;
      t = this.y;
      this.y = this.ny;
      this.ny = t;
      t = this.vx;
      this.vx = this.nvx;
      this.nvx = t;
      t = this.vy;
      this.vy = this.nvy;
      this.nvy = t;
      for (let a = this.count - 1; a >= 0; a--) {
        const i = this.active[a],
          state = this.state[i];
        const departed =
          state === 3 &&
          (this.x[i] < -50 ||
            this.x[i] > w + 50 ||
            this.y[i] < -50 ||
            this.y[i] > h + 50);
        const expiredDeath =
          state === 4 && this.time - this.deathStarted[i] >= this.deathDuration;
        if (departed || expiredDeath) {
          if (expiredDeath) this.deathProgress[i] = 1;
          this.state[i] = 0;
          this.active[a] = this.active[--this.count];
          this.stats.removed++;
        }
      }
      const birthsBefore = this.stats.born;
      this.replenish();
      entering += this.stats.born - birthsBefore;
      let dying = 0;
      for (let a = 0; a < this.count; a++)
        if (this.state[this.active[a]] === 4) dying++;
      this.stats.alive = this.count;
      this.stats.entering = entering;
      this.stats.leaving = leaving;
      this.stats.dying = dying;
      this.stats.simulationMs = performance.now() - start;
    }
  }
  B.edgeForce = edgeForce;
  B.Simulation = Simulation;
})((globalThis.Boids = globalThis.Boids || {}));
