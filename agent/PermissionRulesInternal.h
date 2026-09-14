#pragma once
#include "PermissionRules.h"
namespace iiLocalLLM::agent::detail {
// Allow requires coverage of every Bash command. Deny/ask match any operation.
bool permissionRulesMatch(const QStringList&, const ToolDefinition&, const QJsonObject&, const ToolContext&, bool allow);
}
