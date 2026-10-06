"""Explicit host opt-out for tests of contracts predating economic admission."""
import atexit
import functools
import json
import os
from pathlib import Path
import tempfile

@functools.lru_cache(maxsize=1)
def _profile():
    directory = Path(__file__).resolve().parents[1] / "build"
    descriptor, path = tempfile.mkstemp(prefix="legacy-decision-", suffix=".json", dir=directory)
    with os.fdopen(descriptor, "w") as stream:
        json.dump({"enabled": False}, stream)
    atexit.register(lambda: Path(path).unlink(missing_ok=True))
    return path

def legacy_decision_command(command):
    values = list(command)
    option = "--agent-decision" if "--agent-workspace" in values else "--decision" if "--model" in values else None
    if option and option not in values:
        values += [option, _profile()]
    return values
