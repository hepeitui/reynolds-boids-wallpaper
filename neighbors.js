(function (root) {
  "use strict";

  var Boids = (root.Boids = root.Boids || {});
  var MAX_K = 48;
  var CANDIDATE_LIMIT = 96;
  var CANDIDATE_PRIMARY_LIMIT = 88;
  var EXPLORATION_QUOTA = 8;
  var TWO_HOP_SAMPLE = 4;
  var CELL_SIZE = 40;
  var RADIUS = 58;
  var SEPARATION_RADIUS = 15;
  var RADIUS_SQ = RADIUS * RADIUS;
  var SEPARATION_RADIUS_SQ = SEPARATION_RADIUS * SEPARATION_RADIUS;
  var CALIBRATION_PERIOD = 0.25;

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
    this._knownGeneration = new Uint32Array(this.capacity);
    this._knownState = new Uint8Array(this.capacity);
    this._knownLive = new Uint8Array(this.capacity);
    this._epoch = 0;
    this._liveCount = 0;
    this._gridInvalidated = true;
    this._gridReady = false;
    this._gridSim = null;

    this._candidateSlots = new Int32Array(CANDIDATE_LIMIT);
    this._candidateDistSq = new Float64Array(CANDIDATE_LIMIT);
    this._candidateMarks = new Uint32Array(this.capacity);
    this._candidateEpoch = 0;
    this._candidateMark = 0;
    this._candidateCount = 0;
    this._gridProbeCount = 0;
    this._randomState = new Uint32Array(this.capacity);
    this._rotation = new Uint32Array(this.capacity);
    this._twoHopCursor = new Uint32Array(this.capacity);

    this._queryMarks = new Uint32Array(this.capacity);
    this._queryEpoch = 0;
    this._qualitySlots = new Int32Array(MAX_K);
    this._qualityDistSq = new Float64Array(MAX_K);
    this._qualityRecalls = new Float32Array(Math.max(1, this.capacity));
    this._componentParent = new Int32Array(this.capacity);
    this._componentRank = new Uint8Array(this.capacity);

    this._preserveSlots = new Int32Array(MAX_K);
    this._preserveGenerations = new Uint32Array(MAX_K);
    this._preserveTimes = new Float64Array(MAX_K);
    this._graphStride = this.capacity * MAX_K;
    this._graphs = [
      this._newGraph(this._graphStride),
      this._newGraph(this._graphStride),
    ];
    this._graphCurrent = 0;
    this._graphReady = false;
    this._graphK = 0;
    this._graphTimer = 0;
    this._graphTime = 0;
    this._lastGraphMode = false;
    this._calibrationCredit = 0;
    this._calibrationCursor = 0;
    this._calibrationEpoch = 1;
    this._calibrationMarks = new Uint32Array(this.capacity);
    this._calibrationCount = 0;
    this._lastCalibrationTime = -1;
    this._invalidated = true;

    this.metrics = {
      gridBuildMs: 0,
      graphRefreshMs: 0,
      graphRefreshes: 0,
      queryCandidates: 0,
      aggregatedNeighbors: 0,
      aggregateBuildMs: 0,
      candidateScans: 0,
      candidateUnique: 0,
      candidateBudgetHits: 0,
      explorationCandidatesSkipped: 0,
      gridProbeScanLimited: 0,
      gridQueries: 0,
      gridEntries: 0,
      maxBucketOccupancy: 0,
      medianBucketOccupancy: 0,
      occupiedBuckets: 0,
      bucketOverflow: 0,
      invalidPositions: 0,
      invalidReferences: 0,
      calibrationCoverage: 0,
      calibrationQueries: 0,
      separationMisses: 0,
      farthestError: 0,
      componentCount: 0,
    };
  }

  Neighbors.prototype._newGraph = function (stride) {
    var graph = {
      slots: new Int32Array(stride),
      generations: new Uint32Array(stride),
      times: new Float64Array(stride),
      counts: new Uint8Array(this.capacity),
      sourceGenerations: new Uint32Array(this.capacity),
    };
    graph.slots.fill(-1);
    return graph;
  };

  Neighbors.prototype._resetForSimulation = function (sim) {
    this._gridSim = sim;
    this._gridInvalidated = true;
    this._gridReady = false;
    this._graphReady = false;
    this._graphCurrent = 0;
    this._graphK = 0;
    this._graphTimer = 0;
    this._graphTime = 0;
    this._lastGraphMode = false;
    this._calibrationCredit = 0;
    this._calibrationCursor = 0;
    this._calibrationEpoch = 1;
    this._calibrationCount = 0;
    this._lastCalibrationTime = -1;
    this._invalidated = true;
    this._epoch = 0;
    this._liveStamp.fill(0);
    this._gridStamp.fill(0);
    this._knownGeneration.fill(0);
    this._knownState.fill(0);
    this._knownLive.fill(0);
    this._candidateMarks.fill(0);
    this._queryMarks.fill(0);
    this._calibrationMarks.fill(0);
    this._randomState.fill(0);
    this._rotation.fill(0);
    this._twoHopCursor.fill(0);
    for (var i = 0; i < 2; i++) {
      this._graphs[i].counts.fill(0);
      this._graphs[i].sourceGenerations.fill(0);
      this._graphs[i].slots.fill(-1);
    }
  };

  Neighbors.prototype.invalidate = function () {
    this._invalidated = true;
    this._gridInvalidated = true;
    this._aggregateEpoch = -1;
    this._aggregateSim = null;
  };

  Neighbors.prototype._hashCell = function (cellX, cellY) {
    var x = Math.imul(cellX | 0, 0x8da6b343);
    var y = Math.imul(cellY | 0, 0xd8163841);
    return (x ^ y) & this._bucketMask;
  };

  Neighbors.prototype._buildGrid = function (sim) {
    var start = now();
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
    this.metrics.invalidPositions = 0;
    this.metrics.maxBucketOccupancy = 0;
    this.metrics.medianBucketOccupancy = 0;
    this.metrics.occupiedBuckets = 0;

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
        this.metrics.invalidPositions++;
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
    this._gridSim = sim;
    this._gridReady = true;
    this._gridInvalidated = false;
    this.metrics.gridBuildMs = now() - start;
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

  Neighbors.prototype._less = function (sim, slotA, distA, slotB, distB) {
    if (distA < distB) return true;
    if (distA > distB) return false;
    var idA = sim.id[slotA];
    var idB = sim.id[slotB];
    if (idA < idB) return true;
    if (idA > idB) return false;
    return slotA < slotB;
  };

  Neighbors.prototype._queryGrid = function (
    sim,
    source,
    radius,
    k,
    allInRadius,
  ) {
    this.metrics.gridQueries++;
    var count = 0;
    if (!this._isGridPositionValid(sim, source)) return 0;
    var x = sim.x[source];
    var y = sim.y[source];
    var radiusSq = radius * radius;
    var minCellX = Math.floor((x - radius) / this.cellSize);
    var maxCellX = Math.floor((x + radius) / this.cellSize);
    var minCellY = Math.floor((y - radius) / this.cellSize);
    var maxCellY = Math.floor((y + radius) / this.cellSize);
    for (var cellY = minCellY; cellY <= maxCellY; cellY++) {
      for (var cellX = minCellX; cellX <= maxCellX; cellX++) {
        var bucket = this._hashCell(cellX, cellY);
        for (
          var candidate = this._bucketHeads[bucket];
          candidate !== -1;
          candidate = this._gridNext[candidate]
        ) {
          if (
            this._cellX[candidate] !== cellX ||
            this._cellY[candidate] !== cellY
          )
            continue;
          if (candidate === source || !isLive(sim, candidate)) continue;
          this.metrics.queryCandidates++;
          var dx = sim.x[candidate] - x;
          var dy = sim.y[candidate] - y;
          var distanceSq = dx * dx + dy * dy;
          if (distanceSq > radiusSq) continue;
          if (allInRadius) {
            this.result[count] = candidate;
            this._resultDistSq[count] = distanceSq;
            count++;
          } else {
            var position = count;
            if (count < k) {
              count++;
            } else {
              var last = count - 1;
              if (
                !this._less(
                  sim,
                  candidate,
                  distanceSq,
                  this.result[last],
                  this._resultDistSq[last],
                )
              )
                continue;
              position = last;
            }
            while (
              position > 0 &&
              this._less(
                sim,
                candidate,
                distanceSq,
                this.result[position - 1],
                this._resultDistSq[position - 1],
              )
            ) {
              if (position < k) {
                this.result[position] = this.result[position - 1];
                this._resultDistSq[position] = this._resultDistSq[position - 1];
              }
              position--;
            }
            if (position < k) {
              this.result[position] = candidate;
              this._resultDistSq[position] = distanceSq;
            }
          }
        }
      }
    }
    for (var i = 0; i < count; i++)
      this.dist[i] = Math.sqrt(this._resultDistSq[i]);
    return count;
  };

  Neighbors.prototype._readK = function (sim) {
    var requested =
      sim.settings && Number.isFinite(sim.settings.k)
        ? Math.floor(sim.settings.k)
        : 16;
    return Math.max(1, Math.min(MAX_K, requested));
  };

  Neighbors.prototype._readGraphHz = function (sim) {
    var requested =
      sim.settings && Number.isFinite(sim.settings.graphHz)
        ? sim.settings.graphHz
        : 15;
    return Math.max(10, Math.min(30, requested));
  };

  Neighbors.prototype._writeRow = function (
    sim,
    graph,
    source,
    k,
    preserveGraph,
  ) {
    var base = source * MAX_K;
    var count = Math.min(k, this._queryScratchCount);
    var canPreserve =
      preserveGraph &&
      preserveGraph.sourceGenerations[source] === sim.generation[source] >>> 0;
    var oldCount = canPreserve ? preserveGraph.counts[source] : 0;
    var copyOldRow = canPreserve && graph === preserveGraph;
    if (copyOldRow) {
      var oldBase = source * MAX_K;
      for (var copy = 0; copy < oldCount; copy++) {
        var copyOffset = oldBase + copy;
        this._preserveSlots[copy] = preserveGraph.slots[copyOffset];
        this._preserveGenerations[copy] = preserveGraph.generations[copyOffset];
        this._preserveTimes[copy] = preserveGraph.times[copyOffset];
      }
    }
    graph.sourceGenerations[source] = sim.generation[source] >>> 0;
    graph.counts[source] = count;
    for (var i = 0; i < count; i++) {
      var slot = this.result[i];
      var generation = sim.generation[slot] >>> 0;
      var edgeTime = this._graphTime;
      if (canPreserve) {
        var oldBase = source * MAX_K;
        for (var oldIndex = 0; oldIndex < oldCount; oldIndex++) {
          var oldSlot = copyOldRow
            ? this._preserveSlots[oldIndex]
            : preserveGraph.slots[oldBase + oldIndex];
          var oldGeneration = copyOldRow
            ? this._preserveGenerations[oldIndex]
            : preserveGraph.generations[oldBase + oldIndex];
          if (oldSlot === slot && oldGeneration === generation) {
            edgeTime = copyOldRow
              ? this._preserveTimes[oldIndex]
              : preserveGraph.times[oldBase + oldIndex];
            break;
          }
        }
      }
      graph.slots[base + i] = slot;
      graph.generations[base + i] = generation;
      graph.times[base + i] = edgeTime;
    }
  };

  Neighbors.prototype._writeExactRowBoth = function (sim, source, k) {
    this._queryScratchCount = this._queryGrid(
      sim,
      source,
      this.radius,
      k,
      false,
    );
    var current = this._graphs[this._graphCurrent];
    var other = this._graphs[1 - this._graphCurrent];
    this._writeRow(sim, current, source, k, current);
    this._writeRow(sim, other, source, k, current);
  };

  Neighbors.prototype._nextCandidateEpoch = function () {
    if (this._candidateEpoch === 0xffffffff) {
      this._candidateMarks.fill(0);
      this._candidateEpoch = 1;
    } else {
      this._candidateEpoch++;
      if (this._candidateEpoch === 0) this._candidateEpoch = 1;
    }
    this._candidateMark = this._candidateEpoch;
    return this._candidateMark;
  };

  Neighbors.prototype._tryCandidate = function (sim, source, slot, limit) {
    this.metrics.candidateScans++;
    if (
      slot < 0 ||
      slot >= this.capacity ||
      slot === source ||
      !isLive(sim, slot) ||
      !this._isGridPositionValid(sim, slot)
    )
      return false;
    if (this._candidateMarks[slot] === this._candidateMark) return false;
    var dx = sim.x[slot] - sim.x[source];
    var dy = sim.y[slot] - sim.y[source];
    var distanceSq = dx * dx + dy * dy;
    if (distanceSq > RADIUS_SQ) return false;
    if (this._candidateCount >= limit) {
      this.metrics.candidateBudgetHits++;
      return false;
    }
    var index = this._candidateCount++;
    this._candidateSlots[index] = slot;
    this._candidateDistSq[index] = distanceSq;
    this._candidateMarks[slot] = this._candidateMark;
    this.metrics.candidateUnique++;
    return true;
  };

  Neighbors.prototype._validGraphReference = function (
    sim,
    graph,
    source,
    entry,
  ) {
    var offset = source * MAX_K + entry;
    var slot = graph.slots[offset];
    return (
      slot >= 0 &&
      slot < this.capacity &&
      slot !== source &&
      isLive(sim, slot) &&
      graph.generations[offset] === sim.generation[slot] >>> 0 &&
      this._isGridPositionValid(sim, slot)
    );
  };

  Neighbors.prototype._addGridProbe = function (sim, source, slot) {
    this.metrics.candidateScans++;
    if (slot === source || !isLive(sim, slot)) return;
    var dx = sim.x[slot] - sim.x[source];
    var dy = sim.y[slot] - sim.y[source];
    var distanceSq = dx * dx + dy * dy;
    if (
      distanceSq > RADIUS_SQ ||
      this._candidateMarks[slot] === this._candidateMark
    )
      return;

    var first = CANDIDATE_PRIMARY_LIMIT;
    var count = this._gridProbeCount;
    var position = count;
    while (
      position > 0 &&
      this._less(
        sim,
        slot,
        distanceSq,
        this._candidateSlots[first + position - 1],
        this._candidateDistSq[first + position - 1],
      )
    )
      position--;
    if (count === EXPLORATION_QUOTA && position === count) {
      this.metrics.explorationCandidatesSkipped++;
      return;
    }
    if (count < EXPLORATION_QUOTA) this._gridProbeCount++;
    else {
      var removed = this._candidateSlots[first + EXPLORATION_QUOTA - 1];
      this._candidateMarks[removed] = 0;
      this.metrics.explorationCandidatesSkipped++;
    }
    var stop = Math.min(count, EXPLORATION_QUOTA - 1);
    for (var i = stop; i > position; i--) {
      this._candidateSlots[first + i] = this._candidateSlots[first + i - 1];
      this._candidateDistSq[first + i] = this._candidateDistSq[first + i - 1];
    }
    this._candidateSlots[first + position] = slot;
    this._candidateDistSq[first + position] = distanceSq;
    this._candidateMarks[slot] = this._candidateMark;
    this.metrics.candidateUnique++;
  };

  Neighbors.prototype._probeGrid = function (sim, source, scanLimit) {
    var x = sim.x[source];
    var y = sim.y[source];
    var minCellX = Math.floor((x - this.radius) / this.cellSize);
    var maxCellX = Math.floor((x + this.radius) / this.cellSize);
    var minCellY = Math.floor((y - this.radius) / this.cellSize);
    var maxCellY = Math.floor((y + this.radius) / this.cellSize);
    var scanned = 0;
    for (var cellY = minCellY; cellY <= maxCellY; cellY++) {
      for (var cellX = minCellX; cellX <= maxCellX; cellX++) {
        var bucket = this._hashCell(cellX, cellY);
        for (
          var slot = this._bucketHeads[bucket];
          slot !== -1;
          slot = this._gridNext[slot]
        ) {
          if (scanned >= scanLimit) {
            this.metrics.gridProbeScanLimited++;
            return;
          }
          scanned++;
          if (this._cellX[slot] === cellX && this._cellY[slot] === cellY)
            this._addGridProbe(sim, source, slot);
        }
      }
    }
  };

  Neighbors.prototype._randomProbe = function (sim, source) {
    var liveCount = this._liveCount;
    if (liveCount < 2) return;
    var state = this._randomState[source];
    if (state === 0) {
      state =
        (Math.imul(source + 1, 0x9e3779b1) ^
          Math.imul(sim.generation[source] + 1, 0x85ebca6b)) >>>
        0;
      if (state === 0) state = 1;
    }
    var remaining = EXPLORATION_QUOTA - this._gridProbeCount;
    var attempts = remaining * 3;
    while (remaining > 0 && attempts-- > 0) {
      state ^= state << 13;
      state ^= state >>> 17;
      state ^= state << 5;
      state >>>= 0;
      var slot = this._liveSlots[state % liveCount];
      this.metrics.candidateScans++;
      if (
        slot === source ||
        this._candidateMarks[slot] === this._candidateMark ||
        !this._isGridPositionValid(sim, slot)
      )
        continue;
      var dx = sim.x[slot] - sim.x[source];
      var dy = sim.y[slot] - sim.y[source];
      var distanceSq = dx * dx + dy * dy;
      if (distanceSq > RADIUS_SQ) continue;
      var index = CANDIDATE_PRIMARY_LIMIT + this._gridProbeCount++;
      this._candidateSlots[index] = slot;
      this._candidateDistSq[index] = distanceSq;
      this._candidateMarks[slot] = this._candidateMark;
      this.metrics.candidateUnique++;
      remaining--;
    }
    this._randomState[source] = state;
  };

  Neighbors.prototype._collectGraphCandidates = function (
    sim,
    source,
    oldGraph,
  ) {
    this._candidateCount = 0;
    this._gridProbeCount = 0;
    this._nextCandidateEpoch();
    var oldCount =
      oldGraph.sourceGenerations[source] === sim.generation[source] >>> 0
        ? oldGraph.counts[source]
        : 0;
    var rotation = oldCount ? this._rotation[source] % oldCount : 0;
    for (
      var i = 0;
      i < oldCount && this._candidateCount < CANDIDATE_PRIMARY_LIMIT;
      i++
    ) {
      var entry = (i + rotation) % oldCount;
      if (!this._validGraphReference(sim, oldGraph, source, entry)) {
        this.metrics.invalidReferences++;
        continue;
      }
      this._tryCandidate(
        sim,
        source,
        oldGraph.slots[source * MAX_K + entry],
        CANDIDATE_PRIMARY_LIMIT,
      );
    }

    var sampleOffset = this._twoHopCursor[source];
    for (
      var oneHop = 0;
      oneHop < oldCount && this._candidateCount < CANDIDATE_PRIMARY_LIMIT;
      oneHop++
    ) {
      var oneEntry = (oneHop + rotation) % oldCount;
      if (!this._validGraphReference(sim, oldGraph, source, oneEntry)) continue;
      var middle = oldGraph.slots[source * MAX_K + oneEntry];
      var middleCount =
        oldGraph.sourceGenerations[middle] === sim.generation[middle] >>> 0
          ? oldGraph.counts[middle]
          : 0;
      if (!middleCount) continue;
      var take = Math.min(TWO_HOP_SAMPLE, middleCount);
      var start = sampleOffset % middleCount;
      for (
        var sample = 0;
        sample < take && this._candidateCount < CANDIDATE_PRIMARY_LIMIT;
        sample++
      ) {
        var secondEntry = (start + sample) % middleCount;
        if (!this._validGraphReference(sim, oldGraph, middle, secondEntry)) {
          this.metrics.invalidReferences++;
          continue;
        }
        this._tryCandidate(
          sim,
          source,
          oldGraph.slots[middle * MAX_K + secondEntry],
          CANDIDATE_PRIMARY_LIMIT,
        );
      }
      sampleOffset++;
    }
    if (this._candidateCount >= CANDIDATE_PRIMARY_LIMIT)
      this.metrics.candidateBudgetHits++;
    this._rotation[source] = oldCount ? (rotation + 1) % oldCount : 0;
    this._twoHopCursor[source] = (this._twoHopCursor[source] + 1) >>> 0;

    this._probeGrid(sim, source, CANDIDATE_LIMIT);
    if (this._gridProbeCount < EXPLORATION_QUOTA)
      this._randomProbe(sim, source);

    var candidateTotal = this._candidateCount + this._gridProbeCount;
    var k = this._workingK;
    var resultCount = 0;
    for (
      var candidateIndex = 0;
      candidateIndex < candidateTotal;
      candidateIndex++
    ) {
      var poolIndex =
        candidateIndex < this._candidateCount
          ? candidateIndex
          : CANDIDATE_PRIMARY_LIMIT + candidateIndex - this._candidateCount;
      var slot = this._candidateSlots[poolIndex];
      var distanceSq = this._candidateDistSq[poolIndex];
      var position = resultCount;
      if (resultCount < k) {
        resultCount++;
      } else {
        var last = resultCount - 1;
        if (
          !this._less(
            sim,
            slot,
            distanceSq,
            this.result[last],
            this._resultDistSq[last],
          )
        )
          continue;
        position = last;
      }
      while (
        position > 0 &&
        this._less(
          sim,
          slot,
          distanceSq,
          this.result[position - 1],
          this._resultDistSq[position - 1],
        )
      ) {
        if (position < k) {
          this.result[position] = this.result[position - 1];
          this._resultDistSq[position] = this._resultDistSq[position - 1];
        }
        position--;
      }
      if (position < k) {
        this.result[position] = slot;
        this._resultDistSq[position] = distanceSq;
      }
    }
    this._queryScratchCount = resultCount;
  };

  Neighbors.prototype._findRoot = function (parent, slot) {
    var root = slot;
    while (parent[root] !== root) root = parent[root];
    while (parent[slot] !== slot) {
      var next = parent[slot];
      parent[slot] = root;
      slot = next;
    }
    return root;
  };

  Neighbors.prototype._detectLifecycleChanges = function (sim, k) {
    for (var slot = 0; slot < this.capacity; slot++) {
      if (this._liveStamp[slot] !== this._epoch) {
        if (this._knownLive[slot]) {
          this._graphs[0].counts[slot] = 0;
          this._graphs[1].counts[slot] = 0;
        }
        this._knownLive[slot] = 0;
        this._knownState[slot] = sim.state[slot];
        this._calibrationMarks[slot] = 0;
        continue;
      }
      var generation = sim.generation[slot] >>> 0;
      var state = sim.state[slot];
      var changed =
        !this._knownLive[slot] ||
        this._knownGeneration[slot] !== generation ||
        (state === 1 && this._knownState[slot] !== 1);
      if (changed) {
        this._calibrationMarks[slot] = 0;
        if (this._graphReady && this._isGridPositionValid(sim, slot))
          this._writeExactRowBoth(sim, slot, k);
      }
      this._knownLive[slot] = 1;
      this._knownGeneration[slot] = generation;
      this._knownState[slot] = state;
    }
  };

  Neighbors.prototype._refreshGraph = function (sim, k, exact) {
    var start = now();
    var oldGraph = this._graphs[this._graphCurrent];
    var nextIndex = 1 - this._graphCurrent;
    var nextGraph = this._graphs[nextIndex];
    nextGraph.counts.fill(0);
    var wasReady = this._graphReady;
    this._workingK = k;

    for (var i = 0; i < this._liveCount; i++) {
      var source = this._liveSlots[i];
      if (!this._isGridPositionValid(sim, source)) {
        nextGraph.sourceGenerations[source] = sim.generation[source] >>> 0;
        nextGraph.counts[source] = 0;
        continue;
      }
      if (exact || !wasReady) {
        this._queryScratchCount = this._queryGrid(
          sim,
          source,
          this.radius,
          k,
          false,
        );
      } else {
        this._collectGraphCandidates(sim, source, oldGraph);
      }
      this._writeRow(sim, nextGraph, source, k, oldGraph);
    }

    this._graphCurrent = nextIndex;
    this._graphReady = true;
    this._graphK = k;
    this._invalidated = false;
    this.metrics.graphRefreshMs = now() - start;
    this.metrics.graphRefreshes++;
  };

  Neighbors.prototype._calibrate = function (sim, dt, k) {
    var count = this._liveCount;
    if (!count) {
      this._calibrationCredit = 0;
      this._calibrationCursor = 0;
      this.metrics.calibrationCoverage = 1;
      return;
    }
    this._calibrationCredit += (Math.max(0, dt) * count) / CALIBRATION_PERIOD;
    var due = Math.floor(this._calibrationCredit);
    if (due > count) {
      due = count;
      this._calibrationCredit = 0;
    } else {
      this._calibrationCredit -= due;
    }
    if (due < 1) {
      this.metrics.calibrationCoverage =
        this._lastCalibrationTime >= 0 &&
        this._graphTime - this._lastCalibrationTime <= CALIBRATION_PERIOD
          ? 1
          : Math.min(1, this._calibrationCount / count);
      return;
    }

    for (var i = 0; i < due; i++) {
      if (this._calibrationCursor >= count) this._calibrationCursor = 0;
      var source = this._liveSlots[this._calibrationCursor++];
      if (!this._isGridPositionValid(sim, source)) continue;
      this._writeExactRowBoth(sim, source, k);
      this.metrics.calibrationQueries++;
      if (this._calibrationMarks[source] !== this._calibrationEpoch) {
        this._calibrationMarks[source] = this._calibrationEpoch;
        this._calibrationCount++;
      }
    }
    if (this._calibrationCount >= count) {
      this._lastCalibrationTime = this._graphTime;
      this._calibrationEpoch++;
      if (this._calibrationEpoch === 0) {
        this._calibrationMarks.fill(0);
        this._calibrationEpoch = 1;
      }
      this._calibrationCount = 0;
    }
    this.metrics.calibrationCoverage =
      this._lastCalibrationTime >= 0 &&
      this._graphTime - this._lastCalibrationTime <= CALIBRATION_PERIOD
        ? 1
        : Math.min(1, this._calibrationCount / count);
  };

  Neighbors.prototype.update = function (sim, dt) {
    if (this._gridSim !== sim) this._resetForSimulation(sim);
    var elapsed = Number.isFinite(dt) && dt > 0 ? dt : 0;
    this._graphTime += elapsed;
    this.metrics.queryCandidates = 0;
    this.metrics.aggregatedNeighbors = 0;
    this.metrics.aggregateBuildMs = 0;
    this.metrics.candidateScans = 0;
    this.metrics.candidateUnique = 0;
    this.metrics.candidateBudgetHits = 0;
    this.metrics.explorationCandidatesSkipped = 0;
    this.metrics.gridProbeScanLimited = 0;
    this.metrics.gridQueries = 0;
    this.metrics.invalidReferences = 0;
    this.metrics.calibrationQueries = 0;
    this.metrics.separationMisses = 0;

    this._buildGrid(sim);
    var k = this._readK(sim);
    this._detectLifecycleChanges(sim, k);
    var graphMode = sim.settings && sim.settings.mode === "graph";
    if (graphMode) {
      var force =
        !this._graphReady ||
        this._graphK !== k ||
        !this._lastGraphMode ||
        this._invalidated;
      if (force) {
        this._refreshGraph(sim, k, true);
        this._graphTimer = 0;
      } else {
        this._graphTimer += elapsed;
        var interval = 1 / this._readGraphHz(sim);
        if (this._graphTimer >= interval) {
          this._graphTimer %= interval;
          this._refreshGraph(sim, k, false);
        }
      }
      this._lastGraphMode = true;
      this._calibrate(sim, elapsed, k);
    } else {
      this._lastGraphMode = false;
      this._graphTimer = 0;
      this.metrics.calibrationCoverage = this._liveCount === 0 ? 1 : 0;
    }
  };

  Neighbors.prototype._ensureGrid = function (sim) {
    if (this._gridSim !== sim || !this._gridReady || this._gridInvalidated)
      this.update(sim, 0);
  };

  Neighbors.prototype._queryGraph = function (sim, source, k) {
    var graph = this._graphs[this._graphCurrent];
    if (!this._isGridPositionValid(sim, source)) return 0;
    if (graph.sourceGenerations[source] !== sim.generation[source] >>> 0) {
      this.metrics.invalidReferences++;
      return 0;
    }
    if (this._queryEpoch === 0xffffffff) {
      this._queryMarks.fill(0);
      this._queryEpoch = 1;
    } else {
      this._queryEpoch++;
      if (this._queryEpoch === 0) this._queryEpoch = 1;
    }
    var mark = this._queryEpoch;
    var count = 0;
    var graphCount = graph.counts[source];
    var base = source * MAX_K;
    var x = sim.x[source];
    var y = sim.y[source];
    for (var i = 0; i < graphCount; i++) {
      var offset = base + i;
      var slot = graph.slots[offset];
      this.metrics.queryCandidates++;
      if (
        slot < 0 ||
        slot >= this.capacity ||
        slot === source ||
        !isLive(sim, slot) ||
        graph.generations[offset] !== sim.generation[slot] >>> 0 ||
        !this._isGridPositionValid(sim, slot)
      ) {
        this.metrics.invalidReferences++;
        continue;
      }
      if (this._queryMarks[slot] === mark) {
        this.metrics.invalidReferences++;
        continue;
      }
      this._queryMarks[slot] = mark;
      var dx = sim.x[slot] - x;
      var dy = sim.y[slot] - y;
      var distanceSq = dx * dx + dy * dy;
      if (distanceSq > RADIUS_SQ) continue;
      var position = count;
      if (count < k) {
        count++;
      } else {
        var last = count - 1;
        if (
          !this._less(
            sim,
            slot,
            distanceSq,
            this.result[last],
            this._resultDistSq[last],
          )
        )
          continue;
        position = last;
      }
      while (
        position > 0 &&
        this._less(
          sim,
          slot,
          distanceSq,
          this.result[position - 1],
          this._resultDistSq[position - 1],
        )
      ) {
        if (position < k) {
          this.result[position] = this.result[position - 1];
          this._resultDistSq[position] = this._resultDistSq[position - 1];
        }
        position--;
      }
      if (position < k) {
        this.result[position] = slot;
        this._resultDistSq[position] = distanceSq;
      }
    }
    for (var j = 0; j < count; j++)
      this.dist[j] = Math.sqrt(this._resultDistSq[j]);
    return count;
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
    var x = sim.x[source],
      y = sim.y[source],
      size = this.cellSize;
    var positionsX = sim.x,
      positionsY = sim.y,
      cellX = this._cellX,
      cellY = this._cellY;
    var minX = Math.floor((x - this.radius) / size),
      maxX = Math.floor((x + this.radius) / size);
    var minY = Math.floor((y - this.radius) / size),
      maxY = Math.floor((y + this.radius) / size);
    var total = 0,
      sumX = 0,
      sumY = 0,
      dirX = 0,
      dirY = 0,
      sepX = 0,
      sepY = 0;
    var scanned = 0,
      aggregated = 0,
      separate = sim.settings.separation !== 0;
    for (var cy = minY; cy <= maxY; cy++) {
      for (var cx = minX; cx <= maxX; cx++) {
        var bucket = this._hashCell(cx, cy),
          head = this._bucketHeads[bucket];
        if (head < 0) continue;
        var mixed = this._aggregateMixed[bucket];
        if (!mixed && (cellX[head] !== cx || cellY[head] !== cy)) continue;
        var left = cx * size,
          top = cy * size;
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
          var nearX = Math.max(left - x, 0, x - left - size);
          var nearY = Math.max(top - y, 0, y - top - size);
          if (!separate || nearX * nearX + nearY * nearY > SEPARATION_RADIUS_SQ)
            continue;
        }
        for (
          var candidate = head;
          candidate !== -1;
          candidate = this._gridNext[candidate]
        ) {
          // The simulation invalidates this snapshot immediately on death.
          if (
            candidate === source ||
            !isLive(sim, candidate) ||
            (mixed && (cellX[candidate] !== cx || cellY[candidate] !== cy))
          )
            continue;
          var dx = positionsX[candidate] - x,
            dy = positionsY[candidate] - y,
            d2 = dx * dx + dy * dy;
          if (!whole) {
            if (d2 > RADIUS_SQ) continue;
            total++;
            sumX += dx;
            sumY += dy;
            dirX += unitX[candidate];
            dirY += unitY[candidate];
          }
          if (separate && d2 < SEPARATION_RADIUS_SQ && d2 > 1e-8) {
            var distance = Math.sqrt(d2),
              factor = (1 - distance / SEPARATION_RADIUS) / distance;
            sepX -= dx * factor;
            sepY -= dy * factor;
          }
        }
      }
    }
    this.metrics.queryCandidates += scanned;
    this.metrics.aggregatedNeighbors += aggregated;
    output[0] = total;
    output[1] = sumX;
    output[2] = sumY;
    output[3] = dirX;
    output[4] = dirY;
    output[5] = sepX;
    output[6] = sepY;
    return output;
  };

  Neighbors.prototype.query = function (sim, slot, mode) {
    this._ensureGrid(sim);
    if (slot < 0 || slot >= this.capacity || !isLive(sim, slot)) return 0;
    var selectedMode = mode || (sim.settings && sim.settings.mode) || "radius";
    if (selectedMode === "graph") {
      var k = this._readK(sim);
      if (!this._graphReady || this._graphK !== k || this._invalidated)
        this._refreshGraph(sim, k, true);
      return this._queryGraph(sim, slot, k);
    }
    if (selectedMode === "grid")
      return this._queryGrid(sim, slot, this.radius, this._readK(sim), false);
    return this._queryGrid(sim, slot, this.radius, this.capacity, true);
  };

  Neighbors.prototype.querySeparation = function (sim, slot) {
    this._ensureGrid(sim);
    if (slot < 0 || slot >= this.capacity || !isLive(sim, slot)) return 0;
    return this._queryGrid(
      sim,
      slot,
      this.separationRadius,
      this.capacity,
      true,
    );
  };

  Neighbors.prototype._truth = function (sim, source, k) {
    var count = 0;
    if (
      !isLive(sim, source) ||
      !Number.isFinite(sim.x[source]) ||
      !Number.isFinite(sim.y[source])
    )
      return 0;
    var x = sim.x[source];
    var y = sim.y[source];
    for (var i = 0; i < this._liveCount; i++) {
      var slot = this._liveSlots[i];
      if (
        slot === source ||
        !isLive(sim, slot) ||
        !Number.isFinite(sim.x[slot]) ||
        !Number.isFinite(sim.y[slot])
      )
        continue;
      var dx = sim.x[slot] - x;
      var dy = sim.y[slot] - y;
      var distanceSq = dx * dx + dy * dy;
      if (distanceSq > RADIUS_SQ) continue;
      var position = count;
      if (count < k) {
        count++;
      } else {
        var last = count - 1;
        if (
          !this._less(
            sim,
            slot,
            distanceSq,
            this._qualitySlots[last],
            this._qualityDistSq[last],
          )
        )
          continue;
        position = last;
      }
      while (
        position > 0 &&
        this._less(
          sim,
          slot,
          distanceSq,
          this._qualitySlots[position - 1],
          this._qualityDistSq[position - 1],
        )
      ) {
        if (position < k) {
          this._qualitySlots[position] = this._qualitySlots[position - 1];
          this._qualityDistSq[position] = this._qualityDistSq[position - 1];
        }
        position--;
      }
      if (position < k) {
        this._qualitySlots[position] = slot;
        this._qualityDistSq[position] = distanceSq;
      }
    }
    return count;
  };

  Neighbors.prototype._countInvalidReferences = function (sim) {
    var invalid = 0;
    var graph = this._graphs[this._graphCurrent];
    for (var i = 0; i < this._liveCount; i++) {
      var source = this._liveSlots[i];
      if (graph.sourceGenerations[source] !== sim.generation[source] >>> 0) {
        invalid += graph.counts[source];
        continue;
      }
      var base = source * MAX_K;
      var count = graph.counts[source];
      for (var j = 0; j < count; j++) {
        var offset = base + j;
        var slot = graph.slots[offset];
        if (
          slot < 0 ||
          slot >= this.capacity ||
          slot === source ||
          !isLive(sim, slot) ||
          graph.generations[offset] !== sim.generation[slot] >>> 0
        ) {
          invalid++;
          continue;
        }
        for (var previous = 0; previous < j; previous++) {
          if (
            graph.slots[base + previous] === slot &&
            graph.generations[base + previous] === graph.generations[offset]
          ) {
            invalid++;
            break;
          }
        }
      }
    }
    return invalid;
  };

  Neighbors.prototype._componentCount = function (sim) {
    var graph = this._graphs[this._graphCurrent];
    for (var i = 0; i < this._liveCount; i++) {
      var slot = this._liveSlots[i];
      this._componentParent[slot] = slot;
      this._componentRank[slot] = 0;
    }
    for (var n = 0; n < this._liveCount; n++) {
      var source = this._liveSlots[n];
      if (graph.sourceGenerations[source] !== sim.generation[source] >>> 0)
        continue;
      var base = source * MAX_K;
      for (var j = 0; j < graph.counts[source]; j++) {
        var offset = base + j;
        var neighbor = graph.slots[offset];
        if (
          neighbor < 0 ||
          neighbor >= this.capacity ||
          !isLive(sim, neighbor) ||
          graph.generations[offset] !== sim.generation[neighbor] >>> 0
        )
          continue;
        var rootA = this._findRoot(this._componentParent, source);
        var rootB = this._findRoot(this._componentParent, neighbor);
        if (rootA === rootB) continue;
        if (this._componentRank[rootA] < this._componentRank[rootB]) {
          this._componentParent[rootA] = rootB;
        } else if (this._componentRank[rootA] > this._componentRank[rootB]) {
          this._componentParent[rootB] = rootA;
        } else {
          this._componentParent[rootB] = rootA;
          this._componentRank[rootA]++;
        }
      }
    }
    var components = 0;
    for (var j = 0; j < this._liveCount; j++) {
      var slot = this._liveSlots[j];
      if (this._findRoot(this._componentParent, slot) === slot) components++;
    }
    return components;
  };

  Neighbors.prototype._coverage = function () {
    if (!this._liveCount) return 1;
    if (
      this._lastCalibrationTime >= 0 &&
      this._graphTime - this._lastCalibrationTime <= CALIBRATION_PERIOD
    )
      return 1;
    return Math.min(1, this._calibrationCount / this._liveCount);
  };

  Neighbors.prototype.quality = function (sim, samples) {
    // Integration swaps position buffers after update(); diagnostics sample the new frozen snapshot.
    this._buildGrid(sim);
    var k = this._readK(sim);
    if (!this._graphReady || this._graphK !== k || this._invalidated)
      this._refreshGraph(sim, k, true);
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
    var farthestError = 0;
    var ageTotal = 0;
    var ageCount = 0;
    var maxAge = 0;
    var separationMisses = 0;
    var graph = this._graphs[this._graphCurrent];
    var recallCount = 0;

    for (var sample = 0; sample < sampleCount; sample++) {
      var activeIndex = Math.floor((sample * this._liveCount) / sampleCount);
      var source = this._liveSlots[activeIndex];
      var truthCount = this._truth(sim, source, k);
      var graphCount = this._queryGraph(sim, source, k);
      var matched = 0;
      for (var t = 0; t < truthCount; t++) {
        for (var g = 0; g < graphCount; g++) {
          if (this._qualitySlots[t] === this.result[g]) {
            matched++;
            break;
          }
        }
      }
      if (truthCount) {
        var recall = matched / truthCount;
        this._qualityRecalls[recallCount++] = recall;
        recallSum += recall;
        truthSamples++;
        var truthFarthest = Math.sqrt(this._qualityDistSq[truthCount - 1]);
        var graphFarthest = graphCount
          ? Math.sqrt(this._resultDistSq[graphCount - 1])
          : 0;
        var error = Math.abs(graphFarthest - truthFarthest);
        if (error > farthestError) farthestError = error;
      } else {
        emptyTruthSamples++;
      }

      var base = source * MAX_K;
      var rowCount = graph.counts[source];
      for (var edge = 0; edge < rowCount; edge++) {
        var offset = base + edge;
        var neighbor = graph.slots[offset];
        if (
          neighbor < 0 ||
          neighbor >= this.capacity ||
          !isLive(sim, neighbor) ||
          graph.generations[offset] !== sim.generation[neighbor] >>> 0
        )
          continue;
        var edgeDx = sim.x[neighbor] - sim.x[source];
        var edgeDy = sim.y[neighbor] - sim.y[source];
        if (edgeDx * edgeDx + edgeDy * edgeDy > RADIUS_SQ) continue;
        var edgeAge = Math.max(0, this._graphTime - graph.times[offset]);
        ageTotal += edgeAge;
        ageCount++;
        if (edgeAge > maxAge) maxAge = edgeAge;
      }

      var closeCount = this.querySeparation(sim, source);
      for (var n = 0; n < this._liveCount; n++) {
        var candidate = this._liveSlots[n];
        if (
          candidate === source ||
          !Number.isFinite(sim.x[source]) ||
          !Number.isFinite(sim.y[source]) ||
          !Number.isFinite(sim.x[candidate]) ||
          !Number.isFinite(sim.y[candidate])
        )
          continue;
        var dx = sim.x[candidate] - sim.x[source];
        var dy = sim.y[candidate] - sim.y[source];
        if (dx * dx + dy * dy > SEPARATION_RADIUS_SQ) continue;
        var found = false;
        for (var resultIndex = 0; resultIndex < closeCount; resultIndex++) {
          if (this.result[resultIndex] === candidate) {
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
    var recallP5 = recallCount
      ? this._qualityRecalls[Math.floor((recallCount - 1) * 0.05)]
      : null;
    var invalidReferences = this._countInvalidReferences(sim);
    var componentCount = this._componentCount(sim);
    var coverage = this._coverage();
    var meanRecall = truthSamples ? recallSum / truthSamples : null;
    var meanAge = ageCount ? ageTotal / ageCount : 0;

    this.metrics.invalidReferences = invalidReferences;
    this.metrics.calibrationCoverage = coverage;
    this.metrics.separationMisses = separationMisses;
    this.metrics.farthestError = farthestError;
    this.metrics.componentCount = componentCount;
    return {
      samples: sampleCount,
      samplesWithTruth: truthSamples,
      emptyTruthSamples: emptyTruthSamples,
      recallK: meanRecall,
      recallP5: recallP5,
      farthestError: farthestError,
      meanNeighborAge: meanAge,
      maxNeighborAge: maxAge,
      invalidReferences: invalidReferences,
      coverage: coverage,
      separationMisses: separationMisses,
      componentCount: componentCount,
      candidateScans: this.metrics.candidateScans,
      candidateUnique: this.metrics.candidateUnique,
      candidateBudgetHits: this.metrics.candidateBudgetHits,
    };
  };

  Boids.Neighbors = Neighbors;
  if (typeof module !== "undefined" && module.exports)
    module.exports = Neighbors;
})(globalThis);
