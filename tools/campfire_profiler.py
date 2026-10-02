#!/usr/bin/env python3
"""Campfire external profiler: Instruments capture without engine instrumentation.
CPU stacks, allocation stacks, Metal/system traces and hardware counters use
macOS/Xcode facilities. No engine SDK, printf, compiler hooks or source rewrite.
"""
import argparse
from datetime import datetime, timezone
import html
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import signal
import subprocess
import sys
import time

from summarize_xctrace import summarize

TEMPLATES = {
    "cpu": "Time Profiler",
    "memory": "Allocations",
    "gpu": "Metal System Trace",
    "system": "System Trace",
    "counters": "CPU Counters",
}
CLIENTS = {"campfire_client", "campfire_editor", "asr_client", "asr_editor"}


def duration(text):
    value = int(text)
    if not 1 <= value <= 300:
        raise argparse.ArgumentTypeError("duration must be 1..300 seconds")
    return value


def positive_pid(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("PID must be positive")
    return value


def recording_command(mode, seconds, trace, pid=None, launch=None, arguments=()):
    if (pid is None) == (launch is None):
        raise ValueError("choose exactly one of --pid and --launch")
    if mode not in TEMPLATES or not 1 <= seconds <= 300:
        raise ValueError("invalid capture mode/duration")
    if pid is not None and (pid <= 0 or arguments):
        raise ValueError("launch arguments cannot be used with --pid")
    command = ["xcrun", "xctrace", "record", "--template", TEMPLATES[mode],
               "--time-limit", str(seconds) + "s", "--output", str(trace), "--no-prompt"]
    if pid is not None:
        command += ["--attach", str(pid)]
    else:
        command += ["--launch", "--", str(launch), *arguments]
    return command


def run_command(command, logfile, timeout):
    """Interrupt only our profiler process on timeout; never kill an attached PID."""
    with Path(logfile).open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            process.send_signal(signal.SIGINT)  # lets xctrace finalize its own capture
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            raise


def process_list():
    output = subprocess.run(["ps", "-axo", "pid=,comm="], capture_output=True,
                            text=True, check=True, timeout=10).stdout
    result = []
    for line in output.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) == 2 and Path(fields[1]).name in CLIENTS:
            result.append({"pid": int(fields[0]), "executable": fields[1]})
    return result


def debug_executable(source, output, runner=run_command):
    folder = output / "target"
    folder.mkdir()
    copy = folder / Path(source).name
    shutil.copy2(source, copy)
    entitlements = output / "debug-entitlements.plist"
    entitlements.write_bytes(plistlib.dumps({"com.apple.security.get-task-allow": True}))
    command = ["codesign", "--force", "--sign", "-", "--entitlements", str(entitlements), str(copy)]
    if runner(command, output / "codesign.log", 30) != 0:
        raise RuntimeError("Could not debug-sign the private copy; see codesign.log")
    return copy


# Metal and System traces of an unthrottled renderer grow by gigabytes a
# second, in memory and on disk: longer than this is refused.
HEAVY_MODES = {"gpu": 20, "system": 20}
# What Instruments leaves in TMPDIR while it records, and does not always
# take away (a finished trace holds its own copy).
SCRATCH_PATTERNS = ("instruments*.ktrace", "xrgpu_aps_*")


def instruments_scratch():
    folder = Path(os.environ.get("TMPDIR", "/tmp"))
    found = set()
    for pattern in SCRATCH_PATTERNS:
        found.update(folder.glob(pattern))
    return found


def remove_scratch(before):
    for item in instruments_scratch() - before:
        if item.is_dir():
            shutil.rmtree(item, ignore_errors=True)
        else:
            try:
                item.unlink()
            except OSError:
                pass


