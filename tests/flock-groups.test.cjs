const { test } = require("node:test");
const assert = require("node:assert/strict");
require("../flock-groups.js");

function scene(points) {
  return {
    count: points.length,
    active: Int32Array.from(points, (_, i) => i),
    state: new Uint8Array(points.length).fill(2),
    id: Uint32Array.from(points, (_, i) => i + 1),
    x: Float32Array.from(points, p => p[0]),
    y: Float32Array.from(points, p => p[1]),
    vx: Float64Array.from(points, p => Math.cos(p[2] || 0)),
    vy: Float64Array.from(points, p => Math.sin(p[2] || 0)),
  };
}
function reference(sim) {
  const n = sim.count, neighbors = Array.from({ length: n }, () => []);
  const live = i => sim.state[i] === 1 || sim.state[i] === 2;
  const unit = Array.from({ length: n }, (_, i) => {
    const speed = Math.hypot(sim.vx[i], sim.vy[i]);
    return speed > 1e-6 ? [sim.vx[i] / speed, sim.vy[i] / speed] : [0, 0];
  });
  for (let i = 0; i < n; i++) {
    if (!live(i)) continue;
    for (let j = i + 1; j < n; j++) {
      if (!live(j)) continue;
      const [ux, uy] = unit[i], [vx, vy] = unit[j];
      const aligned = ux === 0 && uy === 0 ? vx === 0 && vy === 0 : ux * vx + uy * vy >= Math.SQRT1_2 - 1e-12;
      if (aligned && (sim.x[i] - sim.x[j]) ** 2 + (sim.y[i] - sim.y[j]) ** 2 <= 58 ** 2) {
        neighbors[i].push(j);
        neighbors[j].push(i);
      }
    }
  }
  const core = neighbors.map(list => list.length >= 4), parent = Array.from({ length: n }, (_, i) => i);
  const root = i => parent[i] === i ? i : (parent[i] = root(parent[i]));
  for (let i = 0; i < n; i++)
    if (core[i]) for (const j of neighbors[i]) if (core[j]) parent[root(j)] = root(i);
  const nearest = new Int32Array(n).fill(-1);
  const membership = Array.from({ length: n }, (_, i) => {
    if (!live(i)) return -1;
    if (core[i]) return root(i);
    const candidates = neighbors[i].filter(j => core[j]);
    candidates.sort((a, b) => {
      const distance = j => (sim.x[i] - sim.x[j]) ** 2 + (sim.y[i] - sim.y[j]) ** 2;
      return distance(a) - distance(b) || sim.id[a] - sim.id[b];
    });
    nearest[i] = candidates.length ? candidates[0] : -1;
    return candidates.length ? root(candidates[0]) : i;
  });
  return { membership, neighbors, core, nearest, unit };
}
function samePartition(actual, expected) {
  for (let i = 0; i < expected.length; i++) {
    if (expected[i] < 0) {
      assert.equal(actual[i], -1);
      continue;
    }
    assert.ok(actual[i] >= 0);
    for (let j = 0; j < i; j++)
      if (expected[j] >= 0)
        assert.equal(actual[i] === actual[j], expected[i] === expected[j], `membership mismatch: ${i}, ${j}`);
  }
}

test("radius and heading thresholds include the boundary but reject beyond it", () => {
  const points = [...Array.from({ length: 5 }, () => [0, 0, 0]), ...Array.from({ length: 5 }, () => [58, 0, Math.PI / 4])];
  const sim = scene(points), groups = new Boids.FlockGroups(10);
  let membership = groups.update(sim);
  assert.equal(membership[0], membership[5]);
  sim.x.fill(58.001, 5);
  membership = groups.update(sim);
  assert.notEqual(membership[0], membership[5]);
  sim.x.fill(58, 5);
  for (let i = 5; i < 10; i++) {
    sim.vx[i] = Math.cos(Math.PI / 4 + 0.001);
    sim.vy[i] = Math.sin(Math.PI / 4 + 0.001);
  }
  membership = groups.update(sim);
  assert.notEqual(membership[0], membership[5]);
});

test("a border bird attaches to one nearest core without joining both flocks", () => {
  const sim = scene([[-70, 0], [-70, 0], [-70, 0], [-70, 0], [-55, 0], [70, 0], [70, 0], [70, 0], [70, 0], [55, 0], [0, 0]]);
  const groups = new Boids.FlockGroups(sim.count);
  const original = Array.from(groups.update(sim));
  assert.equal(original[10], original[0]);
  assert.notEqual(original[0], original[5]);
  sim.active.reverse();
  samePartition(groups.update(sim), original);
});

