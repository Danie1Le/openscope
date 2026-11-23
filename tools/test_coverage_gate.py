"""Tests for the pure grouping and threshold logic of coverage_gate.py (run from tools/)."""

import unittest

import coverage_gate

AREAS = {"trigger": ["t.cpp"], "measurement": ["m.cpp", "n.cpp"]}


def counts(covered, count):
    return {"covered": covered, "count": count}


class AreaTotalsTest(unittest.TestCase):
    def test_sums_the_files_of_each_area(self):
        lines = {"t.cpp": counts(1, 2), "m.cpp": counts(5, 10), "n.cpp": counts(3, 4)}
        self.assertEqual(
            coverage_gate.area_totals(lines, AREAS), {"trigger": (1, 2), "measurement": (8, 14)}
        )


class FailingAreasTest(unittest.TestCase):
    def test_a_small_area_fails_although_the_sum_is_high(self):
        totals = {"trigger": (0, 23), "measurement": (1000, 1000)}
        self.assertEqual(coverage_gate.failing_areas(totals), ["trigger"])

    def test_the_threshold_itself_passes(self):
        self.assertEqual(coverage_gate.failing_areas({"a": (85, 100)}), [])
        self.assertEqual(coverage_gate.failing_areas({"a": (84, 100)}), ["a"])

    def test_every_failing_area_is_named(self):
        totals = {"a": (0, 10), "b": (10, 10), "c": (1, 10)}
        self.assertEqual(coverage_gate.failing_areas(totals), ["a", "c"])

    def test_an_area_without_lines_fails(self):
        self.assertEqual(coverage_gate.failing_areas({"a": (0, 0)}), ["a"])


class SummaryTest(unittest.TestCase):
    def test_marks_the_failed_areas_and_only_them(self):
        lines = {path: counts(1, 2) for path in coverage_gate.GATED_FILES}
        totals = coverage_gate.area_totals(lines)
        trigger_lines = 2 * len(coverage_gate.AREAS["trigger"])
        measurement_lines = 2 * len(coverage_gate.AREAS["measurement"])

        text = coverage_gate.summary(lines, totals, ["measurement"]).splitlines()

        self.assertIn(f"  trigger: 50.00% ({trigger_lines // 2} of {trigger_lines} lines) ok", text)
        self.assertIn(
            f"  measurement: 50.00% ({measurement_lines // 2} of {measurement_lines} lines) BELOW",
            text,
        )


class ConfigurationTest(unittest.TestCase):
    def test_every_gated_file_is_in_exactly_one_area(self):
        self.assertEqual(len(coverage_gate.GATED_FILES), len(set(coverage_gate.GATED_FILES)))

    def test_areas_are_the_three_named_ones(self):
        self.assertEqual(list(coverage_gate.AREAS), ["trigger", "measurement", "SCPI parser"])


if __name__ == "__main__":
    unittest.main()
