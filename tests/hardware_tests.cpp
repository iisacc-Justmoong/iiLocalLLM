#include <iiLocalLLM.h>
#include "../service/Managers.h"
#include <QtTest/QtTest>
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
class HardwareTests : public QObject {
    Q_OBJECT
private slots:
    void policy_data()
    {
        QTest::addColumn<HardwareInfo>("hardware");
        QTest::addColumn<ComputeBackend>("expected");
        QTest::newRow("no-gpu") << HardwareInfo{} << ComputeBackend::Cpu;
        QTest::newRow("apple-silicon") << inventory({gpu("apple", GpuVendor::Apple, ComputeBackend::Metal)}, true) << ComputeBackend::Metal;
        QTest::newRow("nvidia-cuda") << inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda)}) << ComputeBackend::Cuda;
        QTest::newRow("amd-vulkan") << inventory({gpu("amd", GpuVendor::Amd, ComputeBackend::Vulkan)}) << ComputeBackend::Vulkan;
        QTest::newRow("intel-vulkan") << inventory({gpu("intel", GpuVendor::Intel, ComputeBackend::Vulkan)}) << ComputeBackend::Vulkan;
        QTest::newRow("intel-mac-metal-is-not-apple-silicon") << inventory({gpu("intel", GpuVendor::Intel, ComputeBackend::Metal)}) << ComputeBackend::Cpu;
        QTest::newRow("nvidia-no-cuda") << inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Vulkan)}) << ComputeBackend::Cpu;
        QTest::newRow("unknown-vendor") << inventory({gpu("unknown", GpuVendor::Unknown, ComputeBackend::Vulkan)}) << ComputeBackend::Cpu;
        QTest::newRow("cuda-before-vulkan") << inventory({gpu("amd", GpuVendor::Amd, ComputeBackend::Vulkan, 32),
            gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda, 8)}) << ComputeBackend::Cuda;
        QTest::newRow("metal-before-cuda") << inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda, 32),
            gpu("apple", GpuVendor::Apple, ComputeBackend::Metal, 8)}, true) << ComputeBackend::Metal;
    }
    void policy()
    {
        QFETCH(HardwareInfo, hardware);
        QFETCH(ComputeBackend, expected);
        const auto ordered = orderExecutionDevices(hardware, available(hardware));
        QVERIFY(!ordered.isEmpty());
        QCOMPARE(ordered.first().backend, expected);
        QCOMPARE(ordered.last().backend, ComputeBackend::Cpu);
    }
    void unsupportedAndUnavailableAccelerationIsExcluded()
    {
        auto h = inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda)});
        QCOMPARE(orderExecutionDevices(h, {{ComputeBackend::Cpu, {}}}).first().backend, ComputeBackend::Cpu);
        h.cudaAvailable = false;
        QCOMPARE(orderExecutionDevices(h, available(h)).first().backend, ComputeBackend::Cpu);
        h.cudaAvailable = true;
        h.gpus.first().availableBackends.clear();
        QCOMPARE(orderExecutionDevices(h, {{ComputeBackend::Cuda, "nvidia"}, {ComputeBackend::Cpu, {}}}).size(), 1);
        QVERIFY(orderExecutionDevices(h, {{ComputeBackend::Cuda, "missing"}}).isEmpty());
    }
    void neverCombinesCapabilitiesFromDifferentAdapters()
    {
        const auto h = inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Vulkan),
            gpu("amd", GpuVendor::Amd, ComputeBackend::Cuda)});
        QCOMPARE(orderExecutionDevices(h, available(h)).first().backend, ComputeBackend::Cpu);
    }
    void sameBackendUsesMemoryThenStableId()
    {
        const auto h = inventory({gpu("z", GpuVendor::Amd, ComputeBackend::Vulkan, 32),
            gpu("b", GpuVendor::Intel, ComputeBackend::Vulkan, 8), gpu("a", GpuVendor::Amd, ComputeBackend::Vulkan, 32)});
        auto candidates = available(h);
        candidates.append(candidates);
        const auto ordered = orderExecutionDevices(h, candidates);
        QCOMPARE(ordered.size(), 4);
        QCOMPARE(ordered[0].deviceId, QStringLiteral("a"));
        QCOMPARE(ordered[1].deviceId, QStringLiteral("z"));
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
        QVERIFY(adapter["vram_bytes"].isNull());
        QCOMPARE(adapter["unified_memory"].toBool(), true);
        QCOMPARE(adapter["recommended_working_set_bytes"].toInteger(), qint64(*g.recommendedWorkingSetBytes));
        QCOMPARE(json["ram_bytes"].toInteger(), qint64(*h.ramBytes));
        QVERIFY(hardwareObject({})["ram_bytes"].isNull());
    }
    void gpuInitializationFallback_data()
    {
        QTest::addColumn<ErrorCode>("failure");
        QTest::newRow("allocation") << ErrorCode::ResourceLimit;
        QTest::newRow("driver-disappeared") << ErrorCode::RuntimeUnavailable;
        QTest::newRow("initialization") << ErrorCode::RuntimeFailure;
    }
    void gpuInitializationFallback()
    {
        QFETCH(ErrorCode, failure);
        const auto h = inventory({gpu("nvidia", GpuVendor::Nvidia, ComputeBackend::Cuda)});
        auto runtime = std::make_shared<RuntimeProbe>();
        runtime->supported = available(h);
        runtime->gpuFailure = failure;
        detail::RuntimeManager manager(2, h);
        manager.addRuntime(runtime);
        const auto loaded = manager.load({"model", "fixture", 128}, {});
        QCOMPARE(loaded.execution.runtime, runtime->id());
        QCOMPARE(loaded.execution.device.backend, ComputeBackend::Cpu);
        QCOMPARE(loaded.execution.fallbackReasons.size(), 1);
        QCOMPARE(runtime->attempts.size(), 2);
        QCOMPARE(runtime->attempts.first().backend, ComputeBackend::Cuda);
        QCOMPARE(manager.list().first().execution.device.backend, ComputeBackend::Cpu);
    }
    void cancellationAndInvalidModelDoNotRetryOnCpu_data()
    {
        QTest::addColumn<ErrorCode>("failure");
        QTest::newRow("cancelled") << ErrorCode::Cancelled;
        QTest::newRow("invalid-model") << ErrorCode::InvalidArgument;
        QTest::newRow("not-found") << ErrorCode::NotFound;
    }
    void cancellationAndInvalidModelDoNotRetryOnCpu()
    {
        QFETCH(ErrorCode, failure);
        const auto h = inventory({gpu("apple", GpuVendor::Apple, ComputeBackend::Metal)}, true);
        auto runtime = std::make_shared<RuntimeProbe>();
        runtime->supported = available(h);
        runtime->gpuFailure = failure;
        detail::RuntimeManager manager(2, h);
        manager.addRuntime(runtime);
        QVERIFY_THROWS_EXCEPTION(Error, manager.load({"model", "fixture", 128}, {}));
        QCOMPARE(runtime->attempts.size(), 1);
        QVERIFY(manager.list().isEmpty());
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
        QCOMPARE(manager.load({"model", "fixture", 128}, {}).execution.runtime, compatible->id());
        QVERIFY(irrelevant->attempts.isEmpty());
    }
    void overrideAndFailedCpuNeverPublishModel()
    {
        auto runtime = std::make_shared<RuntimeProbe>();
        runtime->supported = {{ComputeBackend::Cpu, {}}};
        detail::RuntimeManager manager(2, {});
        manager.addRuntime(runtime);
        for (const auto& field : {"runtime", "backend", "device", "device_id", "gpu_layers", "n_gpu_layers"}) {
            ModelSpec model{"model", "fixture", 128, {{QString::fromLatin1(field), "override"}}};
            QVERIFY_THROWS_EXCEPTION(Error, manager.load(model, {}));
        }
        QVERIFY(runtime->attempts.isEmpty());
        runtime->failCpu = true;
        QVERIFY_THROWS_EXCEPTION(Error, manager.load({"model", "fixture", 128}, {}));
        QVERIFY(manager.list().isEmpty());
    }
    void nativeSnapshotIsStableAndQueryable()
    {
        Service service;
        const auto h = service.hardware();
        QVERIFY(!h.cpuArchitecture.isEmpty());
        QVERIFY(h.ramBytes && *h.ramBytes > 0);
        QCOMPARE(hardwareObject(h), hardwareObject(detectHardware()));
        for (const auto& gpu : h.gpus)
            if (gpu.unifiedMemory.value_or(false)) QVERIFY(!gpu.vramBytes);
        if (h.appleSilicon && h.metalAvailable) {
            QVERIFY(std::any_of(h.gpus.begin(), h.gpus.end(), [](const auto& gpu) {
                return gpu.vendor == GpuVendor::Apple && gpu.unifiedMemory.value_or(false)
                    && !gpu.vramBytes && gpu.availableBackends.contains(ComputeBackend::Metal);
            }));
        }
    }
};
QTEST_GUILESS_MAIN(HardwareTests)
#include "hardware_tests.moc"
