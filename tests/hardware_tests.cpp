#include <iiLocalLLM.h>
#include "../src/service/Managers.h"
#include "CTestSupport.h"
#include <QtCore/QCoreApplication>
#include <array>
#include <string_view>
#include <QtCore/QJsonArray>

using namespace iiLocalLLM;
namespace {
GpuInfo gpu(QString id, GpuVendor vendor, ComputeBackend backend, quint64 memory = 0)
{
    GpuInfo result;
    result.id = id;
    result.name = id;
    result.vendor = vendor;
    if (memory) result.vramBytes = memory;
    result.availableBackends = {backend};
    return result;
}
HardwareInfo inventory(QList<GpuInfo> gpus, bool apple = false)
{
    HardwareInfo h;
    h.appleSilicon = apple;
    h.gpus = std::move(gpus);
    for (const auto& g : h.gpus) {
        h.metalAvailable |= g.availableBackends.contains(ComputeBackend::Metal);
        h.cudaAvailable |= g.availableBackends.contains(ComputeBackend::Cuda);
        h.vulkanAvailable |= g.availableBackends.contains(ComputeBackend::Vulkan);
    }
    return h;
}
QList<RuntimeDevice> available(const HardwareInfo& h)
{
    QList<RuntimeDevice> result{{ComputeBackend::Cpu, {}}};
    for (const auto& g : h.gpus)
        for (const auto backend : g.availableBackends) result.append({backend, g.id});
    return result;
}
class Model final : public RuntimeModel {
public:
    TokenList tokenize(const QList<ChatMessage>&, const CancellationToken&) override { return {1}; }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken&) override { return {}; }
};
class RuntimeProbe final : public Runtime {
public:
    QString name = QStringLiteral("probe");
    QString path = QStringLiteral("fixture");
    QList<RuntimeDevice> supported;
    QList<RuntimeDevice> attempts;
    ErrorCode gpuFailure = ErrorCode::None;
    bool failCpu = false;
    QString id() const override { return name; }
    bool supportsModel(const ModelSpec& model) const override { return model.path == path; }
    QList<RuntimeDevice> devices(const HardwareInfo&) const override { return supported; }
    std::shared_ptr<RuntimeModel> load(const ModelSpec&, const RuntimeDevice& device, const CancellationToken&) override
    {
        attempts.append(device);
        if (device.backend != ComputeBackend::Cpu && gpuFailure != ErrorCode::None)
            throw Error(gpuFailure, QStringLiteral("injected GPU initialization error"));
        if (device.backend == ComputeBackend::Cpu && failCpu)
            throw Error(ErrorCode::RuntimeFailure, QStringLiteral("injected CPU error"));
        return std::make_shared<Model>();
    }
};
}
class HardwareTests {
public:
    void nativeRuntimeLimits()
    {
        CTEST_CHECK(createLlamaRuntime());
        CTEST_CHECK(createLlamaRuntime({false, 0, true}));
        CTEST_CHECK(createLlamaRuntime({}, LlamaKvCache::Q8_0));
        CTEST_CHECK(createLlamaRuntime({true, 4}, LlamaKvCache::Q4_0));
        CTEST_THROWS(Error, createLlamaRuntime({}, static_cast<LlamaKvCache>(99)));
        LlamaMemoryOptions memory;
        memory.kvCache = LlamaKvCache::Q8_0;
        memory.runtime.maxGpuLayers = 0;
        Service service({}, {}, memory);
        CTEST_CHECK(service.hardware().ramBytes.has_value());
        const auto mocked = inventory({gpu("llama.cpp/MTL/MTL0", GpuVendor::Apple, ComputeBackend::Metal)}, true);
        CTEST_COMPARE(createLlamaRuntime(memory.runtime, memory.kvCache)->devices(mocked).size(), llamaRuntimeAvailable() ? 1 : 0);
        try {
            createLlamaRuntime({true, -2});
            iillm_ctest::fail(__FILE__, __LINE__, "A negative GPU layer limit below -1 must be rejected");
        } catch (const Error& error) {
            CTEST_COMPARE(error.code(), ErrorCode::InvalidArgument);
        }
    }
    void policy(const HardwareInfo& hardware, ComputeBackend expected)
    {
        const auto ordered = orderExecutionDevices(hardware, available(hardware));
        CTEST_CHECK(!ordered.isEmpty());
        CTEST_COMPARE(ordered.first().backend, expected);
        CTEST_COMPARE(ordered.last().backend, ComputeBackend::Cpu);
    }
    void unsupportedAndUnavailableAccelerationIsExcluded()
    {
        auto h = inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda)});
        CTEST_COMPARE(orderExecutionDevices(h, {{ComputeBackend::Cpu, {}}}).first().backend, ComputeBackend::Cpu);
        h.cudaAvailable = false;
        CTEST_COMPARE(orderExecutionDevices(h, available(h)).first().backend, ComputeBackend::Cpu);
        h.cudaAvailable = true;
        h.gpus.first().availableBackends.clear();
        CTEST_COMPARE(orderExecutionDevices(h, {{ComputeBackend::Cuda, "nvidia"}, {ComputeBackend::Cpu, {}}}).size(), 1);
        CTEST_CHECK(orderExecutionDevices(h, {{ComputeBackend::Cuda, "missing"}}).isEmpty());
    }
    void neverCombinesCapabilitiesFromDifferentAdapters()
    {
        const auto h = inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Vulkan),
            gpu("amd", GpuVendor::Amd, ComputeBackend::Cuda)});
        CTEST_COMPARE(orderExecutionDevices(h, available(h)).first().backend, ComputeBackend::Cpu);
    }
    void sameBackendUsesMemoryThenStableId()
    {
        const auto h = inventory({gpu("z", GpuVendor::Amd, ComputeBackend::Vulkan, 32),
            gpu("b", GpuVendor::Intel, ComputeBackend::Vulkan, 8), gpu("a", GpuVendor::Amd, ComputeBackend::Vulkan, 32)});
        auto candidates = available(h);
        candidates.append(candidates);
        const auto ordered = orderExecutionDevices(h, candidates);
        CTEST_COMPARE(ordered.size(), 4);
        CTEST_COMPARE(ordered[0].deviceId, QStringLiteral("a"));
        CTEST_COMPARE(ordered[1].deviceId, QStringLiteral("z"));
    }
    void unifiedMemoryIsNotReportedAsVram()
    {
        auto g = gpu("apple", GpuVendor::Apple, ComputeBackend::Metal);
        g.unifiedMemory = true;
        g.recommendedWorkingSetBytes = 24ull * 1024 * 1024 * 1024;
        auto h = inventory({g}, true);
        h.ramBytes = 32ull * 1024 * 1024 * 1024;
        const auto json = hardwareObject(h);
        const auto adapter = json["gpus"].toArray().first().toObject();
        CTEST_CHECK(adapter["vram_bytes"].isNull());
        CTEST_COMPARE(adapter["unified_memory"].toBool(), true);
        CTEST_COMPARE(adapter["recommended_working_set_bytes"].toInteger(), qint64(*g.recommendedWorkingSetBytes));
        CTEST_COMPARE(json["ram_bytes"].toInteger(), qint64(*h.ramBytes));
        CTEST_CHECK(hardwareObject({})["ram_bytes"].isNull());
    }
    void gpuInitializationFallback(ErrorCode failure)
    {
        const auto h = inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda)});
        auto runtime = std::make_shared<RuntimeProbe>();
        runtime->supported = available(h);
        runtime->gpuFailure = failure;
        detail::RuntimeManager manager(2, h);
        manager.addRuntime(runtime);
        const auto loaded = manager.load({"model", "fixture", 128}, {});
        CTEST_COMPARE(loaded.execution.runtime, runtime->id());
        CTEST_COMPARE(loaded.execution.device.backend, ComputeBackend::Cpu);
        CTEST_COMPARE(loaded.execution.fallbackReasons.size(), 1);
        CTEST_COMPARE(runtime->attempts.size(), 2);
        CTEST_COMPARE(runtime->attempts.first().backend, ComputeBackend::Cuda);
        CTEST_COMPARE(manager.list().first().execution.device.backend, ComputeBackend::Cpu);
    }
    void cancellationAndInvalidModelDoNotRetryOnCpu(ErrorCode failure)
    {
        const auto h = inventory({gpu("apple", GpuVendor::Apple, ComputeBackend::Metal)}, true);
        auto runtime = std::make_shared<RuntimeProbe>();
        runtime->supported = available(h);
        runtime->gpuFailure = failure;
        detail::RuntimeManager manager(2, h);
        manager.addRuntime(runtime);
        CTEST_THROWS(Error, manager.load({"model", "fixture", 128}, {}));
        CTEST_COMPARE(runtime->attempts.size(), 1);
        CTEST_CHECK(manager.list().isEmpty());
    }
    void formatAndCapabilitiesDetermineRuntime()
    {
        auto irrelevant = std::make_shared<RuntimeProbe>();
        irrelevant->name = "a-other";
        irrelevant->path = "another-format";
        irrelevant->supported = {{ComputeBackend::Cpu, {}}};
        auto compatible = std::make_shared<RuntimeProbe>();
        compatible->supported = {{ComputeBackend::Cpu, {}}};
        detail::RuntimeManager manager(2, {});
        manager.addRuntime(irrelevant);
        manager.addRuntime(compatible);
        CTEST_COMPARE(manager.load({"model", "fixture", 128}, {}).execution.runtime, compatible->id());
        CTEST_CHECK(irrelevant->attempts.isEmpty());
    }
    void overrideAndFailedCpuNeverPublishModel()
    {
        auto runtime = std::make_shared<RuntimeProbe>();
        runtime->supported = {{ComputeBackend::Cpu, {}}};
        detail::RuntimeManager manager(2, {});
        manager.addRuntime(runtime);
        for (const auto& field : {"runtime", "backend", "device", "device_id", "gpu_layers", "n_gpu_layers"}) {
            ModelSpec model{"model", "fixture", 128, {{QString::fromLatin1(field), "override"}}};
            CTEST_THROWS(Error, manager.load(model, {}));
        }
        CTEST_CHECK(runtime->attempts.isEmpty());
        runtime->failCpu = true;
        CTEST_THROWS(Error, manager.load({"model", "fixture", 128}, {}));
        CTEST_CHECK(manager.list().isEmpty());
    }
    void nativeSnapshotIsStableAndQueryable()
    {
        Service service;
        const auto h = service.hardware();
        CTEST_CHECK(!h.cpuArchitecture.isEmpty());
        CTEST_CHECK(h.ramBytes && *h.ramBytes > 0);
        CTEST_COMPARE(hardwareObject(h), hardwareObject(detectHardware()));
        for (const auto& gpu : h.gpus)
            if (gpu.unifiedMemory.value_or(false)) CTEST_CHECK(!gpu.vramBytes);
        if (h.appleSilicon && h.metalAvailable) {
            CTEST_CHECK(std::any_of(h.gpus.begin(), h.gpus.end(), [](const auto& gpu) {
                return gpu.vendor == GpuVendor::Apple && gpu.unifiedMemory.value_or(false)
                    && !gpu.vramBytes && gpu.availableBackends.contains(ComputeBackend::Metal);
            }));
        }
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--offline")) {
        std::cerr << "Expected no argument or --offline" << std::endl;
        return 1;
    }
    const bool offline = argc == 2;
    iillm_ctest::CTestRunner runner;
    HardwareTests tests;
    if (!offline)
        runner.run("nativeRuntimeLimits", [&] { tests.nativeRuntimeLimits(); });
    struct PolicyRow { const char* name; HardwareInfo hardware; ComputeBackend expected; };
    const std::array rows{
        PolicyRow{"no-gpu", {}, ComputeBackend::Cpu},
        PolicyRow{"apple-silicon", inventory({gpu("apple", GpuVendor::Apple, ComputeBackend::Metal)}, true), ComputeBackend::Metal},
        PolicyRow{"nvidia-cuda", inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda)}), ComputeBackend::Cuda},
        PolicyRow{"amd-vulkan", inventory({gpu("amd", GpuVendor::Amd, ComputeBackend::Vulkan)}), ComputeBackend::Vulkan},
        PolicyRow{"intel-vulkan", inventory({gpu("intel", GpuVendor::Intel, ComputeBackend::Vulkan)}), ComputeBackend::Vulkan},
        PolicyRow{"intel-mac-metal-is-not-apple-silicon", inventory({gpu("intel", GpuVendor::Intel, ComputeBackend::Metal)}), ComputeBackend::Cpu},
        PolicyRow{"nvidia-no-cuda", inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Vulkan)}), ComputeBackend::Cpu},
        PolicyRow{"unknown-vendor", inventory({gpu("unknown", GpuVendor::Unknown, ComputeBackend::Vulkan)}), ComputeBackend::Cpu},
        PolicyRow{"cuda-before-vulkan", inventory({gpu("amd", GpuVendor::Amd, ComputeBackend::Vulkan, 32), gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda, 8)}), ComputeBackend::Cuda},
        PolicyRow{"metal-before-cuda", inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda, 32), gpu("apple", GpuVendor::Apple, ComputeBackend::Metal, 8)}, true), ComputeBackend::Metal}
    };
    for (const auto& row : rows)
        runner.run(std::string("policy:") + row.name, [&] { tests.policy(row.hardware, row.expected); });
    runner.run("unsupportedAndUnavailableAccelerationIsExcluded", [&] { tests.unsupportedAndUnavailableAccelerationIsExcluded(); });
    runner.run("neverCombinesCapabilitiesFromDifferentAdapters", [&] { tests.neverCombinesCapabilitiesFromDifferentAdapters(); });
    runner.run("sameBackendUsesMemoryThenStableId", [&] { tests.sameBackendUsesMemoryThenStableId(); });
    runner.run("unifiedMemoryIsNotReportedAsVram", [&] { tests.unifiedMemoryIsNotReportedAsVram(); });
    struct FailureRow { const char* name; ErrorCode failure; };
    for (const auto& row : {FailureRow{"allocation", ErrorCode::ResourceLimit}, FailureRow{"driver-disappeared", ErrorCode::RuntimeUnavailable}, FailureRow{"initialization", ErrorCode::RuntimeFailure}})
        runner.run(std::string("gpuInitializationFallback:") + row.name, [&] { tests.gpuInitializationFallback(row.failure); });
    for (const auto& row : {FailureRow{"cancelled", ErrorCode::Cancelled}, FailureRow{"invalid-model", ErrorCode::InvalidArgument}, FailureRow{"not-found", ErrorCode::NotFound}})
        runner.run(std::string("cancellationAndInvalidModelDoNotRetryOnCpu:") + row.name, [&] { tests.cancellationAndInvalidModelDoNotRetryOnCpu(row.failure); });
    runner.run("formatAndCapabilitiesDetermineRuntime", [&] { tests.formatAndCapabilitiesDetermineRuntime(); });
    runner.run("overrideAndFailedCpuNeverPublishModel", [&] { tests.overrideAndFailedCpuNeverPublishModel(); });
    if (!offline)
        runner.run("nativeSnapshotIsStableAndQueryable", [&] { tests.nativeSnapshotIsStableAndQueryable(); });
    return runner.result();
}

#ifdef _WIN32
#include <dxgi1_2.h>
#endif
