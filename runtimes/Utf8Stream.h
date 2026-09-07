#pragma once
#include <QtCore/QStringConverter>

namespace iiLocalLLM::detail {
// Qt owns incremental UTF-8 decoding. A truncated final code point is visible as U+FFFD.
class Utf8Stream : public QStringDecoder {
public:
    Utf8Stream() : QStringDecoder(QStringDecoder::Utf8) {}
    QString finish()
    {
        const bool incomplete = state.remainingChars != 0;
        resetState();
        return incomplete ? QString(QChar::ReplacementCharacter) : QString{};
    }
};
}
