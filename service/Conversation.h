#pragma once
#include "../Runtime.h"

namespace iiLocalLLM::detail {
void validateConversationRequest(const ConversationRequest&, int maxCharacters);
void validateConversationReply(RuntimeConversationReply&, const ConversationRequest&);
}
