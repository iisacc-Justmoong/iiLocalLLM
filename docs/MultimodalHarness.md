# Native multimodal harness

Version 0.55 carries typed media from user input and tool observations through the agent transcript, model routing, native budgeting, generation and compaction. The llama.cpp adapter links the pinned `libmtmd` implementation in process. Image/audio bytes become native embeddings rather than base64 text prompts. Inference uses the existing local Service scheduler.

## Input and persistence

`agent::RunRequest::content`, `Message::content` and `ToolResult::content` use a content-block array. Authenticated `agent.run`, `agent.inputs.enqueue` and their connection-owned MCP equivalents accept `content`. A content-only submission may omit the textual prompt. Queued media keeps existing priority, interruption, replay identity and acknowledgement contracts. Transcript recovery, session fork and forked skill execution retain the original blocks.

```json
{
  "session_id": "<owned session>",
  "prompt": "Compare these observations",
  "content": [
    {"type": "image", "mimeType": "image/png", "data": "<canonical base64>"},
    {"type": "audio", "mimeType": "audio/wav", "data": "<canonical base64>"},
    {"type": "video", "frames": [
      {"timestamp_ms": 0, "mimeType": "image/jpeg", "data": "<canonical base64>"},
      {"timestamp_ms": 1000, "mimeType": "image/jpeg", "data": "<canonical base64>"}
    ]}
  ]
}
```

Supported image formats are PNG, JPEG, BMP and GIF; audio formats are WAV, MP3 and FLAC. The selected projector must actually support the input modality. Video input is an ordered sequence of supplied image frames with strictly increasing nonnegative timestamps. The application owns container decoding and frame selection. MP4 decoding, automatic frame sampling, streaming capture and generated audio/video output are outside this contract.

Text blocks, embedded text resources and resource links remain observations. Embedded image/audio resource blobs become native media for inference, while the transcript preserves the original resource. Inline OpenAI `image_url` data URLs and `input_audio` blocks are also accepted by structured conversations. Remote URLs and arbitrary media paths are not fetched during inference.

Each content array is bounded to 64 blocks and 64 total images/audio items, including video frames. It uses the existing engine/service input-character budget (1 MiB by default). The service counts the full request, including tools and system instructions. Images are inspected before decoding and limited to 16 megapixels. All decoded images/audio in one request share a 128 MiB budget; image dimensions are checked against the remaining budget before allocation. Every request must also fit the model's native token context.

## Verified model packages

A GGUF manifest may contain an optional package-relative `projector` field:

```json
{
  "id": "vision-small",
  "architecture": "smolvlm",
  "format": "gguf",
  "quantization": "Q4_K_M",
  "context_length": 8192,
  "capabilities": ["chat", "text-generation", "image", "video"],
  "entry_point": "model.gguf",
  "projector": "mmproj.gguf"
}
```

The projector must be a separate file inside the package, have GGUF magic and pass the same inventory/hash checks as model weights. ModelManager supplies its resolved path to the runtime; it is not an arbitrary client model-load option. Memory admission includes projector weight bytes and an additional scratch reservation. The native projector uses the service-selected device. Missing, incompatible or modality-inappropriate projectors produce explicit errors. A declared capability alone is not successful inference proof.

## iiDecision model-path optimization

The existing run and tool probability/value gates remain enabled by default. Trusted `DecisionOptions::modelRoutes` adds a `model_route` assessment before each model measurement/generation, using the complete model-visible history. New tool media and queued input can change the next model selection. After compaction, the visible requirements are assessed again.

`ModelRoute` and modality masks are standard C++23 contracts without Qt dependencies. Text/image/audio/video bits are 1/2/4/8; video also requires image support. Unsupported routes are removed before economics are compared. iiDecision computes evidence-conditioned success/failure probabilities and selects the highest net expected value under the existing probability and value thresholds. Ties keep configured order. A cheaper adequate route can win over an expensive general model.

The existing private daemon `--agent-decision` and MCP `--decision` configuration accepts:

