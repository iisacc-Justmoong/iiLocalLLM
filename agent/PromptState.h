#pragma once
#include "Types.h"

namespace iiLocalLLM::agent::detail {
inline QJsonObject promptState(const Message& message) {
    const auto state=message.metadata["iilocal.user_prompt_hook"].toObject();
    return message.role==MessageRole::User&&state["version"]==1?state:QJsonObject{};
}
inline bool rejectedPrompt(const Message& message) {return promptState(message)["disposition"]=="blocked";}
inline bool conversationInput(const Message& message) {
    return message.role==MessageRole::User&&!rejectedPrompt(message)&&!message.metadata.contains("iilocal.session_start");
}
inline Message promptForModel(Message message) {
    const auto state=promptState(message);
    if(state["disposition"]=="accepted"&&!state["context"].toString().isEmpty())
        message.text+="\n\nHost-provided UserPromptSubmit context:\n"+state["context"].toString();
    return message;
}
}
