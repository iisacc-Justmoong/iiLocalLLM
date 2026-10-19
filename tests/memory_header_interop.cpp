extern "C" {
#include "Memory.h"
}
#include <cstring>

int main()
{
    char destination[5]{};
    std::memcpy(destination,"test",4);
    if (std::strcmp(destination,"test")!=0) return 1;
    const auto available=iiLocalLLM::availableRamBytes();
    return available && *available==0 ? 2 : 0;
}
