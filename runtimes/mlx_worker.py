#!/usr/bin/env python3
"""Private serial NDJSON worker. Model weights/tokenizer stay local; stdout is protocol only."""
import contextlib
import json
from pathlib import Path
from types import SimpleNamespace
import sys


def common_prefix(left, right):
    count = 0
    for a, b in zip(left, right):
        if a != b:
            break
        count += 1
    return count


def select_device(mx, backend):
    """Run before importing mlx_lm: its generation stream captures the default device."""
    if backend == "metal":
        if not mx.metal.is_available():
            raise RuntimeError("Selected Metal device is unavailable")
        device = mx.gpu
    elif backend == "cpu":
        device = mx.cpu
    else:
        raise ValueError("Service must select cpu or metal for this MLX worker")
    mx.set_default_device(device)
    mx.eval(mx.array([1.0]) + 1.0)
    return backend


def token_chunks(tokenizer, steps, maximum):
    """Adapt mlx_lm token generation to transport chunks without its GPU wired-limit wrapper."""
    detokenizer = tokenizer.detokenizer
    detokenizer.reset()
    for count, (token, _) in enumerate(steps, 1):
        stopped = token in tokenizer.eos_token_ids
        if not stopped:
            detokenizer.add_token(token)
        final = stopped or count == maximum
        if final:
            detokenizer.finalize()
        yield SimpleNamespace(text=detokenizer.last_segment, token=token, generation_tokens=count,
                              finish_reason=("stop" if stopped else "length") if final else None)
        if final:
            return


def prepare_cache(model, entry, tokens, make_cache, can_trim, trim):
    """Reuse only cache states whose exact token prefix can be safely truncated."""
    if entry is None:
        return make_cache(model), 0
    previous, cache = entry
    count = min(common_prefix(previous, tokens), len(tokens) - 1)
    if count <= 0 or not can_trim(cache):
        return make_cache(model), 0
    amount = len(previous) - count
    if amount and trim(cache, amount) != amount:
        return make_cache(model), 0
    return cache, count


def full_history_processor(prompt, processors, mx, biased_tokens=()):
    """Restore tokens omitted by prefix caching and chunked prefill before applying penalties."""
    prefix = None

    def process(history, logits):
        nonlocal prefix
        if prefix is None:
            observed = history.tolist()
            if not observed or len(observed) > len(prompt) or prompt[-len(observed):] != observed:
                raise ValueError("Unexpected MLX logits-processor history")
            if any(token >= logits.shape[-1] for token in biased_tokens):
                raise ValueError("logit_bias token is outside the vocabulary")
            prefix = mx.array(prompt[:len(prompt) - len(observed)], dtype=history.dtype)
        complete = mx.concatenate([prefix, history]) if len(prefix) else history
        for processor in processors:
            logits = processor(complete, logits)
        return logits

    return process


