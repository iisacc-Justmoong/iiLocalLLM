#pragma once
#include "Engine.h"
namespace iiLocalLLM::agent::detail {
AgentHookExecutor hookAgentExecutor(EngineOptions,std::shared_ptr<TaskStore>);
}
