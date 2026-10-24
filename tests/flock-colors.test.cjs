const { test } = require("node:test");
const assert = require("node:assert/strict");
require("../flock-groups.js");
require("../flock-colors.js");
require("../neighbors.js");
require("../simulation.js");
require("../settings.js");

const delta = (a, b) => a - b - Math.floor(a - b + 0.5);
function scene(points, hues, heading = [1, 0]) {
  const n = points.length;
  const sim = {
    count: n, time: 0, initialSeed: 1234,
    active: Int32Array.from(points, (_, i) => i),
    state: new Uint8Array(n).fill(2),
    id: Uint32Array.from(points, (_, i) => i + 1),
    x: Float32Array.from(points, p => p[0]),
    y: Float32Array.from(points, p => p[1]),
    vx: new Float32Array(n).fill(heading[0]),
    vy: new Float32Array(n).fill(heading[1]),
    hue: new Float32Array(n), previousHue: new Float32Array(n),
  };
  sim.colors = new Boids.FlockColors(n);
  for (let i = 0; i < n; i++) {
    sim.colors.spawn(sim, i);
    sim.colors.birthHue[i] = sim.colors.target[i] = sim.hue[i] = sim.previousHue[i] = hues[i];
  }
  return sim;
}
function advance(sim, seconds) {
  let maxJump = 0;
  for (let step = 0; step < Math.round(seconds * 60); step++) {
    sim.time += 1 / 60;
    sim.colors.step(sim, 1 / 60);
    for (let i = 0; i < sim.count; i++)
      maxJump = Math.max(maxJump, Math.abs(delta(sim.hue[i], sim.previousHue[i])));
  }
  return maxJump;
}
const line = n => Array.from({ length: n }, (_, i) => [i * 12, 0]);

test("a sparse chain between dense flocks must not share their leaders", () => {
  const points = line(8).concat(line(8).map(([x, y]) => [x + 300, y]), [[128, 0], [176, 0], [224, 0], [272, 0]]);
  const sim = scene(points, [...Array(8).fill(0.1), ...Array(8).fill(0.45), ...Array(4).fill(0.3)]);
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.1)) < 0.01, `left flock borrowed distant leaders: ${sim.hue[0]}`);
  assert.ok(Math.abs(delta(sim.hue[8], 0.45)) < 0.01);
});

test("overlapping flocks flying in opposite directions keep independent colors", () => {
  const sim = scene(line(8).concat(line(8)), [...Array(8).fill(0.1), ...Array(8).fill(0.65)]);
  for (let i = 8; i < 16; i++) sim.vx[i] = -1;
  advance(sim, 12);
  for (let i = 0; i < 16; i++)
    assert.ok(Math.abs(delta(sim.hue[i], i < 8 ? 0.1 : 0.65)) < 0.01, `crossing flock contaminated bird ${i}: ${sim.hue[i]}`);
});

test("front three birds influence followers, not the whole flock average", () => {
  const a = scene(line(8), [0.1, 0.1, 0.1, 0.1, 0.1, 0.3, 0.3, 0.3]);
  const b = scene(line(8), [0.1, 0.6, 0.7, 0.8, 0.9, 0.3, 0.3, 0.3]);
  advance(a, 10);
  advance(b, 10);
  assert.ok(Math.abs(delta(a.hue[0], 0.23)) < 0.001);
  assert.ok(Math.abs(delta(a.hue[0], b.hue[0])) < 1e-6);
});

test("front follows flight direction; changing leaders changes color smoothly", () => {
  const sim = scene(line(8), [0.2, 0.2, 0.2, 0.2, 0.2, 0.65, 0.65, 0.65]);
  advance(sim, 10);
  const influenced = sim.hue[0];
  assert.ok(delta(influenced, 0.2) > 0.28);
  sim.vx.fill(-1);
  assert.ok(advance(sim, 10) < 0.0045);
  assert.ok(Math.abs(delta(sim.hue[0], 0.2)) < 0.005);
  assert.ok(delta(0.65, sim.hue[7]) > 0.28);
});

test("followers prefer leader color while preserving their immutable birth color", () => {
  const hues = [0.1, 0.25, 0.1, 0.1, 0.1, 0.5, 0.5, 0.5];
  const sim = scene(line(8), hues);
  advance(sim, 30);
  for (let i = 0; i < sim.count; i++)
    assert.equal(sim.colors.birthHue[i], Math.fround(hues[i]));
  assert.ok(Math.abs(delta(sim.hue[0], 0.36)) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[1], 0.4125)) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[0], 0.5)) < Math.abs(delta(sim.hue[0], 0.1)));
  assert.ok(Math.abs(delta(sim.hue[0], sim.hue[1])) > 0.04);
});

test("each leader slowly returns to its own color, without following other leaders", () => {
  const sim = scene(line(6), [0.1, 0.1, 0.1, 0.45, 0.6, 0.75]);
  for (let i = 3; i < 6; i++) sim.hue[i] = sim.previousHue[i] = 0.25;
  assert.ok(advance(sim, 1) < 0.007);
  for (let i = 3; i < 6; i++) {
    const remaining = Math.abs(delta(sim.hue[i], sim.colors.birthHue[i]));
    assert.ok(remaining < Math.abs(delta(0.25, sim.colors.birthHue[i])));
    assert.ok(remaining > 0.01, "returning to birth color must not snap");
  }
  advance(sim, 20);
  for (let i = 3; i < 6; i++)
    assert.ok(Math.abs(delta(sim.hue[i], sim.colors.birthHue[i])) < 1e-5);
});

