"""Analyze actual native-host CSV runs; never labels submissions as display frames."""
import argparse
import csv
import json
import math
import statistics
import re
from pathlib import Path

BUDGET_MS = 1000.0 / 60.0


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def analyze(metadata_path):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
    csv_path = metadata_path.parent / "run.csv"
    text = csv_path.read_text(encoding="utf-8-sig")
    rows = list(csv.DictReader(line for line in text.splitlines() if line and not line.startswith("#")))
    start = float(metadata["effective_warmup_seconds"])
    duration = int(metadata["sample_seconds"])
    sample = [row for row in rows if start <= float(row["elapsed_seconds"]) < start + duration]
    before = [row for row in rows if float(row["elapsed_seconds"]) < start]
    invalid = list(metadata.get("invalid_reasons", []))
    if metadata.get("status") != "valid":
        invalid.append("Runner did not mark this run valid")
    if metadata.get("process_exit_code") != 0:
        invalid.append("Successful application exit was not captured")
    timed_exits = [float(value) for value in re.findall(
        r"^# event elapsed_seconds=([0-9]+(?:\.[0-9]+)?) seconds_deadline_exit\s*$",
        text, flags=re.MULTILINE)]
    if not any(value >= start + duration for value in timed_exits):
        invalid.append("Application did not reach the complete sample endpoint before timed exit")
    if len(sample) < 2:
        invalid.append("Fewer than two sample frames")
    dimensions = {(row["physical_width"], row["physical_height"], row["logical_width"], row["logical_height"]) for row in sample}
    if len(dimensions) != 1:
        invalid.append("Window dimensions changed or are unavailable")
    positive = [row for row in sample if float(row["submit_interval_ms"]) > 0]
    if len(positive) != len(sample):
        invalid.append("Sample contains a first/resume frame without a preceding interval")
    intervals = [float(row["submit_interval_ms"]) for row in positive]
    if any(not math.isfinite(value) for value in intervals):
        raise ValueError(f"Nonfinite interval in {csv_path}")
    frame_work = [float(row["frame_cpu_ms"]) for row in sample]
    update_work = [float(row["frame_cpu_ms"]) for row in sample if int(row["simulation_steps_since_previous"]) > 0]
    sim_work = [float(row["simulation_ms"]) for row in sample]
    draw_work = [float(row["draw_call_ms"]) for row in sample]
    second_counts = [0] * duration
    for row in sample:
        bucket = int(float(row["elapsed_seconds"]) - start)
        if 0 <= bucket < duration:
            second_counts[bucket] += 1
    within = sum(value <= BUDGET_MS for value in intervals) / len(intervals) if intervals else None
    seconds_at_60 = sum(count >= 60 for count in second_counts) / duration
    p90 = percentile(intervals, .90)
    prior = before[-1] if before else (sample[0] if sample else None)
    def counter_delta(field):
        return int(sample[-1][field]) - int(prior[field]) if sample and prior else None
    headers = {}
    for line in text.splitlines():
        if line.startswith("# ") and "=" in line:
            key, value = line[2:].split("=", 1)
            headers[key] = value
    capacity = int(headers["max_capacity"].split()[0])
    valid = not invalid
    return {
        "run": metadata_path.parent.name,
        "metadata": str(metadata_path),
        "raw_csv": str(csv_path),
        "status": "valid" if valid else "invalid",
        "invalid_reasons": invalid,
        "count": int(metadata["requested_count_birds"]),
        "compiled_capacity": capacity,
        "fps_cap": int(metadata["requested_application_fps_cap"]),
        "monitor_index": int(metadata["settings"]["monitor_index"]),
        "process_exit_code": metadata.get("process_exit_code"),
        "seed": int(metadata["fixed_simulation_seed"]),
        "warmup_seconds": start,
        "sample_seconds": duration,
        "frames": len(sample),
        "dimensions": list(next(iter(dimensions))) if len(dimensions) == 1 else None,
        "adapter": headers.get("adapter"),
        "submission_fps": 1000.0 / statistics.mean(intervals) if intervals else None,
        "submit_p50_ms": percentile(intervals, .50),
        "submit_p90_ms": p90,
        "submit_p99_ms": percentile(intervals, .99),
        "submit_max_ms": max(intervals) if intervals else None,
        "interval_budget_fraction": within,
        "p90_budget_pass": bool(valid and p90 is not None and p90 <= BUDGET_MS and within >= .90),
        "complete_second_counts": second_counts,
        "seconds_at_60_fraction": seconds_at_60,
        "seconds_90_at_60_pass": bool(valid and seconds_at_60 >= .90),
        "frame_processing_p90_ms": percentile(frame_work, .90),
        "physics_updating_frame_processing_p90_ms": percentile(update_work, .90),
        "physics_updating_frame_budget_fraction": sum(value <= BUDGET_MS for value in update_work) / len(update_work) if update_work else None,
        "frame_simulation_p90_ms": percentile(sim_work, .90),
        "draw_call_p90_ms": percentile(draw_work, .90),
        "simulation_hz": sum(int(row["simulation_steps_since_previous"]) for row in sample) / duration,
        "dropped_simulation_steps": counter_delta("dropped_simulation_steps"),
        "dropped_frame_deadlines": counter_delta("dropped_frame_deadlines"),
        "min_total_birds": min(int(row["total_birds"]) for row in sample) if sample else None,
        "min_active_birds": min(int(row["birds_active"]) for row in sample) if sample else None,
        "max_entering_birds": max(int(row["entering"]) for row in sample) if sample else None,
        "peak_working_set_mib": metadata.get("process_peak_working_set_mib"),
        "actual_display_measured": False,
        "metric_scope": "Application submission intervals; 120-cap runs can include frames without a new 60-Hz physics update. Draw-call time includes CPU upload/submission/Present, not measured GPU execution.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="Directory containing completed command-metadata.json files")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    paths = sorted(args.root.rglob("command-metadata.json"))
    if not paths:
        parser.error("No completed run metadata found")
    results = [analyze(path) for path in paths]
    summary = {"metric": "application-submission-P90", "frame_budget_ms": BUDGET_MS, "percentile_method": "nearest rank ceil(0.90*N)", "slow_frames_removed": False, "actual_display_measured": False, "runs": results}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    columns = ["count", "compiled_capacity", "monitor_index", "fps_cap", "status", "frames", "submission_fps", "submit_p90_ms", "submit_p99_ms", "interval_budget_fraction", "p90_budget_pass", "seconds_at_60_fraction", "physics_updating_frame_processing_p90_ms", "frame_simulation_p90_ms", "draw_call_p90_ms", "simulation_hz", "dropped_simulation_steps", "min_total_birds", "process_exit_code", "run"]
    csv_output = args.output.with_suffix(".csv")
    with csv_output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)
    for result in results:
        print(json.dumps({key: result[key] for key in columns}, ensure_ascii=False))
    print(f"Wrote {args.output} and {csv_output}; no real display FPS is claimed")


if __name__ == "__main__":
    main()
