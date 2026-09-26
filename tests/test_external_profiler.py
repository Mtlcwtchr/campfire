"""External profiler tests: no engine, Xcode, target process or GPU required."""
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import campfire_profiler as profiler
from summarize_xctrace import summarize

XML = '''<trace-query-result><node><row>
<thread id="t" fmt="Main"/><weight id="w">1000000</weight>
<tagged-backtrace id="stack"><backtrace id="bt">
<frame id="leaf" name="leaf&lt;T&gt;"/><frame id="parent" name="parent"/><frame ref="parent"/>
</backtrace></tagged-backtrace></row><row><thread ref="t"/><weight ref="w"/>
<tagged-backtrace ref="stack"/></row><row><weight>0</weight></row></node></trace-query-result>'''


class ExternalProfilerTests(unittest.TestCase):
    def test_attach_is_external_and_bounded(self):
        command = profiler.recording_command("cpu", 10, Path("trace file.trace"), pid=42)
        self.assertIn("--attach", command)
        self.assertNotIn("--launch", command)
        self.assertIn("--no-prompt", command)
        self.assertEqual(command[command.index("--time-limit") + 1], "10s")
        self.assertEqual(command[command.index("--output") + 1], "trace file.trace")

    def test_launch_keeps_argv_without_shell_interpolation(self):
        command = profiler.recording_command("memory", 5, Path("capture.trace"),
                                             launch=Path("/tmp/engine with spaces"), arguments=["--seed", "x; touch nothing"])
        self.assertEqual(command[-5:], ["--launch", "--", "/tmp/engine with spaces", "--seed", "x; touch nothing"])
        for mode in profiler.TEMPLATES:
            self.assertIn(profiler.TEMPLATES[mode], profiler.recording_command(mode, 1, Path("x"), pid=1))

    def test_invalid_targets_and_durations_rejected(self):
        for kwargs in ({}, {"pid": 0}, {"pid": 1, "launch": Path("client")}, {"pid": 1, "arguments": ["arg"]}):
            with self.assertRaises(ValueError): profiler.recording_command("cpu", 5, Path("x"), **kwargs)
        for seconds in (0, 301):
            with self.assertRaises(ValueError): profiler.recording_command("cpu", seconds, Path("x"), pid=1)

    def test_record_success_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            def fake(command, logfile, timeout):
                self.assertEqual(timeout, 100)
                Path(command[command.index("--output") + 1]).mkdir()
                return 0
            trace = profiler.record("cpu", 10, output, pid=42, runner=fake)
            self.assertTrue(trace.exists())
            metadata = json.loads((output / "recording.json").read_text())
            self.assertEqual(metadata["status"], "complete")
            self.assertFalse(metadata["instrumented_engine"])
            with self.assertRaises(FileExistsError): profiler.record("cpu", 10, output, pid=42, runner=fake)

    def test_failed_capture_is_not_reported_as_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            with self.assertRaises(RuntimeError):
                profiler.record("gpu", 1, output, pid=42, runner=lambda *args: 1)
            self.assertEqual(json.loads((output / "recording.json").read_text())["status"], "failed")
            with self.assertRaises(ValueError): profiler.cpu_report(output)

    def test_completed_trace_with_target_exit_is_kept_with_warning(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture"
            def fake(command, logfile, timeout):
                Path(command[command.index("--output") + 1]).mkdir()
                Path(logfile).write_text("Recording completed. Saving output file...\n")
                return 54
            trace = profiler.record("cpu", 3, output, launch=Path("client"), runner=fake)
            self.assertTrue(trace.exists())
            data = json.loads((output / "recording.json").read_text())
            self.assertEqual(data["status"], "recorded_with_warning")
            self.assertEqual(data["profiler_exit_code"], 54)
            self.assertIn("terminated", data["warning"])

    def test_timeout_interrupts_only_profiler(self):
        class Child:
            signals = []
            waits = 0
            def wait(self, timeout):
                self.waits += 1
                if self.waits == 1: raise subprocess.TimeoutExpired("xctrace", timeout)
                return 0
            def send_signal(self, value): self.signals.append(value)
        child = Child()
        with tempfile.TemporaryDirectory() as temporary, patch.object(profiler.subprocess, "Popen", return_value=child), patch.object(profiler.os, "kill") as kill:
            with self.assertRaises(subprocess.TimeoutExpired):
                profiler.run_command(["xctrace", "--attach", "42"], Path(temporary) / "log", 1)
            self.assertEqual(child.signals, [signal.SIGINT])
            kill.assert_not_called()

    def test_debug_copy_never_modifies_original_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = root / "campfire_client"
            original.write_bytes(b"original executable")
            output = root / "capture"
            def fake(command, logfile, timeout):
                if command[0] == "codesign":
                    copied = Path(command[-1])
                    self.assertNotEqual(copied, original)
                    copied.write_bytes(b"signed copy")
                else:
                    self.assertIn(str(output / "target/campfire_client"), command)
                    Path(command[command.index("--output") + 1]).mkdir()
                return 0
            profiler.record("memory", 3, output, launch=original, runner=fake, debug_copy=True)
            self.assertEqual(original.read_bytes(), b"original executable")
            self.assertEqual((output / "target/campfire_client").read_bytes(), b"signed copy")
            with self.assertRaises(ValueError):
                profiler.record("memory", 3, root / "invalid", pid=42, debug_copy=True)

    def test_cpu_summary_resolves_references_and_recursive_frames_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            xml = Path(temporary) / "data.xml"
            xml.write_text(XML)
            data = summarize(xml)
            self.assertEqual(data["samples"], 2)
            self.assertEqual(data["sampled_cpu_ms"], 2.0)
            self.assertEqual(data["self"][0]["name"], "leaf<T>")
            self.assertEqual(next(r for r in data["inclusive"] if r["name"] == "parent")["sampled_ms"], 2.0)
            self.assertIn("leaf&lt;T&gt;", profiler.report_html(data))
            self.assertNotIn("leaf<T>", profiler.report_html(data))

    def test_report_exports_local_html_and_rejects_empty_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace = root / "capture.trace"; trace.mkdir()
            def fake(command, logfile, timeout):
                Path(command[command.index("--output") + 1]).write_text(XML)
                return 0
            report = profiler.cpu_report(trace, runner=fake)
            self.assertTrue(report.exists())
            self.assertTrue((root / "summary.json").exists())
            def empty(command, logfile, timeout):
                Path(command[command.index("--output") + 1]).write_text("<trace-query-result/>")
                return 0
            with self.assertRaises(RuntimeError): profiler.cpu_report(trace, runner=empty)


if __name__ == "__main__":
    unittest.main()
