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

Native test commands reserve short isolated `resources/t/` workspaces through
iiFileProvider, including temporary files and application endpoints, and remove
them after each test. Cross-compiled tests preserve the original direct target
and emulator handling and need a suitable external temporary directory. Some
tests start services or model inference; those require an explicit execution
request. Recovered user data is ignored by Git and is not a build output.
Inference smoke executables are excluded from the default build when no model
fixture is configured; they remain explicit targets and are included when a
configured inference test needs them.
