# Build and resource layout

The module, its service entry points, and selected tests are built in `build/`.
The pinned llama.cpp source is kept under `resources/dependencies/llama.cpp`;
its compiled files and generated patch copies remain in `build/`. An explicit
`IILOCALLLM_LLAMA_SOURCE_DIR` continues to select a different checkout. Models,
fixtures and Python environments recovered from older builds are preserved under
`resources/`, with compatibility links while another module still refers to the
old paths. These links do not imply that model inference was run.
The archive checksum applies when the source is first fetched; an existing
resource checkout is reused without rehashing its contents. Preserve local edits
and refresh that checkout explicitly when changing the upstream pin.

For macOS, the build compiles a narrowly patched copy of the pinned llama.cpp
model loader from `build/`: mmap stays enabled, while whole-model eager prefetch
is disabled. The original resource checkout is not modified. Configuration
fails when a different upstream loader cannot be matched safely. The native
regression observes mmap and prefetch calls during a synthetic model load.

Native test commands reserve short isolated `resources/t/` workspaces through
iiFileProvider, including temporary files and application endpoints, and remove
them after each test. Cross-compiled tests preserve the original direct target
and emulator handling and need a suitable external temporary directory. Some
tests start services or model inference; those require an explicit execution
request. Recovered user data is ignored by Git and is not a build output.
Inference smoke executables are excluded from the default build when no model
fixture is configured; they remain explicit targets and are included when a
configured inference test needs them.

Parameter validation and model catalog tests run through CTest without linking
Qt Test. Catalog fixtures use the resource workspace temporary directory; they
do not load models or perform inference.

The native grammar test runs its existing template and option combinations as
individual CTest-runner cases without Qt Test. It parses grammar and templates
only; it does not load model weights or run inference.

Residency and idle-scheduler cases use CTest and retain their timed event
processing, including the pinned-model and no-polling regression cases.

Hardware policy retains every device and error data row. The offline selection
uses synthetic inventories and runtimes; the two real hardware snapshot cases
remain in the full test and are excluded from the offline selection.

Service tests preserve per-case fixture setup and cleanup, including cleanup on
assertion failure. Their full suite includes local IPC and hardware discovery;
the offline selection runs only the UTF-8 decoder case.

The HTTP test preserves its RPC cancellation and slow-response data rows as
CTest cases. The complete module test configuration no longer requests Qt
Test. HTTP cases start a local server and require an explicit execution request.
