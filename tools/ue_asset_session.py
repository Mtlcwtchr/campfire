"""Bounded, read-only UE import session: keep startup across export corrections.
Run as a Python commandlet with -nullrhi -unattended -NoShaderCompile.
Only accepts inventory/export/quit JSON jobs under AS_UE_SESSION (repo .cache by default).
Never executes arbitrary job code and never saves UE packages. Exits after 15 minutes idle.
"""
import importlib
import json
import os
from pathlib import Path
import sys
import time
import traceback

import unreal

ROOT = Path(__file__).resolve().parents[1]
SESSION = Path(os.environ.get("AS_UE_SESSION", str(ROOT / ".cache/ue_asset_session")))
sys.path.insert(0, str(ROOT / "tools"))


def write(path, data):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2))
    os.replace(temporary, path)


def run():
    SESSION.mkdir(parents=True, exist_ok=True)
    write(SESSION / "ready.json", {"pid": os.getpid(), "ready": True})
    unreal.log_warning("AS_SESSION ready pid=" + str(os.getpid()))
    done = set()
    idle_since = time.monotonic()
    while time.monotonic() - idle_since < 900:
        jobs = sorted(SESSION.glob("job-*.json"))
        for path in jobs:
            if path.name in done:
                continue
            try:
                job = json.loads(path.read_text())
            except (OSError, json.JSONDecodeError):
                continue  # producer has not atomically published a full job yet
            if not isinstance(job, dict):
                continue
            action = job.get("action")
            result = {"job": path.name, "action": action, "ok": False}
            try:
                if action == "quit":
                    write(SESSION / ("result-" + path.name), dict(result, ok=True))
                    unreal.log_warning("AS_SESSION closed")
                    return
                if action not in ("inventory", "export"):
                    raise ValueError("Unsupported session action")
                for name in ("ue_asset_policy", "ue_asset_inventory", "export_ue_assets"):
                    if name in sys.modules:
                        importlib.reload(sys.modules[name])
                    else:
                        importlib.import_module(name)
                if action == "inventory":
                    sys.modules["ue_asset_inventory"].inventory()
                else:
                    sys.modules["export_ue_assets"].main()
                result["ok"] = True
            except Exception:
                result["error"] = traceback.format_exc()
                unreal.log_error("AS_SESSION " + result["error"])
            write(SESSION / ("result-" + path.name), result)
            done.add(path.name)
            idle_since = time.monotonic()
            unreal.log_warning("AS_SESSION finished " + path.name + " ok=" + str(result["ok"]))
            unreal.SystemLibrary.collect_garbage()
        time.sleep(0.25)
    unreal.log_warning("AS_SESSION idle timeout")


if __name__ == "__main__":
    run()

