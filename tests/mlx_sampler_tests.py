"""Numerical regression for the pinned official sampler and complete penalty history."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtimes'))
import mlx.core as mx
from mlx_sample_utils import apply_min_p, make_logits_processors
from mlx_worker import full_history_processor


class SamplerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        mx.set_default_device(mx.cpu)

    def test_min_keep_retains_exact_top_candidates_below_threshold(self):
        logits=mx.log(mx.array([.6,.25,.1,.05]))
        output=apply_min_p(logits,.9,2)
        mx.eval(output)
        self.assertEqual(mx.isfinite(output).tolist(),[True,True,False,False])
        self.assertEqual(output[:2].tolist(),logits[:2].tolist())

    def test_repetition_presence_and_frequency_include_cached_prefix(self):
        processors=make_logits_processors(repetition_penalty=2,repetition_context_size=20,
            presence_penalty=.5,presence_context_size=20,frequency_penalty=.25,frequency_context_size=20)
        apply=full_history_processor([1,1,2],[*processors],mx)
        # Only the last prompt token reaches generate_step's first logits processor.
        output=apply(mx.array([2]),mx.array([[4.,4.,4.,4.]]))
        mx.eval(output)
        self.assertEqual(output.tolist(),[[4.,1.,1.25,4.]])

    def test_out_of_vocabulary_bias_is_rejected(self):
        processors=make_logits_processors(logit_bias={99:5})
        apply=full_history_processor([1],[*processors],mx,[99])
        with self.assertRaisesRegex(ValueError,'vocabulary'):
            apply(mx.array([1]),mx.array([[1.,2.]]))


if __name__=='__main__':
    unittest.main()
