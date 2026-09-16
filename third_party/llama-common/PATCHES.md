# iiLocalLLM local corrections

Upstream: ggml-org/llama.cpp, revision `5202104b59ada9005db079eea43882a2b7bf5802`, MIT license (installed alongside this directory as `llama.cpp.LICENSE`).

iiLocalLLM 0.49 corrects `common/chat-auto-parser-generator.cpp`, `analyze_tools::build_tool_parser_json_native`. When a template wraps each JSON tool call separately, the outer `one_or_more` ignored `parallel_tool_calls=false`. The corrected branch allows only one wrapped call when this option is false and preserves repetition when it is true. Parser and sampler grammar share this definition.

iiLocalLLM 0.50 also corrects `common/chat.cpp`, `common_chat_params_init_qwen3_coder`. Qwen3.5 selects this specialized XML parser, not the differential autoparser. The parser previously inspected only root properties/required, treated a string/object union as raw string, and accepted optional arguments only after all required arguments. `cmake/LlamaQwenTools.cpp.in` retains the pinned method's reasoning, continuation, call-count and token behavior, and changes its argument builder and matching template inputs:

- Self-contained object alternatives under root `anyOf` produce separate argument signatures. SendMessage supplies complete alternatives so plaintext requires summary while structured control messages do not.
- Explicit string fields remain raw XML values. Mixed or otherwise non-explicit-string fields use JSON encoding in all alternatives. This preserves both an object and a string containing the same JSON bytes without coercion.
- Fields that allow strings but require JSON encoding receive matching XML encoding instructions in the rendered tool description. Their historical string arguments are JSON-encoded only in the rendering copy. Stored transcripts, caller schemas and API argument types remain unchanged; objects remain objects. Prompt measurement and generation use the same template preparation.
- Up to six properties, the existing bounded permutation DAG allows required and optional arguments in any order and prevents duplicates. Larger signatures keep the upstream required-first/optional-tail behavior, including its duplicate-optional limitation.

This is not a full JSON Schema compiler. Partial object alternatives retain the root signature; root `oneOf`, arbitrary intersections and raw string constraints still need host validation. iiLocalLLM validates the complete input schema with its existing jsoncons validator before execution. Grammar-disabled generation can also produce invalid calls; no missing argument is synthesized.

`cmake/LlamaToolCalls.cmake` applies exact source replacements to copies under `build/llama-patches/` and compiles those copies into `llama-common`. It does not modify the upstream checkout or add a dependency. Configuration rejects an unrecognized source form instead of silently skipping a correction. The pinned Qwen method SHA-256 is `a44c51fac0c56a9aed8f048ad7467f593540eff6fb912799760dca734cd4c1bd`; an external checkout containing the exact corrected method needs no replacement.

The regression test `tests/native_grammar_tests.cpp` uses pinned upstream Qwen 2.5/3 templates for one versus two calls and Qwen3.5/3 templates for conditional arguments and typed parsing. It also covers XML argument permutations, duplicate rejection, partial alternatives and larger signatures. Separate team native tests exercise model generation through the source and installed SDK. These are downstream corrections, not a claim that upstream has accepted them.