def record(mode, seconds, output, pid=None, launch=None, arguments=(), runner=run_command, debug_copy=False):
    if mode in HEAVY_MODES and seconds > HEAVY_MODES[mode]:
        raise ValueError(f"--mode {mode} is limited to {HEAVY_MODES[mode]} s: its trace grows by gigabytes a second")
    if debug_copy and launch is None:
        raise ValueError("--debug-copy requires --launch; attached processes are never modified")
    output = Path(output).absolute()
    trace = output / "capture.trace"
    command = recording_command(mode, seconds, trace, pid, launch, arguments)
    output.mkdir(parents=True, exist_ok=False)  # never overwrite a previous capture
    metadata = {"tool": "Campfire external profiler", "backend": "xctrace",
                "mode": mode, "template": TEMPLATES[mode], "requested_seconds": seconds,
                "target": {"pid": pid, "executable": str(launch) if launch else None},
                "started_utc": datetime.now(timezone.utc).isoformat(), "status": "recording",
                "instrumented_engine": False, "debug_copy": debug_copy}
    status_file = output / "recording.json"
    status_file.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    started = time.monotonic()
    scratch = instruments_scratch()
    try:
        if debug_copy:
            executable = debug_executable(launch, output, runner)
            metadata["target"]["profiled_executable"] = str(executable)
            command = recording_command(mode, seconds, trace, launch=executable, arguments=arguments)
        # Metal and system traces take minutes to finalize after the target
        # stops; cutting them off leaves an unreadable trace.
        rc = runner(command, output / "record.log", seconds + (900 if mode in ("gpu", "system") else 90))
        metadata["profiler_exit_code"] = rc
        log_path = output / "record.log"
        completed = log_path.exists() and "Recording completed." in log_path.read_text(encoding="utf-8", errors="replace")
        if not trace.exists() or (rc != 0 and not completed):
            raise RuntimeError(f"xctrace did not produce a successful recording (exit {rc}); see {output / 'record.log'}")
        metadata["status"] = "complete" if rc == 0 else "recorded_with_warning"
        if rc != 0:
            metadata["warning"] = f"Recording saved, but xctrace returned {rc}; the launched process may have been terminated at the recording limit. Inspect target status in Instruments."
    except BaseException as error:
        metadata["status"] = "failed"
        metadata["error"] = str(error) or type(error).__name__
        raise
    finally:
        remove_scratch(scratch)
        metadata["elapsed_wall_seconds"] = round(time.monotonic() - started, 3)
        status_file.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return trace


def report_html(summary):
    def table(title, rows):
        body = "".join("<tr><td>" + html.escape(row["name"]) + "</td><td>" +
                       str(row["sampled_ms"]) + "</td><td>" + str(row["percent_of_sampled_cpu"]) +
                       "%</td></tr>" for row in rows)
        return f"<h2>{title}</h2><table><tr><th>Symbol / thread</th><th>Sampled ms</th><th>Share</th></tr>{body}</table>"
    return ("<!doctype html><meta charset='utf-8'><title>Campfire CPU profile</title>"
            "<style>body{font:15px system-ui;max-width:1200px;margin:32px auto;padding:0 20px;background:#171a20;color:#e8edf2}"
            "table{border-collapse:collapse;width:100%}td,th{padding:8px;text-align:left;border-bottom:1px solid #39404b}"
            "td:first-child{overflow-wrap:anywhere}h1,h2{color:#ffbd75}</style><h1>Campfire — external CPU profile</h1>"
            f"<p>{summary['samples']} samples; {summary['sampled_cpu_ms']} ms summed across threads.</p>"
            "<p>Statistical CPU samples, not exact function duration, GPU time or hardware cycles. "
            "Inclusive rows overlap and must not be added. Open capture.trace in Instruments for the full timeline/flame graph.</p>"
            + spikes_html(summary.get("spikes", []))
            + table("Self time", summary["self"]) + callers_html(summary.get("callers", []))
            + table("Inclusive time", summary["inclusive"])
            + table("Threads", summary["threads"]))


def spikes_html(spikes):
    rows = "".join("<tr><td>" + str(s["at_seconds"]) + " s</td><td>" + str(s["ms"]) + " ms</td><td>" +
                   html.escape(s["function"]) + "<br><small>" +
                   " &rarr; ".join(html.escape(n[:90]) for n in s["stack"]) + "</small></td></tr>"
                   for s in spikes)
    return ("<h2>Main-thread spikes</h2><p>Unbroken stretches of the main thread inside one function, "
            "longest first; the deepest function covering the stretch.</p>"
            f"<table><tr><th>When</th><th>Length</th><th>Function / stack</th></tr>{rows}</table>")


