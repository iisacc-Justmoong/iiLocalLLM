// Test-only macOS interposer: observe the synthetic GGUF mapping, and suppress
// its eager prefetch so the regression fails promptly rather than blocking.
#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <sys/stat.h>

namespace {
std::atomic<bool> active{false};
std::atomic<unsigned> mappings{0}, prefetches{0};
std::atomic<std::uintptr_t> mappedBegin{0}, mappedEnd{0};
dev_t targetDevice{};
ino_t targetInode{};

void* observeMmap(void* address, size_t size, int protection, int flags, int fd, off_t offset)
{
    auto* result = mmap(address, size, protection, flags, fd, offset);
    if (active.load() && result != MAP_FAILED && fd >= 0) {
        struct stat info{};
        if (fstat(fd, &info) == 0 && info.st_dev == targetDevice && info.st_ino == targetInode) {
            mappedBegin.store(reinterpret_cast<std::uintptr_t>(result));
            mappedEnd.store(reinterpret_cast<std::uintptr_t>(result) + size);
            ++mappings;
        }
    }
    return result;
}

int observeMadvise(void* address, size_t size, int advice)
{
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (active.load() && advice == POSIX_MADV_WILLNEED &&
        mappedBegin.load() != 0 && begin >= mappedBegin.load() && begin < mappedEnd.load()) {
        ++prefetches;
        return 0;
    }
    return posix_madvise(address, size, advice);
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void* replacement; const void* original; } mmapInterpose = {
    reinterpret_cast<const void*>(&observeMmap), reinterpret_cast<const void*>(&mmap)};
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void* replacement; const void* original; } madviseInterpose = {
    reinterpret_cast<const void*>(&observeMadvise), reinterpret_cast<const void*>(&posix_madvise)};
}

extern "C" void iilocal_test_mmap_begin(std::uint64_t device, std::uint64_t inode)
{
    targetDevice = static_cast<dev_t>(device); targetInode = static_cast<ino_t>(inode);
    mappings = 0; prefetches = 0; mappedBegin = 0; mappedEnd = 0; active = true;
}
extern "C" void iilocal_test_mmap_end() { active = false; }
extern "C" unsigned iilocal_test_mmap_count() { return mappings.load(); }
extern "C" unsigned iilocal_test_prefetch_count() { return prefetches.load(); }
