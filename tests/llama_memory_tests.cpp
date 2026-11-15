#include <iiLocalLLM.h>
#include <QtCore/QCoreApplication>
#include <llama.h>
#include <gguf.h>
#include <ggml-backend.h>
#include <iiFileProviderNativeDevice.h>
#include "llama-ext.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace iiLocalLLM;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void makeModel(const char* path, int width)
{
    // All weights are synthetic zeroes. This fixture exercises allocation and
    // format compatibility, not pretrained inference or model quality.
    std::unique_ptr<gguf_context, decltype(&gguf_free)> file(gguf_init_empty(), gguf_free);
    std::unique_ptr<ggml_context, decltype(&ggml_free)> tensors(
        ggml_init({8 * 1024 * 1024, nullptr, false}), ggml_free);
    require(file && tensors, "Cannot allocate synthetic GGUF fixture");
    gguf_set_val_str(file.get(), "general.architecture", "llama");
    gguf_set_val_str(file.get(), "general.name", "synthetic-memory-test");
    gguf_set_val_u32(file.get(), "llama.context_length", 4096);
    gguf_set_val_u32(file.get(), "llama.embedding_length", width);
    gguf_set_val_u32(file.get(), "llama.block_count", 2);
    gguf_set_val_u32(file.get(), "llama.feed_forward_length", width * 2);
    gguf_set_val_u32(file.get(), "llama.attention.head_count", 1);
    gguf_set_val_u32(file.get(), "llama.attention.head_count_kv", 1);
    gguf_set_val_u32(file.get(), "llama.rope.dimension_count", width);
    gguf_set_val_f32(file.get(), "llama.attention.layer_norm_rms_epsilon", 0.00001f);
    gguf_set_val_str(file.get(), "tokenizer.ggml.model", "llama");
    const char* tokens[] = {"<unk>", "<s>", "</s>", "a", "b", "c", "d", "e"};
    gguf_set_arr_str(file.get(), "tokenizer.ggml.tokens", tokens, 8);
    gguf_set_val_u32(file.get(), "tokenizer.ggml.bos_token_id", 1);
    gguf_set_val_u32(file.get(), "tokenizer.ggml.eos_token_id", 2);
    auto add = [&](const std::string& name, int x, int y) {
        auto* tensor = ggml_new_tensor_2d(tensors.get(), GGML_TYPE_F32, x, y);
        require(tensor && tensor->data, "Cannot allocate synthetic tensor");
        ggml_set_name(tensor, name.c_str());
        std::memset(tensor->data, 0, ggml_nbytes(tensor));
        gguf_add_tensor(file.get(), tensor);
    };
    add("token_embd.weight", width, 8);
    add("output_norm.weight", width, 1);
    for (int layer = 0; layer < 2; ++layer) {
        const auto prefix = "blk." + std::to_string(layer) + ".";
        add(prefix + "attn_norm.weight", width, 1);
        add(prefix + "attn_q.weight", width, width);
        add(prefix + "attn_k.weight", width, width);
        add(prefix + "attn_v.weight", width, width);
        add(prefix + "attn_output.weight", width, width);
        add(prefix + "ffn_norm.weight", width, 1);
        add(prefix + "ffn_gate.weight", width, width * 2);
        add(prefix + "ffn_up.weight", width, width * 2);
        add(prefix + "ffn_down.weight", width * 2, width);
    }
    iiFileProvider::native::Device output(false);
    output.open(path, {.write = true, .exclusive = true});
#ifdef _WIN32
    const auto duplicate = _dup(output.handle());
    auto* stream = duplicate < 0 ? nullptr : _fdopen(duplicate, "wb");
    if (!stream && duplicate >= 0) _close(duplicate);
#else
    const auto duplicate = dup(output.handle());
    auto* stream = duplicate < 0 ? nullptr : fdopen(duplicate, "wb");
    if (!stream && duplicate >= 0) close(duplicate);
#endif
    require(stream, "Cannot attach synthetic fixture stream");
    const bool wrote = gguf_write_to_file_ptr(file.get(), stream, false);
    const bool closed = std::fclose(stream) == 0;
    output.close();
    require(wrote && closed, "Cannot write synthetic GGUF fixture");
}