class Worker:
    def __init__(self, emit):
        self.emit = emit
        self.model = self.tokenizer = None
        self.context_tokens = 0
        self.contexts = {}
        self.backend = None

    def handle(self, request):
        op = request["op"]
        if op == "load":
            import mlx.core as mx
            self.backend = select_device(mx, request["backend"])
            from mlx_lm import load
            path = Path(request["path"]).resolve(strict=True)
            if not path.is_dir():
                raise ValueError("A local MLX model directory is required")
            self.model, self.tokenizer = load(str(path), tokenizer_config={"trust_remote_code": False})
            self.context_tokens = int(request["context_tokens"])
            self.contexts.clear()
            return {"backend": self.backend}
        if self.model is None:
            raise ValueError("No model loaded")
        if op == "tokenize":
            return {"tokens": self.tokenizer.apply_chat_template(
                request["messages"], tokenize=True, add_generation_prompt=True)}
        if op == "drop":
            self.contexts.pop(request["context_id"], None)
            import mlx.core as mx
            mx.clear_cache()
            return {}
        if op != "generate":
            raise ValueError("Unknown worker operation")
        import mlx.core as mx
        from mlx_lm import stream_generate
        from mlx_lm.models.cache import make_prompt_cache, can_trim_prompt_cache, trim_prompt_cache
        # Official pinned sampler revision fixes MLX 0.32 put_along_axis scalar incompatibility.
        from mlx_sample_utils import make_sampler, make_logits_processors
        tokens = request["tokens"]
        maximum = int(request["max_tokens"])
        if not tokens or maximum < 1 or len(tokens) + maximum > self.context_tokens:
            raise ValueError("Context token budget exceeded")
        key = request["context_id"]
        cache, reused = prepare_cache(self.model, self.contexts.pop(key, None), tokens,
                                     make_prompt_cache, can_trim_prompt_cache, trim_prompt_cache)
        mx.random.seed(request["seed"])
        if request.get("typical_p", 1) != 1:
            raise ValueError("MLX does not support typical_p")
        sampler_impl = None

        def sampler(logprobs):
            nonlocal sampler_impl
            if sampler_impl is None:
                vocab_size = logprobs.shape[-1]
                if request.get("min_keep", 1) > vocab_size:
                    raise ValueError("min_keep exceeds vocabulary size")
                sampler_impl = make_sampler(temp=request["temperature"], top_p=request["top_p"],
                    top_k=request["top_k"] if request["top_k"] < vocab_size else 0,
                    min_p=request.get("min_p", 0), min_tokens_to_keep=request.get("min_keep", 1),
                    xtc_probability=request.get("xtc_probability", 0), xtc_threshold=request.get("xtc_threshold", .1))
            return sampler_impl(logprobs)

        context_size = request.get("repetition_context_size", 64)
        penalties_enabled = context_size != 0
        if context_size < 0:
            context_size = self.context_tokens
        bias = {int(token): value for token, value in request.get("logit_bias", {}).items()}
        processors = make_logits_processors(logit_bias=bias,
            repetition_penalty=request.get("repetition_penalty", 1) if penalties_enabled and request.get("repetition_penalty", 1) != 1 else None,
            repetition_context_size=context_size,
            presence_penalty=request.get("presence_penalty", 0) if penalties_enabled else None,
            presence_context_size=context_size,
            frequency_penalty=request.get("frequency_penalty", 0) if penalties_enabled else None,
            frequency_context_size=context_size)
        processors = [full_history_processor(tokens, processors, mx, bias)] if processors else []
        all_tokens = tokens[:]
        last = None
        if self.backend == "cpu":
            # mlx-lm 0.31.3 stream_generate assumes Metal memory fields whenever Metal is installed,
            # even with a CPU default device. Keep its inference/sampler/cache, bypass only that wrapper.
            from mlx_lm.generate import generate_step
            steps = generate_step(mx.array(tokens[reused:]), self.model, max_tokens=maximum,
                                  sampler=sampler, logits_processors=processors, prompt_cache=cache, prefill_step_size=256)
            responses = token_chunks(self.tokenizer, steps, maximum)
        else:
            responses = stream_generate(self.model, self.tokenizer, tokens[reused:], max_tokens=maximum,
                                        sampler=sampler, logits_processors=processors, prompt_cache=cache, prefill_step_size=256)
        for response in responses:
            last = response
            all_tokens.append(int(response.token))
            if response.text:
                self.emit({"type": "delta", "text": response.text,
                           "generated_tokens": response.generation_tokens, "cached_tokens": reused})
        if last is None:
            raise RuntimeError("MLX returned no generation result")
        # Some implementations evaluate a token ahead, and recurrent caches may not be trimmable.
        # Record the observed cache offsets, never assume the number of streamed tokens equals KV length.
        offsets = [getattr(layer, "offset", None) for layer in cache]
        if (offsets and all(isinstance(n, int) and n == offsets[0] for n in offsets)
                and 0 < offsets[0] <= len(all_tokens) and can_trim_prompt_cache(cache)):
            self.contexts[key] = (all_tokens[:offsets[0]], cache)
        return {"finish_reason": last.finish_reason, "generated_tokens": last.generation_tokens,
                "cached_tokens": reused}


def main():
    protocol = sys.stdout

    def emit(value):
        protocol.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")
        protocol.flush()

    worker = Worker(emit)
    for line in sys.stdin:
        try:
            # Third-party load/generate diagnostics must not corrupt the protocol.
            with contextlib.redirect_stdout(sys.stderr):
                result = worker.handle(json.loads(line))
            emit({"type": "result", **result})
        except Exception as error:
            emit({"type": "error", "message": f"{type(error).__name__}: {error}"})


if __name__ == "__main__":
    main()
