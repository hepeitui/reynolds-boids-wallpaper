const { test } = require("node:test");
const assert = require("node:assert/strict");
require("../history.js");

function simulation() {
  return {
    time: 0,
    state: new Uint8Array([2]),
    generation: new Uint32Array([1]),
    x: new Float32Array(1),
    y: new Float32Array(1),
    nx: new Float32Array(1),
    ny: new Float32Array(1),
    hue: new Float32Array([0.99]),
    previousHue: new Float32Array([0.99]),
    deathStarted: new Float64Array([-1]),
    deathGeneration: new Uint32Array(1),
    deathX: new Float32Array(1),
    deathY: new Float32Array(1),
  };
}

test("history samples hues continuously across the circular seam", () => {
  const history = new Boids.History(1);
  const sim = simulation();
  history.record(sim);

  sim.time = 1 / 60 + 1 / 120;
  sim.previousHue[0] = 0.99;
  sim.hue[0] = 1.01;
  sim.nx[0] = 0;
  sim.ny[0] = 0;
  sim.x[0] = 1;
  sim.y[0] = 1;
  history.record(sim);

  const index = history.head[0];
  const sampledHue = history.hue[index];
  assert.ok(Math.abs(sampledHue - 1) < 1e-6, `sampled hue ${sampledHue}`);
});

test("retired generation hue survives death and slot reuse", () => {
  const history = new Boids.History(1);
  const sim = simulation();
  sim.hue[0] = 0.7;
  sim.previousHue[0] = 0.7;
  history.record(sim);

  sim.time = 1 / 60;
  sim.nx[0] = 0;
  sim.ny[0] = 0;
  sim.x[0] = 1;
  sim.previousHue[0] = 0.7;
  sim.hue[0] = 0.72;
  history.record(sim);

  sim.time = 2 / 60;
  sim.state[0] = 4;
  sim.deathStarted[0] = 1.5 / 60;
  sim.deathGeneration[0] = 1;
  sim.deathX[0] = 1.5;
  sim.deathY[0] = 0;
  sim.nx[0] = 1;
  sim.ny[0] = 0;
  sim.x[0] = 2;
  sim.previousHue[0] = 0.72;
  sim.hue[0] = 0.74;
  history.record(sim);
  assert.equal(history.count[0], 0);

  sim.time = 3 / 60;
  sim.state[0] = 2;
  sim.generation[0] = 2;
  sim.x[0] = 100;
  sim.y[0] = 50;
  sim.nx[0] = 100;
  sim.ny[0] = 50;
  sim.previousHue[0] = 0.15;
  sim.hue[0] = 0.15;
  history.record(sim);

  const retired = [];
  history.forEachTrail(
    sim,
    (xs, ys, ts, base, head, count, length, slot, generation, hues) => {
      if (generation !== 1) return;
      let first = (head - count + 1) % length;
      if (first < 0) first += length;
      for (let sample = 0; sample < count; sample++)
        retired.push(hues[base + (first + sample) % length]);
    },
  );

  assert.equal(retired.length, 3);
  assert.ok(Math.abs(retired[0] - 0.7) < 1e-6, `retired hue ${retired[0]}`);
  assert.ok(Math.abs(retired[1] - 0.72) < 1e-6, `retired hue ${retired[1]}`);
  assert.ok(Math.abs(retired[2] - 0.73) < 1e-6, `retired hue ${retired[2]}`);
  assert.ok(Math.abs(history.hue[history.head[0]] - 0.15) < 1e-6);
});
