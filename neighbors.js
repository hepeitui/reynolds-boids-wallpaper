(function (root) {
  "use strict";

  var Boids = (root.Boids = root.Boids || {});
  var CELL_SIZE = 40;
  var RADIUS = 58;
  var SEPARATION_RADIUS = 15;
  var RADIUS_SQ = RADIUS * RADIUS;
  var SEPARATION_RADIUS_SQ = SEPARATION_RADIUS * SEPARATION_RADIUS;

  function isLive(sim, slot) {
    var state = sim.state[slot];
    return state === 1 || state === 2;
  }

  function now() {
    return root.performance && typeof root.performance.now === "function"
      ? root.performance.now()
      : Date.now();
  }

  function nextPowerOfTwo(value) {
    var result = 1;
    while (result < value && result < 0x40000000) result <<= 1;
    return result;
  }

  function Neighbors(capacity) {
    this.capacity = Math.max(0, Math.floor(Number(capacity) || 0));
    this.radius = RADIUS;
    this.separationRadius = SEPARATION_RADIUS;
    this.cellSize = CELL_SIZE;
    this.result = new Int32Array(this.capacity);
    this.dist = new Float32Array(this.capacity);
    this._resultDistSq = new Float64Array(this.capacity);

    this._bucketCount = nextPowerOfTwo(Math.max(1, this.capacity * 2));
    this._bucketMask = this._bucketCount - 1;
    this._bucketHeads = new Int32Array(this._bucketCount);
    this._bucketHeads.fill(-1);
    this._bucketLoads = new Uint32Array(this._bucketCount);
    this._occupancyHistogram = new Uint32Array(this.capacity + 1);
    this._aggregateX = new Float64Array(this._bucketCount);
    this._aggregateY = new Float64Array(this._bucketCount);
    this._aggregateUnitX = new Float64Array(this._bucketCount);
    this._aggregateUnitY = new Float64Array(this._bucketCount);
    this._aggregateMixed = new Uint8Array(this._bucketCount);
    this._aggregateEpoch = -1;
    this._aggregateSim = null;
    this.flock = new Float64Array(7);

    this._cellX = new Float64Array(this.capacity);
    this._cellY = new Float64Array(this.capacity);
    this._gridNext = new Int32Array(this.capacity);
    this._liveSlots = new Int32Array(this.capacity);
    this._liveStamp = new Uint32Array(this.capacity);
    this._gridStamp = new Uint32Array(this.capacity);
    this._epoch = 0;
    this._liveCount = 0;
    this._gridInvalidated = true;
    this._gridReady = false;
    this._gridSim = null;

    this._qualitySlots = new Int32Array(this.capacity);
    this._qualityDistSq = new Float64Array(this.capacity);
    this._qualityRecalls = new Float32Array(Math.max(1, this.capacity));
    this._qualityMarks = new Uint32Array(this.capacity);
    this._qualitySeen = new Uint32Array(this.capacity);
    this._qualityEpoch = 0;

    this.metrics = {
      gridBuildMs: 0,
      queryCandidates: 0,
      aggregatedNeighbors: 0,
      aggregateBuildMs: 0,
      gridQueries: 0,
      gridEntries: 0,
      maxBucketOccupancy: 0,
      medianBucketOccupancy: 0,
      occupiedBuckets: 0,
      bucketOverflow: 0,
      invalidPositions: 0,
    };
  }

  Neighbors.prototype._resetForSimulation = function (sim) {
    this._gridSim = sim;
    this._gridInvalidated = true;
    this._gridReady = false;
    this._aggregateEpoch = -1;
    this._aggregateSim = null;
    this._epoch = 0;
    this._liveStamp.fill(0);
    this._gridStamp.fill(0);
  };

  Neighbors.prototype.invalidate = function () {
    this._gridInvalidated = true;
    this._aggregateEpoch = -1;
    this._aggregateSim = null;
  };

  Neighbors.prototype._hashCell = function (cellX, cellY) {
    var x = Math.imul(cellX | 0, 0x8da6b343);
    var y = Math.imul(cellY | 0, 0xd8163841);
    return (x ^ y) & this._bucketMask;
  };

  Neighbors.prototype._buildGrid = function (sim, recordMetrics) {
    var start = recordMetrics === false ? 0 : now();
    if (this._epoch === 0xffffffff) {
      this._liveStamp.fill(0);
      this._gridStamp.fill(0);
      this._epoch = 1;
    } else {
      this._epoch++;
    }
    this._liveCount = 0;
    this._gridEntryCount = 0;
    this._bucketHeads.fill(-1);
    this._bucketLoads.fill(0);
    if (recordMetrics !== false) {
      this.metrics.invalidPositions = 0;
      this.metrics.maxBucketOccupancy = 0;
      this.metrics.medianBucketOccupancy = 0;
      this.metrics.occupiedBuckets = 0;
    }
    var active = sim.active;
    var activeCount = sim.count;
    var hasActiveList = active && Number.isFinite(activeCount);
    var limit = hasActiveList
      ? Math.max(0, Math.min(this.capacity, activeCount | 0))
      : this.capacity;
    for (var a = 0; a < limit; a++) {
      var slot = hasActiveList ? active[a] : a;
      if (slot < 0 || slot >= this.capacity || !isLive(sim, slot)) continue;
      if (this._liveStamp[slot] === this._epoch) continue;
      this._liveStamp[slot] = this._epoch;
      this._liveSlots[this._liveCount++] = slot;

      var x = sim.x[slot];
      var y = sim.y[slot];
      if (!Number.isFinite(x) || !Number.isFinite(y)) {
        if (recordMetrics !== false) this.metrics.invalidPositions++;
        continue;
      }

      var cellX = Math.floor(x / this.cellSize);
      var cellY = Math.floor(y / this.cellSize);
      this._cellX[slot] = cellX;
      this._cellY[slot] = cellY;
      this._gridStamp[slot] = this._epoch;
      var bucket = this._hashCell(cellX, cellY);
      this._gridNext[slot] = this._bucketHeads[bucket];
      this._bucketHeads[bucket] = slot;
      this._bucketLoads[bucket]++;
      this._gridEntryCount++;

    }

    if (recordMetrics !== false) {
      this._occupancyHistogram.fill(0);
      var occupied = 0;
      var maxOccupancy = 0;
      for (var b = 0; b < this._bucketCount; b++) {
        var load = this._bucketLoads[b];
        if (!load) continue;
        occupied++;
        this._occupancyHistogram[load]++;
        if (load > maxOccupancy) maxOccupancy = load;
      }
      var lowerMedian = Math.max(0, Math.floor((occupied - 1) / 2));
      var upperMedian = Math.max(0, Math.floor(occupied / 2));
      var cumulative = 0;
      var lowerValue = 0;
      var upperValue = 0;
      var foundLower = false;
      for (var occupancy = 1; occupancy <= this.capacity; occupancy++) {
        cumulative += this._occupancyHistogram[occupancy];
        if (!foundLower && cumulative > lowerMedian) {
          lowerValue = occupancy;
          foundLower = true;
        }
        if (cumulative > upperMedian) {
          upperValue = occupancy;
          break;
        }
      }
      this.metrics.occupiedBuckets = occupied;
      this.metrics.maxBucketOccupancy = maxOccupancy;
      this.metrics.medianBucketOccupancy = occupied
        ? (lowerValue + upperValue) * 0.5
        : 0;
      this.metrics.gridEntries = this._gridEntryCount;
      this.metrics.bucketOverflow = 0;
      this.metrics.gridBuildMs = now() - start;
    }
    this._gridSim = sim;
    this._gridReady = true;
    this._gridInvalidated = false;
  };

  Neighbors.prototype._isGridPositionValid = function (sim, slot) {
    return (
      slot >= 0 &&
      slot < this.capacity &&
      this._gridStamp[slot] === this._epoch &&
      isLive(sim, slot) &&
      Number.isFinite(sim.x[slot]) &&
      Number.isFinite(sim.y[slot])
    );
  };

  Neighbors.prototype._queryGrid = function (
    sim,
    source,
    radius,
    outputSlots,
    outputDistSq,
    outputDistances,
    recordMetrics,
  ) {
    if (recordMetrics !== false) this.metrics.gridQueries++;
    var count = 0;
    if (!this._isGridPositionValid(sim, source)) return 0;
    var x = sim.x[source];
    var y = sim.y[source];
    var radiusSq = radius * radius;
    var minCellX = Math.floor((x - radius) / this.cellSize);
    var maxCellX = Math.floor((x + radius) / this.cellSize);
    var minCellY = Math.floor((y - radius) / this.cellSize);
    var maxCellY = Math.floor((y + radius) / this.cellSize);
    var columns = maxCellX - minCellX + 1;
    var rows = maxCellY - minCellY + 1;
    for (var row = 0; row < rows; row++) {
      var cellY = minCellY + row;
      for (var column = 0; column < columns; column++) {
        var cellX = minCellX + column;
        var bucket = this._hashCell(cellX, cellY);
        for (
          var candidate = this._bucketHeads[bucket];
          candidate !== -1;
          candidate = this._gridNext[candidate]
        ) {
          if (
            this._cellX[candidate] !== cellX ||
            this._cellY[candidate] !== cellY ||
            candidate === source ||
            !this._isGridPositionValid(sim, candidate)
          )
            continue;
          if (recordMetrics !== false) this.metrics.queryCandidates++;
          var dx = sim.x[candidate] - x;
          var dy = sim.y[candidate] - y;
          var distanceSq = dx * dx + dy * dy;
          if (distanceSq > radiusSq) continue;
          outputSlots[count] = candidate;
          outputDistSq[count] = distanceSq;
          count++;
        }
      }
    }
    if (outputDistances) {
      for (var i = 0; i < count; i++)
        outputDistances[i] = Math.sqrt(outputDistSq[i]);
    }
    return count;
  };


  Neighbors.prototype.update = function (sim, dt) {
    if (this._gridSim !== sim) this._resetForSimulation(sim);
    this.metrics.queryCandidates = 0;
    this.metrics.aggregatedNeighbors = 0;
    this.metrics.aggregateBuildMs = 0;
    this.metrics.gridQueries = 0;
    this._buildGrid(sim, true);
  };

  Neighbors.prototype._ensureGrid = function (sim) {
    if (this._gridSim !== sim || !this._gridReady || this._gridInvalidated)
      this.update(sim, 0);
  };

  Neighbors.prototype.query = function (sim, slot) {
    this._ensureGrid(sim);
    if (slot < 0 || slot >= this.capacity || !isLive(sim, slot)) return 0;
    return this._queryGrid(
      sim,
      slot,
      this.radius,
      this.result,
      this._resultDistSq,
      this.dist,
      true,
    );
  };

  Neighbors.prototype.querySeparation = function (sim, slot) {
    this._ensureGrid(sim);
    if (slot < 0 || slot >= this.capacity || !isLive(sim, slot)) return 0;
    return this._queryGrid(
      sim,
      slot,
      this.separationRadius,
      this.result,
      this._resultDistSq,
      this.dist,
      true,
    );
  };

  // Exact full-radius force sums. Entire cells inside R use additive sums;
  // boundary cells and all short-distance separation retain exact distances.
  // Output: [count, relativeX, relativeY, directionX, directionY, separationX, separationY].
  Neighbors.prototype.aggregateRadius = function (sim, source, unitX, unitY) {
    this._ensureGrid(sim);
    var output = this.flock;
    output.fill(0);
    if (!this._isGridPositionValid(sim, source) || !isLive(sim, source))
      return output;
    if (this._aggregateEpoch !== this._epoch || this._aggregateSim !== sim) {
      var start = now();
      this._aggregateX.fill(0);
      this._aggregateY.fill(0);
      this._aggregateUnitX.fill(0);
      this._aggregateUnitY.fill(0);
      this._aggregateMixed.fill(0);
      for (var a = 0; a < this._liveCount; a++) {
        var slot = this._liveSlots[a];
        if (this._gridStamp[slot] !== this._epoch) continue;
        var hash = this._hashCell(this._cellX[slot], this._cellY[slot]);
        this._aggregateX[hash] += sim.x[slot];
        this._aggregateY[hash] += sim.y[slot];
        this._aggregateUnitX[hash] += unitX[slot];
        this._aggregateUnitY[hash] += unitY[slot];
        var head = this._bucketHeads[hash];
        if (
          this._cellX[head] !== this._cellX[slot] ||
          this._cellY[head] !== this._cellY[slot]
        )
          this._aggregateMixed[hash] = 1;
      }
      this._aggregateEpoch = this._epoch;
      this._aggregateSim = sim;
      this.metrics.aggregateBuildMs = now() - start;
    }
    this.metrics.gridQueries++;
    var x = sim.x[source];
    var y = sim.y[source];
    var size = this.cellSize;
    var positionsX = sim.x;
    var positionsY = sim.y;
    var cellX = this._cellX;
    var cellY = this._cellY;
    var minX = Math.floor((x - this.radius) / size);
    var maxX = Math.floor((x + this.radius) / size);
    var minY = Math.floor((y - this.radius) / size);
    var maxY = Math.floor((y + this.radius) / size);
    var total = 0;
    var sumX = 0;
    var sumY = 0;
    var dirX = 0;
    var dirY = 0;
    var sepX = 0;
    var sepY = 0;
    var aggregated = 0;
    var candidates = 0;
    var separate = sim.settings.separation !== 0;
    var columns = maxX - minX + 1;
    var rows = maxY - minY + 1;
    for (var row = 0; row < rows; row++) {
      var cy = minY + row;
      for (var column = 0; column < columns; column++) {
        var cx = minX + column;
        var bucket = this._hashCell(cx, cy);
        var head = this._bucketHeads[bucket];
        if (head < 0) continue;
        var mixed = this._aggregateMixed[bucket];
        if (!mixed && (cellX[head] !== cx || cellY[head] !== cy)) continue;
        var left = cx * size;
        var top = cy * size;
        var nearX = Math.max(left - x, 0, x - left - size);
        var nearY = Math.max(top - y, 0, y - top - size);
        var nearDistanceSq = nearX * nearX + nearY * nearY;
        if (nearDistanceSq > RADIUS_SQ) continue;
        var farX = Math.max(Math.abs(left - x), Math.abs(left + size - x));
        var farY = Math.max(Math.abs(top - y), Math.abs(top + size - y));
        var whole = !mixed && farX * farX + farY * farY <= RADIUS_SQ;
        if (whole) {
          var own = cellX[source] === cx && cellY[source] === cy ? 1 : 0;
          var count = this._bucketLoads[bucket] - own;
          total += count;
          sumX += this._aggregateX[bucket] - (count + own) * x;
          sumY += this._aggregateY[bucket] - (count + own) * y;
          dirX += this._aggregateUnitX[bucket] - own * unitX[source];
          dirY += this._aggregateUnitY[bucket] - own * unitY[source];
          aggregated += count;
          if (!separate || nearDistanceSq > SEPARATION_RADIUS_SQ)
            continue;
        }
        for (
          var candidate = head;
          candidate !== -1;
          candidate = this._gridNext[candidate]
        ) {
          if (
            candidate === source ||
            !isLive(sim, candidate) ||
            (mixed && (cellX[candidate] !== cx || cellY[candidate] !== cy))
          )
            continue;
          candidates++;
          var dx = positionsX[candidate] - x;
          var dy = positionsY[candidate] - y;
          var d2 = dx * dx + dy * dy;
          if (!whole) {
            if (d2 > RADIUS_SQ) continue;
            total++;
            sumX += dx;
            sumY += dy;
            dirX += unitX[candidate];
            dirY += unitY[candidate];
          }
          if (separate && d2 < SEPARATION_RADIUS_SQ && d2 > 1e-8) {
            var distance = Math.sqrt(d2);
            var factor = (1 - distance / SEPARATION_RADIUS) / distance;
            sepX -= dx * factor;
            sepY -= dy * factor;
          }
        }
      }
    }
    this.metrics.aggregatedNeighbors += aggregated;
    this.metrics.queryCandidates += candidates;
    output[0] = total;
    output[1] = sumX;
    output[2] = sumY;
    output[3] = dirX;
    output[4] = dirY;
    output[5] = sepX;
    output[6] = sepY;
    return output;
  };

  Neighbors.prototype._nextQualityEpoch = function () {
    if (this._qualityEpoch === 0xffffffff) {
      this._qualityMarks.fill(0);
      this._qualitySeen.fill(0);
      this._qualityEpoch = 1;
    } else {
      this._qualityEpoch++;
      if (this._qualityEpoch === 0) this._qualityEpoch = 1;
    }
    return this._qualityEpoch;
  };

  Neighbors.prototype.quality = function (sim, samples) {
    if (this._gridSim !== sim) this._resetForSimulation(sim);
    this._buildGrid(sim, false);
    var requested =
      samples === undefined
        ? 128
        : Math.max(0, Math.floor(Number(samples) || 0));
    var sampleCount = Math.min(
      this._liveCount,
      requested,
      this._qualityRecalls.length,
    );
    var truthSamples = 0;
    var emptyTruthSamples = 0;
    var recallSum = 0;
    var precisionSum = 0;
    var exactMatches = 0;
    var recallCount = 0;
    var invalidReferences = 0;
    var separationMisses = 0;
    var actualCount;
    var expectedCount;

    for (var sample = 0; sample < sampleCount; sample++) {
      var activeIndex = Math.floor((sample * this._liveCount) / sampleCount);
      var source = this._liveSlots[activeIndex];
      var epoch = this._nextQualityEpoch();
      expectedCount = 0;
      if (this._isGridPositionValid(sim, source)) {
        var x = sim.x[source];
        var y = sim.y[source];
        for (var candidateIndex = 0; candidateIndex < this._liveCount; candidateIndex++) {
          var candidate = this._liveSlots[candidateIndex];
          if (
            candidate === source ||
            !this._isGridPositionValid(sim, candidate)
          )
            continue;
          var dx = sim.x[candidate] - x;
          var dy = sim.y[candidate] - y;
          if (dx * dx + dy * dy > RADIUS_SQ) continue;
          this._qualityMarks[candidate] = epoch;
          expectedCount++;
        }
      }

      actualCount = this._queryGrid(
        sim,
        source,
        this.radius,
        this._qualitySlots,
        this._qualityDistSq,
        null,
        false,
      );

      var matched = 0;
      for (var actual = 0; actual < actualCount; actual++) {
        var result = this._qualitySlots[actual];
        if (
          result < 0 ||
          result >= this.capacity ||
          result === source ||
          !this._isGridPositionValid(sim, result)
        ) {
          invalidReferences++;
          continue;
        }
        if (this._qualityMarks[result] === epoch) {
          if (this._qualitySeen[result] !== epoch) {
            this._qualitySeen[result] = epoch;
            matched++;
          } else {
            invalidReferences++;
          }
        }
      }
      var recall = expectedCount ? matched / expectedCount : 1;
      var precision = actualCount ? matched / actualCount : 1;
      precisionSum += precision;
      if (expectedCount) {
        truthSamples++;
        recallSum += recall;
        this._qualityRecalls[recallCount++] = recall;
      } else {
        emptyTruthSamples++;
      }
      if (
        actualCount === expectedCount &&
        matched === expectedCount
      )
        exactMatches++;

      var separationCount = this._queryGrid(
        sim,
        source,
        this.separationRadius,
        this._qualitySlots,
        this._qualityDistSq,
        null,
        false,
      );
      for (var liveSlotIndex = 0; liveSlotIndex < this._liveCount; liveSlotIndex++) {
        var close = this._liveSlots[liveSlotIndex];
        if (
          close === source ||
          !this._isGridPositionValid(sim, source) ||
          !this._isGridPositionValid(sim, close)
        )
          continue;
        var closeX = sim.x[close] - sim.x[source];
        var closeY = sim.y[close] - sim.y[source];
        if (closeX * closeX + closeY * closeY > SEPARATION_RADIUS_SQ) continue;
        var found = false;
        for (var separationIndex = 0; separationIndex < separationCount; separationIndex++) {
          if (this._qualitySlots[separationIndex] === close) {
            found = true;
            break;
          }
        }
        if (!found) separationMisses++;
      }
    }

    for (var sort = 1; sort < recallCount; sort++) {
      var value = this._qualityRecalls[sort];
      var insert = sort;
      while (insert > 0 && this._qualityRecalls[insert - 1] > value) {
        this._qualityRecalls[insert] = this._qualityRecalls[insert - 1];
        insert--;
      }
      this._qualityRecalls[insert] = value;
    }
    return {
      mode: "radius",
      samples: sampleCount,
      samplesWithTruth: truthSamples,
      emptyTruthSamples: emptyTruthSamples,
      meanRecall: truthSamples ? recallSum / truthSamples : null,
      recallP5: recallCount
        ? this._qualityRecalls[Math.floor((recallCount - 1) * 0.05)]
        : null,
      meanPrecision: sampleCount ? precisionSum / sampleCount : null,
      exactMatchRate: sampleCount ? exactMatches / sampleCount : null,
      invalidReferences: invalidReferences,
      separationMisses: separationMisses,
    };
  };

  Boids.Neighbors = Neighbors;
  if (typeof module !== "undefined" && module.exports)
    module.exports = Neighbors;
})(globalThis);
