"""Synthetic validator fixtures only; these are not measured performance results."""
import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("capacity_analysis", Path(__file__).with_name("analyze-capacity.py"))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class CapacityAnalysisTests(unittest.TestCase):
    def create_run(self, folder, cutoff=11.0, completed=True, exit_code=0):
        metadata = {
            "status": "valid", "invalid_reasons": [], "process_exit_code": exit_code,
            "effective_warmup_seconds": 0, "sample_seconds": 10,
            "requested_count_birds": 1000, "requested_application_fps_cap": 120,
            "fixed_simulation_seed": 20261022, "settings": {"monitor_index": 1},
        }
        metadata_path = folder / "command-metadata.json"
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        fields = [
            "elapsed_seconds", "physical_width", "physical_height", "logical_width", "logical_height",
            "simulation_steps_since_previous", "simulation_ms", "submit_interval_ms", "frame_cpu_ms",
            "draw_call_ms", "total_simulation_steps", "birds_active", "dropped_simulation_steps",
            "dropped_frame_deadlines", "total_birds", "entering",
        ]
        with (folder / "run.csv").open("w", newline="", encoding="utf-8") as stream:
            stream.write("# max_capacity=16384 birds\n# adapter=synthetic-validator-fixture\n")
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            previous_steps = 0
            for index in range(1, 688):
                elapsed = index * .016
                if elapsed >= cutoff:
                    break
                steps = int(elapsed * 60)
                writer.writerow({
                    "elapsed_seconds": elapsed,
                    "physical_width": 1920, "physical_height": 1080,
                    "logical_width": 1536, "logical_height": 864,
                    "simulation_steps_since_previous": steps - previous_steps,
                    "simulation_ms": 1 if steps != previous_steps else 0,
                    "submit_interval_ms": 16, "frame_cpu_ms": 2, "draw_call_ms": 1,
                    "total_simulation_steps": steps, "birds_active": 1000,
                    "dropped_simulation_steps": 0, "dropped_frame_deadlines": 0,
                    "total_birds": 1000, "entering": 0,
                })
                previous_steps = steps
            if completed:
                stream.write("# event elapsed_seconds=11.000000 seconds_deadline_exit\n")
        return metadata_path

    def test_completed_window_qualifies(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = analysis.analyze(self.create_run(Path(temporary)))
        self.assertEqual(result["status"], "valid")
        self.assertEqual(result["submit_p90_ms"], 16)
        self.assertEqual(result["interval_budget_fraction"], 1)
        self.assertTrue(result["p90_budget_pass"])
        self.assertTrue(result["seconds_90_at_60_pass"])

    def test_early_exit_cannot_pass_using_healthy_partial_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = analysis.analyze(self.create_run(Path(temporary), cutoff=9.2, completed=False))
        self.assertEqual(result["submit_p90_ms"], 16)
        self.assertGreaterEqual(result["seconds_at_60_fraction"], .9)
        self.assertEqual(result["status"], "invalid")
        self.assertFalse(result["p90_budget_pass"])
        self.assertFalse(result["seconds_90_at_60_pass"])

    def test_uncaptured_exit_is_not_a_successful_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = analysis.analyze(self.create_run(Path(temporary), exit_code=None))
        self.assertEqual(result["status"], "invalid")
        self.assertFalse(result["p90_budget_pass"])

    def test_nearest_rank_distinguishes_exactly_ninety_percent_from_less(self):
        self.assertEqual(analysis.percentile([16] * 9 + [50], .9), 16)
        self.assertEqual(analysis.percentile([16] * 9 + [50, 50], .9), 50)
        self.assertEqual(analysis.percentile([16] * 9 + [50], .99), 50)


if __name__ == "__main__":
    unittest.main()
