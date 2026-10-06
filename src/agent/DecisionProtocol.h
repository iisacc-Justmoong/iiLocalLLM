#pragma once
#include "DecisionGate.h"
#include "Types.h"

namespace iiLocalLLM::agent {
// Trusted host configuration/return data. Never decoded from model arguments.
IILOCALLLM_EXPORT DecisionOptions decisionOptionsFromJson(const QJsonObject&);
IILOCALLLM_EXPORT QJsonObject toJson(const DecisionReport&);
namespace detail {
DecisionCandidate decisionCandidate(const ToolCall&,QString description = {});
bool decisionControl(const ToolDefinition&);
DecisionReport decide(const DecisionRequest&,const ToolContext&,const EventCallback&);
struct DeferredDecision {QJsonObject report;};
}
}