```json
{
  "enabled": true,
  "value_unit": "host-value",
  "model_routes": [
    {"id": "fast", "model": "model://text-small", "modalities": ["text"]},
    {"id": "vision", "model": "model://vision-small", "modalities": ["text", "image", "video"]},
    {"id": "audio", "model": "model://audio-small", "modalities": ["text", "audio"]}
  ],
  "profiles": [
    {"candidate_id": "run", "success_gain": 100, "failure_loss": 10, "cost": 1, "success_probability": 0.9},
    {"candidate_id": "model:fast", "success_gain": 100, "failure_loss": 10, "cost": 1, "success_probability": 0.9},
    {"candidate_id": "model:vision", "success_gain": 100, "failure_loss": 10, "cost": 3, "success_probability": 0.9},
    {"candidate_id": "model:audio", "success_gain": 100, "failure_loss": 10, "cost": 3, "success_probability": 0.9}
  ]
}
```

These are illustrative host inputs, not measured model accuracy or prices. Production hosts can use `DecisionOptions::inputs` or authenticated `decision_input` returns for goal-specific estimates and finite joint evidence models. Routing requests expose the current goal, modality mask, message/tool counts and media SHA-256/size/MIME/timestamps without base64 payloads. Generated model assertions are not trusted economic estimates.

Missing/low-value route assessments return `RunStatus::Deferred` before native measurement/generation. No capable route returns `unsupported_modalities`. `RunResult::selectedModel` and `routing` expose model selection separately from `decision` (run/tool economics). Routes are trusted host configuration; model arguments, transcripts and `agent.run` parameters cannot change them. Empty routes retain the configured session model, which can still consume native media. Explicit `decision.enabled=false` selects the first compatible configured route and reports that opt-out.

This compares configured alternatives at the current turn. It is not a learned global planner or a model-quality calibration system.

## Budgets, cache and compaction

`RuntimeConversationPrompt::tokenCount()` includes media embeddings and native temporal positions. Measurement and generation use the same preparation path and context checks. Text token vectors remain available for sampler penalties; native embedding chunks supply the media. Unpredictable host markers and typed parts prevent literal marker text from impersonating a bitmap slot.

Media prompts use a fresh KV prefix and report zero cached tokens. Text conversations retain existing prefix reuse. Projector scratch work is serialized. Cancellation is checked between native chunks and during llama decoding; an encoder operation already in progress may finish before cancellation is observed.

Micro-compaction can clear old tool media from the model-visible view while preserving original transcript records. Semantic summarization passes selected media as typed content with source message IDs. The exact latest user input remains retained. Summaries are model-generated handoffs, not proof that media interpretation is correct.

## Reproducible verification

`iiLocalLLM.multimodal` covers validation, temporal order, host configuration, capability filtering, economic deferral, content-only input, recovery/fork, queued mixed history, tool-triggered rerouting and authenticated API delivery. Existing decision/agent/service/input/MCP/compaction tests remain regression checks.

Optional native tests use local packages and fixtures; test execution never downloads models. The vision smoke uses the supplied image and a sibling `blue.png` for its timestamped frame pair. It also creates compressed 2048-pixel PNGs in memory to verify the aggregate decoded-media budget.

```sh
cmake -S . -B build \
  -DIILOCALLLM_TEST_VISION_PACKAGE=/absolute/package \
  -DIILOCALLLM_TEST_IMAGE=/absolute/red.png \
  -DIILOCALLLM_TEST_AUDIO_PACKAGE=/absolute/audio-package \
  -DIILOCALLLM_TEST_AUDIO=/absolute/speech.wav
cmake --build build -j 6
ctest --test-dir build -R '^iiLocalLLM.multimodal' --output-on-failure
```

Native JSON receipts include actual image/video or audio generation, the selected iiDecision route, text/media token budgets, aggregate memory admission and unsupported-modality rejection. Installed consumers compile the same smoke against the staged SDK to check public headers, dependencies and dylib loading. See [Verification.md](Verification.md) for measured evidence and platform boundaries.