def callers_html(callers):
    rows = "".join("<tr><td>" + html.escape(c["name"][:120]) + "</td><td>" + str(c["sampled_ms"]) + "</td><td>" +
                   "<br>".join(str(ch["sampled_ms"]) + " ms: " + " &larr; ".join(html.escape(n[:80]) for n in ch["via"])
                               for ch in c["chains"]) + "</td></tr>" for c in callers)
    return f"<h2>Who calls the hottest</h2><table><tr><th>Function</th><th>Self ms</th><th>Called via</th></tr>{rows}</table>"


def cpu_report(capture, runner=run_command, spike_ms=25.0):
    capture = Path(capture).resolve()
    trace = capture / "capture.trace" if capture.is_dir() and capture.suffix != ".trace" else capture
    if not trace.exists():
        raise ValueError("capture.trace does not exist")
    folder = trace.parent
    metadata_file = folder / "recording.json"
    if metadata_file.exists():
        metadata = json.loads(metadata_file.read_text(encoding="utf-8"))
        if metadata.get("status") not in ("complete", "recorded_with_warning") or metadata.get("mode") != "cpu":
            raise ValueError("CPU summary requires a completed CPU capture; other modes are viewed in Instruments")
    xml = folder / "time-profile.xml"
    command = ["xcrun", "xctrace", "export", "--input", str(trace), "--xpath",
               '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]', "--output", str(xml)]
    if runner(command, folder / "export.log", 90) != 0:
        raise RuntimeError("xctrace export failed; see export.log")
    summary = summarize(xml, spike_ms)
    if not summary["samples"]:
        raise RuntimeError("No CPU samples in capture; an empty recording is not a successful profile")
    (folder / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    report = folder / "report.html"
    report.write_text(report_html(summary), encoding="utf-8")
    return report


def parser():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("processes", help="list Campfire and legacy ASR client/editor processes")
    sub.add_parser("templates", help="list installed Instruments templates")
    capture = sub.add_parser("record", help="record a process without modifying its source")
    target = capture.add_mutually_exclusive_group(required=True)
    target.add_argument("--pid", type=positive_pid)
    target.add_argument("--launch", type=Path, metavar="EXECUTABLE")
    capture.add_argument("--mode", choices=TEMPLATES, default="cpu")
    capture.add_argument("--seconds", type=duration, default=10)
    capture.add_argument("--output", type=Path, help="new capture directory")
    capture.add_argument("--open", action="store_true", help="open the resulting trace in Instruments")
    capture.add_argument("--debug-copy", action="store_true",
                         help="debug-sign a private launch copy for Allocations; original executable is unchanged")
    capture.add_argument("arguments", nargs=argparse.REMAINDER, help="arguments after -- for the launched program")
    summary = sub.add_parser("report", help="export a CPU capture to local HTML and JSON")
    summary.add_argument("capture", type=Path)
    summary.add_argument("--spike-ms", type=float, default=25.0,
                         help="shortest unbroken main-thread stretch reported as a spike")
    return ap


def main(argv=None):
    ap = parser()
    args = ap.parse_args(argv)
    if platform.system() != "Darwin":
        ap.error("this external backend requires macOS and Xcode Instruments")
    try:
        if args.action == "processes":
            print(json.dumps(process_list(), indent=2))
        elif args.action == "templates":
            subprocess.run(["xcrun", "xctrace", "list", "templates"], check=True, timeout=30)
        elif args.action == "report":
            print(cpu_report(args.capture, spike_ms=args.spike_ms))
        else:
            arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
            if args.pid:
                os.kill(args.pid, 0)  # existence/access only, never changes the target
            launch = args.launch.resolve() if args.launch else None
            if launch and (not launch.is_file() or not os.access(launch, os.X_OK)):
                raise ValueError("--launch must name an existing executable")
            output = args.output or Path(".cache/profiles") / datetime.now().strftime("campfire-%Y%m%d-%H%M%S-%f")
            trace = record(args.mode, args.seconds, output, args.pid, launch, arguments, debug_copy=args.debug_copy)
            print(trace)
            metadata = json.loads((trace.parent / "recording.json").read_text(encoding="utf-8"))
            if metadata.get("warning"):
                print("Warning: " + metadata["warning"], file=sys.stderr)
            if args.mode == "cpu":
                print(cpu_report(trace))
            if args.open:
                subprocess.run(["open", str(trace)], check=True, timeout=15)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Campfire profiler: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
