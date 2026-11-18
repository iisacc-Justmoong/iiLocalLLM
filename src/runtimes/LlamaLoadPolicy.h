#pragma once

#include "../core/Types.h"

namespace iiLocalLLM::detail {
inline bool useCpuRepack(const QJsonObject& options)
{
    const auto value = options.value(QStringLiteral("cpu_repack"));
    if (value.isUndefined()) return true;
    if (!value.isBool())
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("cpu_repack must be a boolean"));
    return value.toBool();
}
}
