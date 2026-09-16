# iiLocalLLM local correction

Upstream: ggml-org/llama.cpp, revision `5202104b59ada9005db079eea43882a2b7bf5802`, MIT license (installed alongside this directory as `llama.cpp.LICENSE`).

iiLocalLLM 0.49 corrects `common/chat-auto-parser-generator.cpp`, `analyze_tools::build_tool_parser_json_native`. When a template wraps each JSON tool call separately, the outer `one_or_more` ignored `parallel_tool_calls=false`. The corrected branch allows only one wrapped call when this option is false and preserves repetition when it is true. Parser and sampler grammar share this definition.

`cmake/LlamaToolCalls.cmake` applies an exact source replacement to a copy under `build/llama-patches/` and compiles that copy into `llama-common`. It does not modify the upstream checkout or add a dependency. Configuration rejects an unrecognized grammar source form instead of silently skipping the correction. An external checkout containing the exact corrected form needs no replacement.

The regression test `tests/native_grammar_tests.cpp` uses the pinned upstream Qwen 2.5 and Qwen 3 templates to check actual grammar acceptance for one versus two calls in auto/required and single/parallel modes. The separate team task native test exercises real model generation through the installed SDK. This is a downstream correction, not a claim that upstream has accepted it.
