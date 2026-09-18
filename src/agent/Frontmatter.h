#pragma once
#include "../Types.h"
namespace iiLocalLLM::agent::detail {
struct FrontmatterDocument { QJsonObject fields; QString body; };
// Bounded YAML mapping, no aliases or duplicate keys. Scalar spellings remain
// strings, except plain YAML null. Each consumer validates its own field types.
FrontmatterDocument readFrontmatter(const QByteArray&,const CancellationToken&);
}
