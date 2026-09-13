import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location("mlx_worker", Path(__file__).parents[1] / "runtimes/mlx_worker.py")
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class HistoryTests(unittest.TestCase):
    class Array(list):
        dtype = "int32"
        def tolist(self):
            return list(self)

    def test_cached_and_prefilled_tokens_are_present_for_every_penalty(self):
        mx = SimpleNamespace(array=lambda values, **_: self.Array(values),
                             concatenate=lambda arrays: self.Array(sum((list(a) for a in arrays), [])))
        observed = []
        def processor(tokens, logits):
            observed.append(list(tokens))
            return logits
        apply = worker.full_history_processor([1, 2, 3, 4], [processor], mx)
        logits = SimpleNamespace(shape=(1, 10))
        apply(self.Array([4]), logits)
        apply(self.Array([4, 5]), logits)
        self.assertEqual(observed, [[1, 2, 3, 4], [1, 2, 3, 4, 5]])
        invalid = worker.full_history_processor([1, 2, 3], [processor], mx, [10])
        with self.assertRaisesRegex(ValueError, "vocabulary"):
            invalid(self.Array([3]), logits)


class DeviceTests(unittest.TestCase):
    def device(self, metal):
        self.events = []
        return SimpleNamespace(cpu="cpu", gpu="metal", metal=SimpleNamespace(is_available=lambda: metal),
            set_default_device=lambda device: self.events.append(("selected", device)),
            array=lambda _: 1.0, eval=lambda value: self.events.append(("evaluated", value)))

    def test_cpu_does_not_depend_on_metal(self):
        self.assertEqual(worker.select_device(self.device(False), "cpu"), "cpu")
        self.assertEqual(self.events, [("selected", "cpu"), ("evaluated", 2.0)])

    def test_metal_requires_a_working_device(self):
        self.assertEqual(worker.select_device(self.device(True), "metal"), "metal")
        self.assertEqual(self.events[0], ("selected", "metal"))
        with self.assertRaises(RuntimeError):
            worker.select_device(self.device(False), "metal")
        self.assertEqual(self.events, [])

    def test_no_implicit_default_or_other_backend(self):
        for backend in (None, "", "auto", "cuda", "vulkan"):
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                worker.select_device(self.device(True), backend)
            self.assertEqual(self.events, [])


class TokenChunkTests(unittest.TestCase):
    class Detokenizer:
        def reset(self):
            self.pending = ""
            self.finished = False

        def add_token(self, token):
            self.pending += chr(token)

        def finalize(self):
            self.finished = True

        @property
        def last_segment(self):
            result, self.pending = self.pending, ""
            return result

    def chunks(self, tokens, maximum):
        self.detokenizer = self.Detokenizer()
        tokenizer = SimpleNamespace(detokenizer=self.detokenizer, eos_token_ids={0})
        return list(worker.token_chunks(tokenizer, ((token, None) for token in tokens), maximum))

    def test_cpu_stream_stops_at_eos_and_excludes_it(self):
        chunks = self.chunks([65, 0, 66], 8)
        self.assertEqual("".join(c.text for c in chunks), "A")
        self.assertEqual(chunks[-1].finish_reason, "stop")
        self.assertEqual(chunks[-1].generation_tokens, 2)
        self.assertTrue(self.detokenizer.finished)

    def test_cpu_stream_flushes_at_token_limit(self):
        chunks = self.chunks([65, 66, 67], 2)
        self.assertEqual("".join(c.text for c in chunks), "AB")
        self.assertEqual(chunks[-1].finish_reason, "length")
        self.assertEqual(chunks[-1].generation_tokens, 2)
        self.assertTrue(self.detokenizer.finished)


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.created = []
        self.trimmed = []

    def make(self, model):
        cache = object()
        self.created.append(cache)
        return cache

    def trim(self, cache, count):
        self.trimmed.append(count)
        return count

    def prepare(self, previous, tokens, trimmable=True):
        return worker.prepare_cache(None, previous, tokens, self.make, lambda _: trimmable, self.trim)

    def test_prefix_reuse_trims_divergent_suffix(self):
        cache = object()
        actual, count = self.prepare(([1, 2, 3, 4], cache), [1, 2, 9])
        self.assertIs(actual, cache)
        self.assertEqual(count, 2)
        self.assertEqual(self.trimmed, [2])

    def test_identical_prompt_replays_one_token(self):
        cache = object()
        actual, count = self.prepare(([1, 2, 3], cache), [1, 2, 3])
        self.assertIs(actual, cache)
        self.assertEqual(count, 2)
        self.assertEqual(self.trimmed, [1])

    def test_recurrent_cache_is_rebuilt(self):
        cache = object()
        actual, count = self.prepare(([1, 2], cache), [1, 2, 3], False)
        self.assertIsNot(actual, cache)
        self.assertEqual(count, 0)

    def test_no_prefix_never_reuses_state(self):
        cache = object()
        actual, count = self.prepare(([1, 2], cache), [9, 8])
        self.assertIsNot(actual, cache)
        self.assertEqual(count, 0)

    def test_failed_trim_rebuilds(self):
        cache = object()
        actual, count = worker.prepare_cache(None, ([1, 2, 3], cache), [1, 9], self.make, lambda _: True, lambda *_: 0)
        self.assertIsNot(actual, cache)
        self.assertEqual(count, 0)


if __name__ == "__main__":
    unittest.main()
