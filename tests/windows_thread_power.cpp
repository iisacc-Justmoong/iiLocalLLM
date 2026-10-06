#include "../cmake/WindowsThreadPower.h"

int main() {
    THREAD_POWER_THROTTLING_STATE state{};
    state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    return SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling,
                                &state, sizeof(state)) ? 0 : 1;
}
