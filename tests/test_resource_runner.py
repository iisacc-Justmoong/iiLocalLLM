"""Contract tests use unique, disposable resource workspaces."""
import importlib.util
from pathlib import Path
import subprocess
import shutil
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import ii_file_provider as provider

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("resource_runner", ROOT / "tests/resource_runner.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class WorkspaceRunnerTest(unittest.TestCase):
    def test_symlink_resource_root_is_rejected(self):
        state = ROOT / "resources/t"
        provider.mkdir(state, parents=True, exist_ok=True)
        owner = state / runner.uuid.uuid4().hex
        provider.mkdir(owner, exist_ok=False)
        link = owner / "resources"
        try:
            try:
                link.symlink_to(ROOT / "resources", target_is_directory=True)
            except OSError as error:
                self.skipTest(f"Cannot create a symbolic link on this platform: {error}")
            with self.assertRaises(ValueError):
                runner.run(link, ["test"])
            self.assertTrue(link.is_symlink())
        finally:
            shutil.rmtree(owner)

    def test_unique_workspaces_are_confined_and_removed(self):
        seen = []
        def execute(args, **kwargs):
            path = Path(kwargs["cwd"])
            self.assertTrue(path.is_relative_to(ROOT / "resources/t"))
            self.assertFalse(any(part.startswith(".") for part in path.parts if part not in (".", "..")))
            self.assertEqual(args[-1], str(path / "input"))
            self.assertEqual(kwargs["env"]["TMPDIR"], str(path))
            self.assertEqual(kwargs["env"]["IIFILEPROVIDER_TEST_DIRECTORY"], str(path))
            self.assertEqual(kwargs["env"]["IILOCALLLM_APP_ENDPOINTS"], str(path / "app-endpoints"))
            self.assertEqual(kwargs["env"]["PYTHONDONTWRITEBYTECODE"], "1")
            seen.append(path)
            return subprocess.CompletedProcess(args, 0)
        for _ in range(2):
            self.assertEqual(runner.run(ROOT / "resources", ["test", "@WORK@/input"], execute=execute), 0)
            self.assertFalse(seen[-1].exists())
        self.assertNotEqual(seen[0], seen[1])

    def test_failure_cleans_workspace_and_preserves_status(self):
        seen = []
        def execute(args, **kwargs):
            seen.append(Path(kwargs["cwd"]))
            return subprocess.CompletedProcess(args, 7)
        self.assertEqual(runner.run(ROOT / "resources", ["test"], execute=execute), 7)
        self.assertFalse(seen[0].exists())

    def test_exception_also_cleans_workspace(self):
        seen = []
        def execute(args, **kwargs):
            seen.append(Path(kwargs["cwd"]))
            raise RuntimeError("expected test failure")
        with self.assertRaises(RuntimeError):
            runner.run(ROOT / "resources", ["test"], execute=execute)
        self.assertFalse(seen[0].exists())

    def test_collision_preserves_the_existing_workspace(self):
        state = ROOT / "resources/t"
        provider.mkdir(state, parents=True, exist_ok=True)
        token = runner.uuid.uuid4().hex[:4]
        while token in runner._issued or (state / token).exists():
            token = runner.uuid.uuid4().hex[:4]
        other = runner.uuid.uuid4().hex[:4]
        while other == token or other in runner._issued or (state / other).exists():
            other = runner.uuid.uuid4().hex[:4]
        blocked = state / token
        provider.mkdir(blocked, exist_ok=False)
        def execute(args, **kwargs):
            self.assertEqual(Path(kwargs["cwd"]).name, other)
            self.assertTrue(blocked.is_dir())
            return subprocess.CompletedProcess(args, 0)
        try:
            with patch.object(runner.uuid, "uuid4", side_effect=[SimpleNamespace(hex=token), SimpleNamespace(hex=other)]):
                self.assertEqual(runner.run(ROOT / "resources", ["test"], execute=execute), 0)
            self.assertTrue(blocked.is_dir())
            self.assertFalse((state / other).exists())
        finally:
            blocked.rmdir()

if __name__ == "__main__":
    unittest.main()
