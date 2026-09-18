#pragma once
#include "PermissionRules.h"
#include "Tools.h"
namespace iiLocalLLM::agent::detail {
// Allow requires coverage of every Bash command. Deny/ask match any operation.
bool permissionRulesMatch(const QList<PermissionRule>&, const ToolDefinition&, const QJsonObject&, const ToolContext&, bool allow);
// The caller must use a fixed system PATH and an empty startup environment.
// Classification is deliberately conservative and is not an OS sandbox.
bool readOnlyShell(const QJsonObject&,const ToolContext&);
}
