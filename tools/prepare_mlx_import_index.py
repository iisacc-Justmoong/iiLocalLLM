#!/usr/bin/env python3
"""Build dependency metadata once; no model, tokenizer or inference is substituted."""
import argparse
import importlib.util
import json
from pathlib import Path
import os
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--if-venv", action="store_true")
parser.add_argument("--force", action="store_true")
args = parser.parse_args()
if args.if_venv and sys.prefix == sys.base_prefix:
    print("MLX import index: unmanaged interpreter; live dependency loading retained")
    raise SystemExit(0)
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "runtimes"))
import mlx_import_index as cache

spec = importlib.util.find_spec("transformers")
if spec is None:
    raise RuntimeError("The selected MLX interpreter has no Transformers installation")
root = Path(spec.origin).parent
target = cache.default_path()
if not args.force:
    try:
        index = cache.ImportIndex(root, json.loads(target.read_text(encoding="utf-8")))
        if index.rows is not None:
            print("MLX import index: installed dependency metadata unchanged")
            raise SystemExit(0)
    except (OSError, ValueError, KeyError, TypeError):
        pass

finder = cache.install(target, record=True)
os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1")
import transformers
if finder.index is None:
    raise RuntimeError("Transformers did not expose the supported import scanner")
document = finder.index.document()
target.parent.mkdir(parents=True, exist_ok=True)
temporary = None
try:
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target.parent, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(document, stream, ensure_ascii=False, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(target)
finally:
    if temporary is not None:
        temporary.unlink(missing_ok=True)
print("MLX import index prepared:", target)
