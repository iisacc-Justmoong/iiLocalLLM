#include "Memory.h"
#include "Runtime.h"
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <cmath>
#include <limits>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#elif defined(Q_OS_WIN)
#define NOMINMAX
#include <windows.h>
#endif

namespace iiLocalLLM {
quint64 MemoryEstimate::totalBytes() const
{
    constexpr auto max = std::numeric_limits<quint64>::max();
    if (contextBytes > max - weightsBytes || overheadBytes > max - weightsBytes - contextBytes)
        throw Error(ErrorCode::ResourceLimit, QStringLiteral("Model memory estimate overflows"));
    return weightsBytes + contextBytes + overheadBytes;
}
std::optional<quint64> availableRamBytes()
{
#ifdef Q_OS_MACOS
    const auto host = mach_host_self();
    vm_size_t page = 0;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    const auto result = host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
    const auto pageResult = host_page_size(host, &page);
    mach_port_deallocate(mach_task_self(), host);
    // speculative_count is already included in free_count. Do not count it twice.
    if (result == KERN_SUCCESS && pageResult == KERN_SUCCESS)
        return (quint64(vm.free_count) + quint64(vm.inactive_count)) * page;
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) return memory.ullAvailPhys;
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/meminfo"));
    if (file.open(QIODevice::ReadOnly)) {
        for (const auto& line : file.readAll().split('\n')) {
            if (!line.startsWith("MemAvailable:")) continue;
            const auto fields = line.simplified().split(' ');
            bool ok = false;
            const auto kb = fields.value(1).toULongLong(&ok);
            if (ok && kb <= std::numeric_limits<quint64>::max() / 1024) return kb * 1024;
        }
    }
#endif
    return std::nullopt;
}
qint64 parseKeepAlive(const QJsonValue& value)
{
    if (value.isUndefined()) return -1;
    double ms = -1;
    if (value.isDouble()) ms = value.toDouble() * 1000;
    else if (value.isString()) {
        static const QRegularExpression pattern(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)(ms|s|m|h)?$"));
        const auto match = pattern.match(value.toString());
        if (match.hasMatch()) {
            const auto unit = match.captured(2);
            ms = match.captured(1).toDouble() * (unit == "ms" ? 1 : unit == "m" ? 60000 : unit == "h" ? 3600000 : 1000);
        }
    }
    if (!std::isfinite(ms) || ms < 0 || ms > 7.0 * 86400000 || ms != std::floor(ms))
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("keep_alive must be 0..7 days in seconds or ms/s/m/h, with millisecond precision"));
    return qint64(ms);
}
QJsonObject memoryEstimateObject(const MemoryEstimate& e)
{
    return {{"estimated_bytes", double(e.totalBytes())}, {"weights_bytes", double(e.weightsBytes)},
        {"context_bytes", double(e.contextBytes)}, {"overhead_bytes", double(e.overheadBytes)}, {"basis", e.basis}};
}
MemoryEstimate Runtime::estimateMemory(const ModelSpec& spec, int contextSlots) const
{
    quint64 weights = 0;
    const QFileInfo entry(spec.path);
    if (entry.isFile()) weights = quint64(entry.size());
    else if (entry.isDir()) {
        QDirIterator files(spec.path, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (files.hasNext()) { files.next(); weights += quint64(files.fileInfo().size()); }
    }
    // Unknown architectures reserve 256 KiB per token per context. Adapters refine this using metadata.
    return {weights, quint64(spec.contextTokens) * quint64(contextSlots) * 256 * 1024,
        std::max(quint64(256 * 1024 * 1024), weights / 4), QStringLiteral("file sizes + conservative unknown-architecture context reserve")};
}
} // namespace iiLocalLLM
