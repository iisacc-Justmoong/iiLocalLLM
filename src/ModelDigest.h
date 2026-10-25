#pragma once
#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#ifdef Q_OS_MACOS
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <limits>
#else
#include <QtCore/QCryptographicHash>
#endif

namespace iiLocalLLM::detail {
// Model integrity always hashes the complete byte stream. The macOS platform
// implementation uses its accelerated SHA-256 rather than the portable fallback.
#ifdef Q_OS_MACOS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
class ModelDigest final {
public:
    ModelDigest() {
#ifdef Q_OS_MACOS
        CC_SHA256_Init(&state_);
#endif
    }
    void addData(QByteArrayView bytes) {
#ifdef Q_OS_MACOS
        while(!bytes.empty()) {
            const auto count=std::min<qsizetype>(bytes.size(),std::numeric_limits<CC_LONG>::max());
            CC_SHA256_Update(&state_,bytes.data(),static_cast<CC_LONG>(count));
            bytes=bytes.sliced(count);
        }
#else
        state_.addData(bytes);
#endif
    }
    QByteArray result() const {
#ifdef Q_OS_MACOS
        auto copy=state_;
        std::array<unsigned char,CC_SHA256_DIGEST_LENGTH> bytes{};
        CC_SHA256_Final(bytes.data(),&copy);
        return QByteArray(reinterpret_cast<const char*>(bytes.data()),bytes.size());
#else
        return state_.result();
#endif
    }
private:
#ifdef Q_OS_MACOS
    CC_SHA256_CTX state_{};
#else
    QCryptographicHash state_{QCryptographicHash::Sha256};
#endif
};
#ifdef Q_OS_MACOS
#pragma clang diagnostic pop
#endif
}
