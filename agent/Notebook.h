#pragma once
#include "../Types.h"

namespace iiLocalLLM::agent {
// Pure nbformat-4 transformation. File access, permissions, observed hashes and
// atomic persistence belong to the workspace tool that calls this function.
struct NotebookEditResult {
    QByteArray content;
    QJsonObject metadata;
};
IILOCALLLM_EXPORT void validateNotebookEditArguments(const QJsonObject&);
IILOCALLLM_EXPORT NotebookEditResult editNotebook(const QByteArray&,const QJsonObject&,
    const CancellationToken& = {});
}