test("followers track the leaders' changing current colors, not their eventual birth colors", () => {
  const sim = scene(line(6), [0.1, 0.1, 0.1, 0.5, 0.5, 0.5]);
  for (let i = 3; i < 6; i++) sim.hue[i] = sim.previousHue[i] = 0.2;
  advance(sim, 1 / 60);
  assert.ok(sim.hue[0] > 0.1 && sim.hue[0] < 0.102);
  assert.ok(sim.hue[5] > 0.2 && sim.hue[5] < 0.21);
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.36)) < 0.002);
});

test("separate flocks with different leaders give identical-born followers different colors", () => {
  const points = line(6).concat(line(6).map(([x, y]) => [x + 500, y + 200]));
  const sim = scene(points, [0.2, 0.2, 0.2, 0, 0, 0, 0.2, 0.2, 0.2, 0.5, 0.5, 0.5]);
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.07)) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[6], 0.395)) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[0], sim.hue[6])) > 0.32);
});

test("splitting away from leaders restores the original color rather than inheriting a group average", () => {
  const sim = scene(line(8), [0.2, 0.2, 0.2, 0.2, 0.2, 0.65, 0.65, 0.65]);
  advance(sim, 10);
  sim.x[0] -= 400;
  assert.ok(advance(sim, 12) < 0.0045);
  assert.ok(Math.abs(delta(sim.hue[0], 0.2)) < 0.001);
});

test("translation and rotation preserve leader color behavior", () => {
  const points = line(8), hues = [0.1, 0.2, 0.3, 0.4, 0.5, 0.7, 0.7, 0.7];
  const a = scene(points, hues);
  const b = scene(points.map(([x, y]) => [997 - y, x - 263]), hues, [0, 1]);
  advance(a, 10);
  advance(b, 10);
  for (let i = 0; i < a.count; i++) assert.ok(Math.abs(delta(a.hue[i], b.hue[i])) < 1e-6);
});

test("circular leader colors cross red continuously instead of taking the long route", () => {
  const sim = scene(line(5), [0.99, 0.99, 0.02, 0.02, 0.02]);
  assert.ok(advance(sim, 10) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[0], 0.0095)) < 0.001);
  for (const hue of sim.hue) assert.ok(hue >= 0 && hue < 1);
});

test("stationary and isolated birds keep their birth color; dying birds neither lead nor change", () => {
  const still = scene(line(4), [0.1, 0.3, 0.5, 0.8], [0, 0]);
  advance(still, 10);
  for (let i = 0; i < still.count; i++) assert.equal(still.hue[i], still.colors.birthHue[i]);
  const sim = scene(line(5), [0.2, 0.2, 0.2, 0.8, 0.8]);
  sim.state[3] = sim.state[4] = 4;
  advance(sim, 10);
  for (let i = 0; i < sim.count; i++) assert.ok(Math.abs(delta(sim.hue[i], i < 3 ? 0.2 : 0.8)) < 1e-6);
});

test("slot reuse chooses a fresh birth color without consuming movement randomness", () => {
  const settings = { ...Boids.Settings.defaults, count: 2 };
  const sim = new Boids.Simulation(settings, 1280, 720);
  sim.spawn();
  const first = sim.hue[0], seed = sim.seed;
  sim.colors.spawn(sim, 0);
  assert.equal(sim.seed, seed);
  sim.state[0] = 0;
  sim.count = 0;
  sim.spawn();
  assert.notEqual(sim.hue[0], first);
  assert.equal(sim.hue[0], sim.previousHue[0]);
  assert.equal(sim.hue[0], sim.colors.birthHue[0]);
});

test("dense same-direction flocks merge and split without retaining borrowed color", () => {
  const points = line(6).concat(line(6).map(([x, y]) => [x + 200, y]));
  const hues = Array(6).fill(0.1).concat([0.4, 0.4, 0.4, 0.7, 0.7, 0.7]);
  const sim = scene(points, hues);
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.1)) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[6], 0.595)) < 0.001);

  for (let i = 6; i < 12; i++) sim.x[i] = 72 + (i - 6) * 12;
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.84)) < 0.001);

  for (let i = 0; i < 6; i++) sim.x[i] -= 400;
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.1)) < 0.001);
  assert.ok(Math.abs(delta(sim.hue[6], 0.595)) < 0.001);
});

test("gradually turning dense flocks retain their own leaders' colors", () => {
  const sim = scene(Array.from({ length: 35 }, () => [0, 0]),
    Array.from({ length: 35 }, (_, i) => i < 20 ? 0.1 : 0.65));
  for (let i = 0; i < 35; i++) {
    const angle = Math.floor(i / 5) * Math.PI / 6;
    sim.vx[i] = Math.cos(angle);
    sim.vy[i] = Math.sin(angle);
  }
  advance(sim, 12);
  assert.ok(Math.abs(delta(sim.hue[0], 0.1)) < 0.01);
  assert.ok(Math.abs(delta(sim.hue[34], 0.65)) < 0.01);
});
