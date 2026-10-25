import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("mlx_import_index", Path(__file__).parents[1] / "src/runtimes/mlx_import_index.py")
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class ImportIndexTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=Path.cwd())
        self.addCleanup(self.directory.cleanup)
        site = Path(self.directory.name)
        self.root = site / "transformers"
        (self.root / "models").mkdir(parents=True)
        (self.root / "utils").mkdir()
        (self.root / "utils/import_utils.py").write_text("scanner v1", encoding="utf-8")
        self.record = site / "transformers-5.0.dist-info/RECORD"
        self.record.parent.mkdir()
        self.record.write_text("installed source inventory", encoding="utf-8")
        self.structure = {frozenset(): {"alpha.configuration_alpha": {"AlphaConfig"}},
                          frozenset({"torch"}): {"alpha.modeling_alpha": {"AlphaModel", "AlphaPreTrainedModel"}}}
        self.document = {"fingerprint": cache.fingerprint(self.root), "models": cache.encode(self.structure)}

    def test_exact_structure_and_prefix_are_reconstructed_without_scanning(self):
        def forbidden(*args, **kwargs):
            raise AssertionError("The unchanged model source tree must not be scanned again")
        indexed = cache.ImportIndex(self.root, self.document).wrap(forbidden)
        self.assertEqual(indexed(self.root / "models"), self.structure)
        expected = {key: {"models." + name: value for name, value in modules.items()}
                    for key, modules in self.structure.items()}
        self.assertEqual(indexed(self.root / "models/__init__.py", prefix="models"), expected)
        result = indexed(self.root / "models")
        result[frozenset()]["injected"] = {"OtherClass"}
        self.assertEqual(indexed(self.root / "models"), self.structure)

    def test_dependency_changes_and_explicit_clearing_use_the_original_scanner(self):
        calls = []
        def original(path, prefix=None):
            calls.append((path, prefix))
            return {frozenset(): {"fresh": {"FreshClass"}}}
        self.record.write_text("new dependency source inventory", encoding="utf-8")
        index = cache.ImportIndex(self.root, self.document)
        self.assertIsNone(index.rows)
        indexed = index.wrap(original)
        self.assertEqual(indexed(self.root / "models"), original(None))
        self.assertEqual(len(calls), 2)
        self.document["fingerprint"] = cache.fingerprint(self.root)
        indexed = cache.ImportIndex(self.root, self.document).wrap(original)
        self.assertEqual(indexed(self.root / "models"), self.structure)
        indexed.cache_clear()
        self.assertEqual(indexed(self.root / "models"), original(None))

    def test_corrupt_metadata_is_rejected_as_data(self):
        for malformed in ([], {"models": [[[], {"../../outside": ["Class"]}]], "fingerprint": cache.fingerprint(self.root)},
                          {"models": [[[], {"valid.module": [42]}]], "fingerprint": cache.fingerprint(self.root)}):
            with self.subTest(document=malformed), self.assertRaises((ValueError, TypeError)):
                cache.ImportIndex(self.root, malformed)

    def test_build_recording_uses_the_official_scanner_once(self):
        calls = []
        def original(path, prefix=None):
            calls.append((path, prefix))
            return self.structure
        index = cache.ImportIndex(self.root, record=True)
        indexed = index.wrap(original)
        indexed(self.root / "models", prefix="models")
        indexed(self.root / "models")
        self.assertEqual(calls, [(str(self.root / "models"), None)])
        self.assertEqual(cache.ImportIndex(self.root, index.document()).rows, cache.encode(self.structure))
        indexed(self.root / "unrelated")
        self.assertEqual(len(calls), 2)

    def test_optional_hook_preserves_the_original_function_on_a_cache_miss(self):
        original = lambda *args, **kwargs: "live scanner"
        class Loader:
            def exec_module(self, module):
                module.define_import_structure = original
        destination = Path(self.directory.name) / "index.json"
        self.document["fingerprint"]["format"] = -1
        destination.write_text(json.dumps(self.document), encoding="utf-8")
        finder = cache._Finder(destination, record=False)
        spec = SimpleNamespace(origin=str(self.root / "utils/import_utils.py"), loader=Loader())
        with patch.object(cache.importlib.machinery.PathFinder, "find_spec", return_value=spec):
            wrapped = finder.find_spec(finder.target)
        module = SimpleNamespace()
        wrapped.loader.exec_module(module)
        self.assertIs(module.define_import_structure, original)
        self.assertIsNone(finder.index)


if __name__ == "__main__":
    unittest.main()