test("dense colocated stationary and moving birds do not borrow one another's flock", () => {
  const sim = scene(Array.from({ length: 15 }, () => [0, 0]));
  sim.vx.fill(0, 0, 5);
  sim.vx.fill(-1, 10);
  const membership = new Boids.FlockGroups(15).update(sim);
  assert.notEqual(membership[0], membership[5]);
  assert.notEqual(membership[5], membership[10]);
  for (let i = 0; i < 15; i++) assert.equal(membership[i], membership[Math.floor(i / 5) * 5]);
});

test("density, border ownership and core coherence match a brute-force neighbor reference", () => {
  let seed = 0x16345;
  const random = () => {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return (seed >>> 0) / 4294967296;
  };
  for (let trial = 0; trial < 8; trial++) {
    const points = Array.from({ length: 150 }, (_, i) => {
      const cluster = i % 6;
      return [(cluster % 3) * 95 + random() * 110 - 55, Math.floor(cluster / 3) * 100 + random() * 110 - 55,
        cluster * Math.PI / 3 + (random() - 0.5) * Math.PI / 2];
    });
    const sim = scene(points);
    for (let i = 0; i < 150; i += 19) sim.state[i] = i % 2 ? 3 : 4;
    const groups = new Boids.FlockGroups(150), actual = groups.update(sim);
    const expected = reference(sim), coherent = new Map();
    for (let i = 0; i < 150; i++) {
      assert.equal(Boolean(groups.core[i]), expected.core[i], `core density mismatch: ${i}`);
      if (!expected.core[i]) continue;
      const label = expected.membership[i];
      if (!coherent.has(label)) coherent.set(label, true);
      for (let j = 0; j < i; j++) {
        if (!expected.core[j] || expected.membership[j] !== label) continue;
        if (expected.unit[i][0] * expected.unit[j][0] + expected.unit[i][1] * expected.unit[j][1] < -1e-10)
          coherent.set(label, false);
      }
    }
    for (let i = 0; i < 150; i++) {
      if (expected.membership[i] < 0) {
        assert.equal(actual[i], -1);
        continue;
      }
      assert.ok(actual[i] >= 0);
      if (!expected.core[i]) {
        const nearest = expected.nearest[i];
        if (nearest >= 0) assert.equal(actual[i], actual[nearest], "border must follow its nearest compatible core");
        else for (let j = 0; j < 150; j++) if (j !== i) assert.notEqual(actual[i], actual[j], "noise must stay independent");
        continue;
      }
      for (let j = 0; j < i; j++) {
        if (!expected.core[j]) continue;
        if (expected.membership[i] !== expected.membership[j]) assert.notEqual(actual[i], actual[j]);
        else if (coherent.get(expected.membership[i])) assert.equal(actual[i], actual[j], "a coherent dense flock must not fragment");
        if (actual[i] === actual[j])
          assert.ok(expected.unit[i][0] * expected.unit[j][0] + expected.unit[i][1] * expected.unit[j][1] >= -1e-10);
      }
      const reached = new Set([i]), queue = [i];
      for (const from of queue)
        for (const to of expected.neighbors[from])
          if (expected.core[to] && actual[to] === actual[i] && !reached.has(to)) {
            reached.add(to);
            queue.push(to);
          }
      for (let j = 0; j < 150; j++)
        if (expected.core[j] && actual[j] === actual[i]) assert.ok(reached.has(j), "one core flock must remain spatially connected");
    }
  }
});

test("rotation, translation, and heading-sector wrap preserve flock membership", () => {
  const points = Array.from({ length: 40 }, (_, i) => [i % 8 * 12, Math.floor(i / 8) * 30, (i % 2 ? 0.02 : -0.02)]);
  const a = scene(points), rotation = Math.PI * 0.73;
  const b = scene(points.map(([x, y, angle]) => [x * Math.cos(rotation) - y * Math.sin(rotation) - 125,
    x * Math.sin(rotation) + y * Math.cos(rotation) + 257, angle + rotation]));
  samePartition(new Boids.FlockGroups(40).update(a), new Boids.FlockGroups(40).update(b));
});

test("gradually turning dense neighbors cannot transitively join opposite-heading flocks", () => {
  const points = Array.from({ length: 35 }, (_, i) => [0, 0, Math.floor(i / 5) * Math.PI / 6]);
  const sim = scene(points), groups = new Boids.FlockGroups(35);
  const membership = groups.update(sim);
  assert.notEqual(membership[0], membership[34]);
  for (let i = 0; i < sim.count; i++)
    for (let j = 0; j < i; j++)
      if (membership[i] === membership[j])
        assert.ok(sim.vx[i] * sim.vx[j] + sim.vy[i] * sim.vy[j] >= -1e-10,
          "a coherent core flock must fit within a 90-degree heading span");
});
