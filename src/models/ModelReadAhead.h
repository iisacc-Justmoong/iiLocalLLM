#pragma once
#include <QtCore/QFileDevice>
#include <algorithm>
#ifdef Q_OS_MACOS
#include <fcntl.h>
#endif

namespace iiLocalLLM::detail {
// An optional kernel I/O hint. Reads, hashes, cancellation and file ownership
// remain with the caller; an unsupported hint falls back to ordinary reads.
class ModelReadAhead final {
public:
    static constexpr qint64 windowBytes=8*1024*1024;
    explicit ModelReadAhead(const QFileDevice& file):file_(file),size_(file.size()) {}
    void prepare(qint64 position) {
#ifdef Q_OS_MACOS
        if(disabled_||!file_.isOpen()||position<0||position>=size_||position<next_)return;
        radvisory advice{};advice.ra_offset=position;
        advice.ra_count=static_cast<int>(std::min(windowBytes,size_-position));
        if(fcntl(file_.handle(),F_RDADVISE,&advice)<0){disabled_=true;return;}
        next_=position+std::min(windowBytes/2,size_-position);
#else
        (void)position;
#endif
    }
private:
    const QFileDevice& file_;qint64 size_=0,next_=0;bool disabled_=false;
};
}