size_t nativeContextBytes(llama_model* model, ggml_type type, bool gpu)
{
    auto params = llama_context_default_params();
    params.n_ctx = 4096; params.n_batch = 32; params.n_ubatch = 32;
    params.type_k = params.type_v = type;
    params.offload_kqv = gpu; params.op_offload = gpu;
    if (type != GGML_TYPE_F16) params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    std::unique_ptr<llama_context, decltype(&llama_free)> ctx(llama_init_from_model(model, params), llama_free);
    require(bool(ctx), "Cannot create synthetic native context");
    size_t result = 0;
    for (const auto& [bufferType, bytes] : llama_get_memory_breakdown(ctx.get())) {
        const auto device = ggml_backend_buft_get_device(bufferType);
        if (!gpu || (device && ggml_backend_dev_type(device) == GGML_BACKEND_DEVICE_TYPE_GPU)) result += bytes.context;
    }
    return result;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        makeModel("synthetic.gguf", 64);
        makeModel("unaligned.gguf", 48);
        ModelSpec spec{"synthetic", "synthetic.gguf", 4096, {}, "gguf"};
        auto f16 = createLlamaRuntime({true, 0}, LlamaKvCache::F16);
        auto q8 = createLlamaRuntime({true, 0}, LlamaKvCache::Q8_0);
        auto q4 = createLlamaRuntime({true, 0}, LlamaKvCache::Q4_0);
        const auto f16Estimate = f16->estimateMemory(spec, 1);
        const auto q8Estimate = q8->estimateMemory(spec, 1);
        const auto q4Estimate = q4->estimateMemory(spec, 1);
        require(q4Estimate.contextBytes < q8Estimate.contextBytes && q8Estimate.contextBytes < f16Estimate.contextBytes,
                "Runtime reservation did not use the selected KV format");
        {
            // Exercise the service's registered runtime and its published
            // reservation, using a synthetic package in the test workspace.
            iiFileProvider::native::File::createDirectories("fixture");
            iiFileProvider::native::File::create("fixture/model.gguf", iiFileProvider::native::File::read("synthetic.gguf"));
            iiFileProvider::native::File::create("fixture/manifest.json",
                R"({"id":"synthetic","architecture":"llama","format":"gguf","quantization":"none","context_length":4096,"capabilities":["text-generation","chat"]})");
            ServiceOptions serviceOptions;
            serviceOptions.modelsDirectory = QStringLiteral("models");
            serviceOptions.maxCachedContexts = 1;
            serviceOptions.maxCachedContextTokens = 4096;
            serviceOptions.defaultContextTokens = 4096;
            LlamaMemoryOptions policy;
            policy.runtime.maxGpuLayers = 0;
            policy.kvCache = LlamaKvCache::Q8_0;
            Service service(serviceOptions, {}, policy);
            const auto installed = service.installModel(QStringLiteral("fixture")).get();
            require(installed.uri == QStringLiteral("model://synthetic"), "Synthetic package installation failed");
            const auto loaded = service.loadModel({QStringLiteral("model://synthetic"), 4096}).get();
            require(loaded.memory.contextBytes == q8Estimate.contextBytes,
                    "Service reservation did not use host Q8 cache policy");
            require(loaded.execution.device.backend == ComputeBackend::Cpu,
                    "Zero GPU layers did not select CPU in the service");
        }
        for (const auto& runtime : {f16, q8, q4}) {
            auto model = runtime->load(spec, {ComputeBackend::Cpu, {}}, {});
            auto context = model->createContext({});
            require(bool(context), "Runtime failed to create synthetic context");
        }
        auto unaligned = spec; unaligned.path = "unaligned.gguf";
        auto fpModel = f16->load(unaligned, {ComputeBackend::Cpu, {}}, {});
        require(bool(fpModel->createContext({})), "F16 unaligned model should be accepted");
        auto qModel = q8->load(unaligned, {ComputeBackend::Cpu, {}}, {});
        bool rejected = false;
        try { (void)qModel->createContext({}); } catch (const Error&) { rejected = true; }
        require(rejected, "Unsupported quantized head width did not reach the runtime context");

#ifdef __APPLE__
        // Exercise the iiLocalLLM adapter itself on Metal when it is present.
        auto gpuRuntime = createLlamaRuntime({true, 1}, LlamaKvCache::Q8_0);
        const auto hardware = detectHardware();
        for (const auto& device : gpuRuntime->devices(hardware)) {
            if (device.backend != ComputeBackend::Metal) continue;
            auto gpuModel = gpuRuntime->load(spec, device, {});
            auto context = gpuModel->createContext({});
            require(bool(context), "iiLocalLLM Metal Q8 context creation failed");
            std::cout << "iiLocalLLM Metal Q8 context created with capped GPU layers\n";
            break;
        }
#endif

        // Measure the actual backend KV allocation using the same pinned llama
        // engine. A missing GPU skips only the optional device measurement.
        llama_backend_init();
        ggml_backend_dev_t gpu = nullptr;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            auto* candidate = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(candidate) == GGML_BACKEND_DEVICE_TYPE_GPU) { gpu = candidate; break; }
        }
        ggml_backend_dev_t devices[] = {gpu, nullptr};
        auto params = llama_model_default_params();
        params.devices = devices; params.n_gpu_layers = gpu ? -1 : 0;
        std::unique_ptr<llama_model, decltype(&llama_model_free)> model(llama_model_load_from_file("synthetic.gguf", params), llama_model_free);
        require(bool(model), "Native synthetic model failed to load");
        const auto nativeF16 = nativeContextBytes(model.get(), GGML_TYPE_F16, gpu);
        const auto nativeQ8 = nativeContextBytes(model.get(), GGML_TYPE_Q8_0, gpu);
        const auto nativeQ4 = nativeContextBytes(model.get(), GGML_TYPE_Q4_0, gpu);
        require(nativeQ4 < nativeQ8 && nativeQ8 < nativeF16, "Native cache allocations did not shrink");
        std::cout << (gpu ? "GPU" : "CPU") << " KV context bytes: f16=" << nativeF16
                  << " q8_0=" << nativeQ8 << " q4_0=" << nativeQ4 << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
