#pragma once
#include "CommandHooks.h"
namespace iiLocalLLM::agent::detail {
struct PromptHook {QString prompt,model;};
struct PromptHookResult {HookResult result;Usage usage;QString model;};
PromptHookResult evaluatePromptHook(const PromptHook&,const HookInput&,const QByteArray&,
    const CommandHookOptions&,int timeoutMs,const CancellationToken&,const std::function<void()>& started);
}
