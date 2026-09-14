#pragma once
#include "PermissionRules.h"
#include "Tools.h"
namespace iiLocalLLM::agent::detail {
// Allow requires coverage of every Bash command. Deny/ask match any operation.
bool permissionRulesMatch(const QList<PermissionRule>&, const ToolDefinition&, const QJsonObject&, const ToolContext&, bool allow);
}
