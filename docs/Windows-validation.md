# Windows validation (2026-10-19)

The MSVC x64 Release build completes with Qt 6.8.3 and CMake 4.4.4. On a
case-insensitive filesystem, the C runtime's `memory.h` lookup can reach the
SDK's `Memory.h` inside an `extern "C"` block. Its C++ declarations now explicitly
retain C++ linkage. The MemoryHeaderInterop regression includes that header
inside C linkage and verifies both C memory functions and the native RAM query.

The full build contains 73 CTest suites. After a complete run and a focused
retry with Git for Windows available, 66 suites pass. Seven suites still fail:

- iiLocalLLM.memory_dream
- iiLocalLLM.session_history
- iiLocalLLM.session_history_engine
- iiLocalLLM.shell_tasks
- iiLocalLLM.discovery
- iiLocalLLM.mcp_http_client
- iiLocalLLM.mcp_stdio

The MemoryHeaderInterop regression passes. The remaining full-suite failures
prevent installation in this maintenance run. No tests are disabled. Native
model inference that requires separately provisioned weights was not exercised.
