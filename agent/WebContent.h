#pragma once
#include "Types.h"
#include <QtCore/QUrl>
namespace iiLocalLLM::agent::detail {
QString webContent(const QByteArray&,const QByteArray& contentType,const QUrl&,int maxCharacters,
    bool& truncated,const CancellationToken&);
QString webExcerpt(QString,int maxCharacters);
}
