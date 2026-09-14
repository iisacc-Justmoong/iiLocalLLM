#pragma once
#include "Skills.h"
#include "Tools.h"
namespace iiLocalLLM::agent::detail {
Tool skillTool(const QString& workingDirectory, const SkillOptions&);
// Recover committed native Skill results; never rerun file reads or tools.
QList<Message> pendingSkillMessages(const QList<Message>&);
}
