# File creation ownership in iiLocalLLM

Agent artifacts, workspace writes, checkpoints, session/task/permission state, plugin installation, web-fetch artifacts and model catalog/registry storage use provider devices and confined native descriptors. Agent authorization and model semantics remain here.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/service/LocalIpcServer.cpp`
- `src/models/ModelCatalog.cpp`
- `src/agent/Api.cpp`
- `src/agent/FileCheckpoints.cpp`
- `src/agent/HookAgent.cpp`
- `src/agent/InputQueue.cpp`
- `src/agent/MemoryDream.cpp`
- `src/agent/PermissionSettingsFile.h`
- `src/agent/PlanMode.cpp`
- `src/agent/Plugins.cpp`
- `src/agent/ProjectMemory.cpp`
- `src/agent/SessionOwners.h`
- `src/agent/SessionStore.cpp`
- `src/agent/ShellTasks.cpp`
- `src/agent/Subagents.cpp`
- `src/agent/TaskStore.cpp`
- `src/agent/Teams.cpp`
- `src/agent/Tools.cpp`
- `src/agent/WebFetch.cpp`
- `src/agent/WorkspaceTools.cpp`
- `src/agent/Worktrees.cpp`
- `src/mcp/LocalApplications.cpp`
- `src/service/ModelRegistry.cpp`
- `tools/mcp_main.cpp`
