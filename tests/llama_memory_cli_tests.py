"""Validate the host's low-memory daemon options without loading a model."""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "runtimes"))
from llama_memory import LlamaMemoryOptions, start_daemon

daemon = sys.argv[1]
try:
    LlamaMemoryOptions(kv_cache="bad")
    raise AssertionError("Invalid Python cache format was accepted")
except ValueError:
    pass
for bad in (-2, 1048577, True, 1.5):
    try:
        LlamaMemoryOptions(gpu_layers=bad)
        raise AssertionError("Invalid Python layer limit was accepted")
    except ValueError:
        pass
child = start_daemon(daemon, LlamaMemoryOptions(kv_cache="q8_0", gpu_layers=0),
                     ["--hardware"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
stdout, stderr = child.communicate(timeout=30)
assert child.returncode == 0 and '"ram_bytes"' in stdout, stderr
help_result = subprocess.run([daemon, "--help"], capture_output=True, text=True, check=True)
assert "--llama-kv-cache" in help_result.stdout
assert "--llama-gpu-layers" in help_result.stdout
for option, value, message in [
    ("--llama-kv-cache", "q3", "llama-kv-cache must be"),
    ("--llama-gpu-layers", "-2", "llama-gpu-layers must be"),
]:
    result = subprocess.run([daemon, option, value, "--hardware"], capture_output=True, text=True)
    assert result.returncode != 0 and message in result.stderr, (result.returncode, result.stderr)
