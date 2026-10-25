"""Read-only, versioned Transformers import metadata for the private MLX worker.

The build tool produces this index from Transformers' own scanner. A miss or
invalid index uses that scanner unchanged; model loading/inference are untouched.
"""
import functools
import hashlib
import importlib.machinery
import json
from pathlib import Path
import sys

FORMAT = 1


def default_path():
    return Path(sys.prefix) / "share" / "iiLocalLLM" / "transformers-import-index.json"


def fingerprint(root):
    root = Path(root)
    records = list(root.parent.glob("transformers-*.dist-info/RECORD"))
    if len(records) != 1:
        raise ValueError("Expected one installed Transformers distribution")
    record = records[0]
    source = root / "utils" / "import_utils.py"
    state = source.stat()
    return {"format": FORMAT, "python": list(sys.version_info[:2]),
            "distribution": record.parent.name,
            "record": hashlib.sha256(record.read_bytes()).hexdigest(),
            "scanner": [state.st_size, state.st_mtime_ns]}


def encode(structure):
    result = []
    for backends, modules in structure.items():
        if not isinstance(backends, frozenset) or not isinstance(modules, dict):
            raise ValueError("Unsupported Transformers import structure")
        result.append([sorted(backends), {name: sorted(exports) for name, exports in modules.items()}])
    return result


def decode(rows, prefix=None):
    # Reconstruct fresh containers: Transformers adds package-level exports to
    # the result and must not mutate the retained index for subsequent queries.
    result = {}
    for backends, modules in rows:
        if not isinstance(backends, list) or not all(isinstance(x, str) for x in backends) or not isinstance(modules, dict):
            raise ValueError("Invalid import index")
        values = {}
        for name, exports in modules.items():
            if not isinstance(name, str) or not all(p.isidentifier() for p in name.split('.')):
                raise ValueError("Invalid module name")
            if not isinstance(exports, list) or not all(isinstance(x, str) and x.isidentifier() for x in exports):
                raise ValueError("Invalid export name")
            values[f"{prefix}.{name}" if prefix is not None else name] = set(exports)
        result[frozenset(backends)] = values
    return result


class ImportIndex:
    def __init__(self, root, document=None, record=False):
        self.root = Path(root)
        self.models = self.root / "models"
        self.record = record
        self.rows = None
        if document is not None and not isinstance(document, dict):
            raise ValueError("Invalid import index document")
        if document is not None and document.get("fingerprint") == fingerprint(self.root):
            decode(document["models"])
            self.rows = document["models"]

    def wrap(self, original):
        @functools.wraps(original)
        def indexed(module_path, prefix=None):
            path = Path(module_path)
            if path.name == "__init__.py":
                path = path.parent
            if path == self.models:
                if self.rows is None and self.record:
                    self.rows = encode(original(str(self.models), prefix=None))
                if self.rows is not None:
                    return decode(self.rows, prefix)
            return original(module_path, prefix=prefix)

        def clear():
            # Explicit upstream cache clearing requests a live source reload.
            self.rows = None
            self.record = False
            if hasattr(original, "cache_clear"):
                original.cache_clear()
        indexed.cache_clear = clear
        for name in ("cache_info", "cache_parameters"):
            if hasattr(original, name):
                setattr(indexed, name, getattr(original, name))
        return indexed

    def document(self):
        if self.rows is None:
            raise ValueError("No Transformers model index was generated")
        return {"fingerprint": fingerprint(self.root), "models": self.rows}


class _Finder:
    target = "transformers.utils.import_utils"

    def __init__(self, path, record):
        self.path = Path(path)
        self.record = record
        self.index = None

    def find_spec(self, fullname, path=None, target=None):
        if fullname != self.target:
            return None
        spec = importlib.machinery.PathFinder.find_spec(fullname, path, target)
        if spec is None or spec.loader is None:
            return None
        original = spec.loader
        owner = self

        class Loader:
            def create_module(self, spec):
                return original.create_module(spec) if hasattr(original, "create_module") else None

            def exec_module(self, module):
                original.exec_module(module)
                try:
                    document = None
                    if not owner.record:
                        document = json.loads(owner.path.read_text(encoding="utf-8"))
                    owner.index = ImportIndex(Path(spec.origin).parents[1], document, owner.record)
                    if owner.record or owner.index.rows is not None:
                        module.define_import_structure = owner.index.wrap(module.define_import_structure)
                    else:
                        owner.index = None
                except (OSError, ValueError, KeyError, TypeError, AttributeError):
                    # Optional acceleration never hides an invalid dependency.
                    owner.index = None
                finally:
                    if owner in sys.meta_path:
                        sys.meta_path.remove(owner)
        spec.loader = Loader()
        return spec


def install(path=None, record=False):
    finder = _Finder(path or default_path(), record)
    if finder.target not in sys.modules:
        sys.meta_path.insert(0, finder)
    return finder
