#pragma once
#include "Tools.h"

namespace iiLocalLLM::agent {
struct UserQuestionOptions {
    bool deferred = true;
    QString previewFormat = "markdown"; // markdown or html; renderers must treat either as untrusted content.
};
// Requires a trusted PermissionResponse or PermissionRequests channel. Initial
// callers cannot supply answers/annotations; a host response retains the exact
// questions and metadata and adds answers/annotations via updatedArguments.
IILOCALLLM_EXPORT Tool userQuestionTool(UserQuestionOptions = {});
}
