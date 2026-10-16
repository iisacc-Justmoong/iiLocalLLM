#include "Runtime.h"
#include "Parameters.h"
#include "Utf8Stream.h"
#include "MemoryEstimate.h"
#include "MediaContent.h"
#include "hardware/Detection.h"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStringConverter>
#include <QtCore/QJsonDocument>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>
#include <limits>
#include <QtCore/QUuid>

#ifdef IILOCALLLM_WITH_LLAMA
#include <llama.h>
#include <gguf.h>
#include <chat.h>
#include <sampling.h>
#include <mtmd.h>
#include <mtmd-helper.h>
#include <stb/stb_image.h>
#endif

namespace iiLocalLLM {
namespace {
#ifdef IILOCALLLM_WITH_LLAMA
using ModelPtr = std::shared_ptr<llama_model>;
using ContextPtr = std::unique_ptr<llama_context, decltype(&llama_free)>;
using SamplerPtr = std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)>;
using BitmapPtr = std::unique_ptr<mtmd_bitmap, decltype(&mtmd_bitmap_free)>;
using ChunksPtr = std::unique_ptr<mtmd_input_chunks, decltype(&mtmd_input_chunks_free)>;
struct NativeProjector {
    std::unique_ptr<mtmd_context, decltype(&mtmd_free)> context{nullptr, mtmd_free};
    std::mutex mutex; // Encoding and embedding decode share projector scratch state.
};
bool abortDecode(void* data) { return static_cast<const CancellationToken*>(data)->isCancelled(); }
void initializeLlama()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        // Upstream grammar debug messages contain generated tokens. Keep them out of default logs.
        llama_log_set([](ggml_log_level level, const char* text, void*) {
            if (level != GGML_LOG_LEVEL_DEBUG) std::fputs(text, stderr);
        }, nullptr);
        llama_backend_init();
    });
}
QString deviceId(ggml_backend_dev_t device)
{
    return QStringLiteral("llama.cpp/") + QString::fromUtf8(ggml_backend_reg_name(ggml_backend_dev_backend_reg(device)))
        + QLatin1Char('/') + QString::fromUtf8(ggml_backend_dev_name(device));
}
std::optional<ComputeBackend> deviceBackend(ggml_backend_dev_t device)
{
    const auto name = QString::fromUtf8(ggml_backend_reg_name(ggml_backend_dev_backend_reg(device)));
    if (name == QStringLiteral("MTL") || name.compare(QStringLiteral("Metal"), Qt::CaseInsensitive) == 0) return ComputeBackend::Metal;
    if (name.compare(QStringLiteral("CUDA"), Qt::CaseInsensitive) == 0) return ComputeBackend::Cuda;
    if (name.compare(QStringLiteral("Vulkan"), Qt::CaseInsensitive) == 0) return ComputeBackend::Vulkan;
    return {};
}

int optionInt(const ModelSpec& spec, const char* key, int fallback, int low, int high)
{
    const auto value = spec.options.value(QString::fromLatin1(key));
    if (value.isUndefined()) return fallback;
    const int n = value.toInt(fallback);
    if (!value.isDouble() || value.toDouble() != n || n < low || n > high)
        throw Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid llama option: ") + QString::fromLatin1(key));
    return n;
}
struct LlamaConversationState final : RuntimePromptState {
    common_chat_params chat;
    common_chat_parser_params parser;
    bool thinkingEnabled = true;
    std::shared_ptr<NativeProjector> projector;
    std::vector<BitmapPtr> bitmaps;
    ChunksPtr chunks{nullptr, mtmd_input_chunks_free};
    qint64 inputTokens = 0;
};
class LlamaContext final : public RuntimeContext {
public:
    LlamaContext(ModelPtr model, const ModelSpec& spec, bool accelerated) : model_(std::move(model)), context_(nullptr, llama_free)
    {
        auto params = llama_context_default_params();
        params.n_ctx = spec.contextTokens;
        params.n_batch = std::min(spec.contextTokens, 512);
        params.n_ubatch = std::min(spec.contextTokens, 128);
        params.n_seq_max = 1;
        params.n_threads = optionInt(spec, "threads", 4, 1, 512);
        params.n_threads_batch = params.n_threads;
        params.offload_kqv = accelerated;
        params.op_offload = accelerated;
        context_.reset(llama_init_from_model(model_.get(), params));
        if (!context_) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("llama.cpp context creation failed"));
    }
    RuntimeResult generate(const TokenList& prompt, const GenerationOptions& options,
                           const CancellationToken& cancel, const TextCallback& onText) override
    { return generateImpl(prompt, options, cancel, onText, nullptr); }
    RuntimeResult generateConversation(const RuntimeConversationPrompt& prompt, const GenerationOptions& options,
        const CancellationToken& cancel, const TextCallback& onText) override
    {
        const auto* state = dynamic_cast<const LlamaConversationState*>(prompt.state.get());
        if (!state) throw Error(ErrorCode::InvalidArgument, "Missing llama.cpp conversation template state");
        return generateImpl(prompt.tokens, options, cancel, onText, state);
    }
private:
    RuntimeResult generateImpl(const TokenList& prompt, const GenerationOptions& options,
        const CancellationToken& cancel, const TextCallback& onText, const LlamaConversationState* conversation)
    {
        cancel.throwIfCancelled();
        validateGenerationOptions(options, "llama.cpp");
        const auto* vocab = llama_model_get_vocab(model_.get());
        const auto vocabSize = llama_vocab_n_tokens(vocab);
        if (options.minKeep > vocabSize) throw Error(ErrorCode::InvalidArgument, "min_keep exceeds vocabulary size");
        std::vector<llama_logit_bias> biases;
        for (auto it = options.logitBias.begin(); it != options.logitBias.end(); ++it) {
            const auto token = it.key().toInt();
            if (token >= vocabSize) throw Error(ErrorCode::InvalidArgument, "logit_bias token is outside the vocabulary");
            biases.push_back({token, float(it.value().toDouble())});
        }
        const auto inputTokens=conversation&&conversation->chunks?conversation->inputTokens:prompt.size();
        if (inputTokens <= 0 || inputTokens + options.maxTokens > llama_n_ctx(context_.get()))
            throw Error(ErrorCode::ContextOverflow, QStringLiteral("llama.cpp context overflow"));
        llama_set_abort_callback(context_.get(), abortDecode, const_cast<CancellationToken*>(&cancel));
        struct ResetAbort { llama_context* ctx; ~ResetAbort() { llama_set_abort_callback(ctx, nullptr, nullptr); } } reset{context_.get()};
        int reuse = 0;
        // Always decode at least the last prompt token to produce current logits.
        const bool media=conversation&&conversation->chunks;
        if(!media)while (reuse < std::min(previous_.size(), prompt.size() - 1) && previous_[reuse] == prompt[reuse]) ++reuse;
        const auto memory = llama_get_memory(context_.get());
        if (!llama_memory_seq_rm(memory, 0, reuse, -1)) {
            llama_memory_clear(memory, true);
            reuse = 0; // Recurrent models may not permit partial cache removal.
        }
        previous_.resize(reuse);
        nextPosition_=reuse;
        if(media) {
            std::lock_guard lock(conversation->projector->mutex);
            const auto ctx=conversation->projector->context.get();
            for(size_t i=0;i<mtmd_input_chunks_size(conversation->chunks.get());++i) {
                cancel.throwIfCancelled();
                const auto status=mtmd_helper_eval_chunk_single(ctx,context_.get(),mtmd_input_chunks_get(conversation->chunks.get(),i),
                    nextPosition_,0,llama_n_batch(context_.get()),i+1==mtmd_input_chunks_size(conversation->chunks.get()),&nextPosition_);
                cancel.throwIfCancelled();
                if(status)throw Error(ErrorCode::RuntimeFailure,QString("Native media evaluation failed: %1").arg(status));
            }
        } else decode(prompt.sliced(reuse), cancel);
        SamplerPtr sampler(nullptr, llama_sampler_free);
        common_sampler_ptr structuredSampler;
        std::set<llama_token> preserved;
        if (conversation) {
            common_params_sampling params;
            params.seed = options.seed; params.min_keep = options.minKeep;
            params.top_k = options.topK; params.top_p = options.topP; params.min_p = options.minP;
            params.typ_p = options.typicalP; params.temp = options.temperature;
            params.xtc_probability = options.xtcProbability; params.xtc_threshold = options.xtcThreshold;
            params.penalty_repeat = options.repetitionPenalty;
            params.penalty_freq = options.frequencyPenalty; params.penalty_present = options.presencePenalty;
            params.penalty_last_n = options.repetitionContextSize < 0 ? llama_n_ctx(context_.get()) : options.repetitionContextSize;
            params.logit_bias = biases;
            params.samplers = {COMMON_SAMPLER_TYPE_PENALTIES, COMMON_SAMPLER_TYPE_TOP_K,
                COMMON_SAMPLER_TYPE_TYPICAL_P, COMMON_SAMPLER_TYPE_TOP_P, COMMON_SAMPLER_TYPE_MIN_P,
                COMMON_SAMPLER_TYPE_XTC, COMMON_SAMPLER_TYPE_TEMPERATURE};
            const auto& chat = conversation->chat;
            if (!chat.grammar.empty()) params.grammar = {COMMON_GRAMMAR_TYPE_TOOL_CALLS, chat.grammar};
            params.grammar_lazy = chat.grammar_lazy;
            params.generation_prompt = chat.generation_prompt;
            for (const auto& text : chat.preserved_tokens) {
                const auto tokens = common_tokenize(vocab, text, false, true);
                if (tokens.size() == 1) preserved.insert(tokens.front());
            }
            params.preserved_tokens = preserved;
            for (auto trigger : chat.grammar_triggers) {
                if (trigger.type == COMMON_GRAMMAR_TRIGGER_TYPE_WORD) {
                    const auto tokens = common_tokenize(vocab, trigger.value, false, true);
                    if (tokens.size() == 1) {
                        if (!preserved.contains(tokens.front())) throw Error(ErrorCode::RuntimeFailure, "Unpreserved grammar trigger token");
                        trigger.type = COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN; trigger.token = tokens.front();
                    }
                }
                params.grammar_triggers.push_back(std::move(trigger));
            }
            structuredSampler.reset(common_sampler_init(model_.get(), params));
            if (!structuredSampler) throw Error(ErrorCode::RuntimeFailure, "Cannot initialize structured sampler");
            // Prompt history feeds penalties only; grammar receives generated tokens and its upstream prefill.
            for (const auto token : prompt) common_sampler_accept(structuredSampler.get(), token, false);
        } else {
        auto params = llama_sampler_chain_default_params();
        sampler.reset(llama_sampler_chain_init(params));
        if (!biases.empty()) llama_sampler_chain_add(sampler.get(), llama_sampler_init_logit_bias(vocabSize, biases.size(), biases.data()));
        if (options.repetitionContextSize != 0 && (options.repetitionPenalty != 1 || options.presencePenalty != 0 || options.frequencyPenalty != 0))
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_penalties(vocabSize,
                options.repetitionContextSize < 0 ? llama_n_ctx(context_.get()) : options.repetitionContextSize,
                options.repetitionPenalty, options.frequencyPenalty, options.presencePenalty));
        if (options.temperature == 0) llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());
        else {
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_k(options.topK));
            if (options.typicalP != 1) llama_sampler_chain_add(sampler.get(), llama_sampler_init_typical(options.typicalP, options.minKeep));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(options.topP, options.minKeep));
            if (options.minP > 0) llama_sampler_chain_add(sampler.get(), llama_sampler_init_min_p(options.minP, options.minKeep));
            if (options.xtcProbability > 0) llama_sampler_chain_add(sampler.get(), llama_sampler_init_xtc(options.xtcProbability, options.xtcThreshold, options.minKeep, options.seed));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(options.temperature));
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(options.seed));
        }
        // New sampler per request: restore the full history, including cached prompt tokens.
        for (const auto token : prompt) llama_sampler_accept(sampler.get(), token);
        }
        RuntimeResult result{FinishReason::Length, 0, reuse};
        detail::Utf8Stream utf8;
        for (int n = 0; n < options.maxTokens; ++n) {
            cancel.throwIfCancelled();
            const auto token = conversation ? common_sampler_sample(structuredSampler.get(), context_.get(), -1)
                : llama_sampler_sample(sampler.get(), context_.get(), -1);
            if (conversation) common_sampler_accept(structuredSampler.get(), token, true);
            ++result.generatedTokens;
            if (llama_vocab_is_eog(vocab, token)) { result.finishReason = FinishReason::Stop; break; }
            QByteArray piece(32, '\0');
            const bool special = preserved.contains(token);
            int length = llama_token_to_piece(vocab, token, piece.data(), piece.size(), 0, special);
            if (length < 0) {
                piece.resize(-length);
                length = llama_token_to_piece(vocab, token, piece.data(), piece.size(), 0, special);
            }
            if (length < 0) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Token decoding failed"));
            piece.resize(length);
            const QString text = utf8(piece);
            if (!text.isEmpty() && !onText(text)) { result.finishReason = FinishReason::Stop; break; }
            // The last sampled token is not evaluated until needed by a subsequent request.
            if (n + 1 < options.maxTokens) decode({token}, cancel, !media);
        }
        const auto tail = utf8.finish();
        if (!tail.isEmpty()) onText(tail);
        if(media)previous_.clear(); // Text token prefixes cannot describe media embeddings.
        return result;
    }
private:
    ModelPtr model_;
    ContextPtr context_;
    TokenList previous_;
    llama_pos nextPosition_=0;
    void decode(const TokenList& tokens, const CancellationToken& cancel, bool trackHistory=true)
    {
        for (qsizetype start = 0; start < tokens.size();) {
            cancel.throwIfCancelled();
            const int count = std::min<qsizetype>(llama_n_batch(context_.get()), tokens.size() - start);
            auto batch = llama_batch_get_one(const_cast<llama_token*>(tokens.constData() + start), count);
            std::vector<llama_pos> positions(count);
            for(int i=0;i<count;++i)positions[i]=nextPosition_+i;
            batch.pos=positions.data();
            const int status = llama_decode(context_.get(), batch);
            cancel.throwIfCancelled();
            if (status != 0) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("llama_decode failed: %1").arg(status));
            if(trackHistory)previous_.append(tokens.sliced(start, count));
            nextPosition_+=count;
            start += count;
        }
    }
};
class LlamaModel final : public RuntimeModel {
public:
    LlamaModel(ModelPtr model, ModelSpec spec, bool accelerated, ggml_backend_dev_t device)
        : model_(std::move(model)), spec_(std::move(spec)), accelerated_(accelerated) {
        if(!spec_.projectorPath.isEmpty()) {
            projector_=std::make_shared<NativeProjector>();auto params=mtmd_context_params_default();
            params.use_gpu=accelerated_;params.device=device;params.n_threads=optionInt(spec_,"threads",4,1,512);params.warmup=false;
            projector_->context.reset(mtmd_init_from_file(spec_.projectorPath.toUtf8().constData(),model_.get(),params));
            if(!projector_->context)throw Error(ErrorCode::RuntimeFailure,"Cannot load native multimodal projector");
        }
    }
    TokenList tokenize(const QList<ChatMessage>& messages, const CancellationToken& cancel) override
    {
        cancel.throwIfCancelled();
        if (spec_.options.contains("enable_thinking")) {
            ConversationRequest request;
            for (const auto& message : messages)
                request.messages.append(QJsonObject{{"role", enumName(message.role)}, {"content", message.content}});
            return prepareConversation(request, cancel).tokens;
        }
        const auto custom = spec_.options.value(QStringLiteral("chat_template"));
        if (!custom.isUndefined() && !custom.isString())
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("chat_template must be a string"));
        const auto override = custom.toString().toUtf8();
        const char* chatTemplate = override.isEmpty() ? llama_model_chat_template(model_.get(), nullptr) : override.constData();
        if (!chatTemplate) throw Error(ErrorCode::InvalidArgument, QStringLiteral("Model has no chat template; set options.chat_template explicitly"));
        std::vector<QByteArray> contents, roles;
        std::vector<llama_chat_message> chat;
        contents.reserve(messages.size()); roles.reserve(messages.size()); chat.reserve(messages.size());
        for (const auto& message : messages) {
            roles.push_back(enumName(message.role).toUtf8());
            contents.push_back(message.content.toUtf8());
            chat.push_back({roles.back().constData(), contents.back().constData()});
        }
        int size = llama_chat_apply_template(chatTemplate, chat.data(), chat.size(), true, nullptr, 0);
        if (size <= 0) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Unsupported llama.cpp chat template"));
        QByteArray formatted(size, '\0');
        size = llama_chat_apply_template(chatTemplate, chat.data(), chat.size(), true, formatted.data(), formatted.size());
        if (size <= 0 || size > formatted.size()) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Chat formatting failed"));
        const auto* vocab = llama_model_get_vocab(model_.get());
        int count = llama_tokenize(vocab, formatted.constData(), size, nullptr, 0, true, true);
        if (count >= 0) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Empty tokenized prompt"));
        TokenList tokens(-count, 0);
        count = llama_tokenize(vocab, formatted.constData(), size, tokens.data(), tokens.size(), true, true);
        if (count < 1) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("Tokenization failed"));
        tokens.resize(count);
        return tokens;
    }
    std::unique_ptr<RuntimeContext> createContext(const CancellationToken& cancel) override
    { cancel.throwIfCancelled(); return std::make_unique<LlamaContext>(model_, spec_, accelerated_); }
    RuntimeConversationPrompt prepareConversation(const ConversationRequest& request, const CancellationToken& cancel) override
    {
        cancel.throwIfCancelled();
        if (!templates_) templates_ = common_chat_templates_init(model_.get(), spec_.options["chat_template"].toString().toStdString());
        common_chat_templates_inputs input;
        auto state = std::make_shared<LlamaConversationState>();state->projector=projector_;
        QJsonArray messages;std::vector<std::string> markers;
        constexpr std::uint64_t mediaLimit=128*1024*1024;
        std::uint64_t decodedMediaBytes=0;
        const auto nonce=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        auto addBitmap=[&](const QJsonObject& block,bool video,QJsonArray& parts) {
            cancel.throwIfCancelled();
            if(!projector_)throw Error(ErrorCode::RuntimeUnavailable,"Native media requires a package-owned multimodal projector");
            const bool audio=block["type"]=="audio";const auto ctx=projector_->context.get();
            if((audio&&!mtmd_support_audio(ctx))||(!audio&&!mtmd_support_vision(ctx)))
                throw Error(ErrorCode::RuntimeUnavailable,"Loaded projector does not support the requested modality");
            const auto bytes=mediaBytes(block);
            if(!audio) {
                int width=0,height=0,channels=0;
                if(!stbi_info_from_memory(reinterpret_cast<const unsigned char*>(bytes.constData()),bytes.size(),&width,&height,&channels)
                    ||width<=0||height<=0)throw Error(ErrorCode::InvalidArgument,"Invalid native image header");
                if(std::uint64_t(width)*std::uint64_t(height)>16*1024*1024)
                    throw Error(ErrorCode::ResourceLimit,"Native image exceeds 16 megapixels");
                if(std::uint64_t(width)*std::uint64_t(height)*3>mediaLimit-decodedMediaBytes)
                    throw Error(ErrorCode::ResourceLimit,"Decoded request media exceeds 128 MiB");
            }
            auto decoded=mtmd_helper_bitmap_init_from_buf(ctx,reinterpret_cast<const unsigned char*>(bytes.constData()),bytes.size(),false,mtmd_helper_init_opt_default());
            BitmapPtr bitmap(decoded.bitmap,mtmd_bitmap_free);
            if(!bitmap)throw Error(ErrorCode::InvalidArgument,"Cannot decode native media bytes");
            if(mtmd_bitmap_is_audio(bitmap.get())!=audio)throw Error(ErrorCode::InvalidArgument,"Media bytes do not match their declared modality");
            const auto decodedBytes=mtmd_bitmap_get_n_bytes(bitmap.get());
            if(decodedBytes>mediaLimit-decodedMediaBytes)throw Error(ErrorCode::ResourceLimit,"Decoded request media exceeds 128 MiB");
            decodedMediaBytes+=decodedBytes;
            mtmd_bitmap_set_mergeable(bitmap.get(),video);
            markers.push_back("<__iillm_"+nonce+"_"+std::to_string(markers.size())+"__>");
            parts.append(QJsonObject{{"type","media_marker"},{"text",QString::fromStdString(markers.back())}});
            state->bitmaps.push_back(std::move(bitmap));
        };
        for(const auto& value:request.messages) {
            auto message=value.toObject();
            if(message["content"].isArray()) {
                QJsonArray parts;
                for(const auto& item:nativeContent(message["content"].toArray())) {
                    const auto block=item.toObject();const auto type=block["type"].toString();
                    if(type=="text")parts.append(block);
                    else if(type=="video") {
                        for(const auto& frame:block["frames"].toArray()) {
                            const auto image=frame.toObject();parts.append(QJsonObject{{"type","text"},{"text",QString("[video frame at %1 ms]").arg(image["timestamp_ms"].toDouble(),0,'g',16)}});
                            addBitmap(image,true,parts);
                        }
                    } else addBitmap(block,false,parts);
                }
                message["content"]=parts;
            }
            messages.append(message);
        }
        input.messages = common_chat_msgs_parse_oaicompat(common_json::parse(QJsonDocument(messages).toJson(QJsonDocument::Compact).toStdString()));
        input.tools = common_chat_tools_parse_oaicompat(common_json::parse(QJsonDocument(request.tools).toJson(QJsonDocument::Compact).toStdString()));
        input.tool_choice = common_chat_tool_choice_parse_oaicompat(request.toolChoice.toStdString());
        input.parallel_tool_calls = request.parallelToolCalls;
        input.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        input.enable_thinking = request.enableThinking.value_or(spec_.options.value("enable_thinking").toBool(true));
        if(!request.responseSchema.isEmpty())input.json_schema=QJsonDocument(request.responseSchema).toJson(QJsonDocument::Compact).toStdString();
        state->thinkingEnabled = input.enable_thinking;
        state->chat = common_chat_templates_apply(templates_.get(), input);
        if (!spec_.options.value("tool_grammar").toBool(true)&&request.responseSchema.isEmpty()) {
            // Upstream object grammars constrain optional property order. Hosts
            // can use natural tool output while keeping parsing and executor validation.
            state->chat.grammar.clear();
            state->chat.grammar_lazy = false;
            state->chat.grammar_triggers.clear();
        }
        state->parser = common_chat_parser_params(state->chat);
        state->parser.reasoning_format = input.reasoning_format;
        if (!state->chat.parser.empty()) state->parser.parser.load(state->chat.parser);
        std::vector<llama_token> tokens;
        if(!state->bitmaps.empty()) {
            // Split only host-generated markers. User text cannot impersonate a bitmap slot.
            std::vector<std::string> text;std::vector<mtmd_input_text> spans;std::vector<mtmd_input_part> parts;
            text.reserve(markers.size()+1);spans.reserve(markers.size()+1);parts.reserve(2*markers.size()+1);
            size_t at=0;
            auto addText=[&](std::string value){text.push_back(std::move(value));spans.push_back({text.back().data(),text.back().size(),false,true});parts.push_back({&spans.back(),nullptr});};
            for(size_t i=0;i<markers.size();++i) {
                const auto pos=state->chat.prompt.find(markers[i],at);
                if(pos==std::string::npos||state->chat.prompt.find(markers[i],pos+markers[i].size())!=std::string::npos)
                    throw Error(ErrorCode::RuntimeUnavailable,"Chat template did not preserve native media ordering");
                if(pos>at)addText(state->chat.prompt.substr(at,pos-at));
                parts.push_back({nullptr,state->bitmaps[i].get()});at=pos+markers[i].size();
            }
            if(at<state->chat.prompt.size())addText(state->chat.prompt.substr(at));
            std::vector<const mtmd_input_part*> pointers;for(const auto& part:parts)pointers.push_back(&part);
            state->chunks.reset(mtmd_input_chunks_init());
            if(mtmd_tokenize_from_parts(projector_->context.get(),state->chunks.get(),pointers.data(),pointers.size(),true))
                throw Error(ErrorCode::RuntimeFailure,"Native multimodal tokenization failed");
            const auto count=mtmd_helper_get_n_tokens(state->chunks.get());
            state->inputTokens=std::max<qint64>(count,mtmd_helper_get_n_pos(state->chunks.get()));
            if(state->inputTokens>std::numeric_limits<int>::max())throw Error(ErrorCode::ResourceLimit,"Native media token budget overflow");
            for(size_t i=0;i<mtmd_input_chunks_size(state->chunks.get());++i) {
                const auto chunk=mtmd_input_chunks_get(state->chunks.get(),i);
                if(mtmd_input_chunk_get_type(chunk)==MTMD_INPUT_CHUNK_TYPE_TEXT) {
                    size_t n=0;const auto data=mtmd_input_chunk_get_tokens_text(chunk,&n);tokens.insert(tokens.end(),data,data+n);
                }
            }
        } else tokens=common_tokenize(llama_model_get_vocab(model_.get()),state->chat.prompt,true,true);
        RuntimeConversationPrompt result;
        result.tokens.reserve(tokens.size());
        for (const auto token : tokens) result.tokens.append(token);
        for (const auto& stop : state->chat.additional_stops) result.stop.append(QString::fromStdString(stop));
        result.state = std::move(state);
        result.inputTokens=static_cast<const LlamaConversationState*>(result.state.get())->inputTokens;
        cancel.throwIfCancelled(); return result;
    }
    RuntimeConversationReply parseConversation(const RuntimeConversationPrompt& prompt, const QString& text) override
    {
        const auto* state = dynamic_cast<const LlamaConversationState*>(prompt.state.get());
        if (!state) throw Error(ErrorCode::InvalidArgument, "Missing llama.cpp conversation parser state");
        const auto parsed = [&] {
            const auto prefix = QString::fromStdString(state->parser.generation_prompt);
            if (!state->thinkingEnabled && state->chat.supports_thinking && state->chat.thinking_start_tag == "<think>"
                && prefix.contains("<think>") && prefix.trimmed().endsWith("</think>")
                && text.trimmed().startsWith("</think>")) {
                // Some Qwen-style models repeat the closing delimiter already
                // supplied by the disabled-thinking prefill. Let the upstream
                // parser consume that generated delimiter as the same boundary.
                // The raw answer and tags elsewhere in its content are untouched.
                auto parser = state->parser;
                parser.generation_prompt.resize(parser.generation_prompt.rfind("</think>"));
                return common_chat_parse(text.toStdString(), false, parser);
            }
            return common_chat_parse(text.toStdString(), false, state->parser);
        }();
        RuntimeConversationReply result{QString::fromStdString(parsed.content), QString::fromStdString(parsed.reasoning_content), {}};
        for (const auto& call : parsed.tool_calls)
            result.toolCalls.append(QJsonObject{{"id", QString::fromStdString(call.id)}, {"type", "function"},
                {"function", QJsonObject{{"name", QString::fromStdString(call.name)}, {"arguments", QString::fromStdString(call.arguments)}}}});
        return result;
    }
private:
    ModelPtr model_;
    ModelSpec spec_;
    bool accelerated_;
    common_chat_templates_ptr templates_;
    std::shared_ptr<NativeProjector> projector_;
};
#endif
class LlamaRuntime final : public Runtime {
public:
    QString id() const override { return QStringLiteral("llama.cpp"); }
    bool supportsModel(const ModelSpec& spec) const override
    {
        if (!spec.format.isEmpty() && spec.format != QStringLiteral("gguf")) return false;
        QFile file(spec.path);
        return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArrayLiteral("GGUF");
    }
    QList<RuntimeDevice> devices(const HardwareInfo& hardware) const override
    {
        QList<RuntimeDevice> result;
#ifdef IILOCALLLM_WITH_LLAMA
        result.append({ComputeBackend::Cpu, {}});
        for (const auto& gpu : hardware.gpus) {
            if (!gpu.id.startsWith(QStringLiteral("llama.cpp/"))) continue;
            for (const auto backend : gpu.availableBackends) result.append({backend, gpu.id});
        }
#else
        Q_UNUSED(hardware);
#endif
        return result;
    }
    MemoryEstimate estimateMemory(const ModelSpec& spec, int contextSlots) const override
    {
        auto estimate = Runtime::estimateMemory(spec, contextSlots);
        if(!spec.projectorPath.isEmpty()) {
            const QFileInfo projector(spec.projectorPath);
            if(!projector.isFile())throw Error(ErrorCode::InvalidManifest,"Multimodal projector is missing");
            estimate.weightsBytes+=projector.size();
            estimate.overheadBytes+=128*1024*1024; // Bounded decoded media plus projector scratch reservation.
        }
#ifdef IILOCALLLM_WITH_LLAMA
        std::unique_ptr<gguf_context, decltype(&gguf_free)> metadata(
            gguf_init_from_file(spec.path.toUtf8().constData(), {true, nullptr}), gguf_free);
        if (!metadata) throw Error(ErrorCode::InvalidManifest, QStringLiteral("Cannot read GGUF memory metadata"));
        const auto architectureKey = gguf_find_key(metadata.get(), "general.architecture");
        if (architectureKey < 0 || gguf_get_kv_type(metadata.get(), architectureKey) != GGUF_TYPE_STRING) return estimate;
        const auto architecture = QByteArray(gguf_get_val_str(metadata.get(), architectureKey));
        auto integer = [&](const char* suffix, quint64 fallback = 0) -> quint64 {
            const auto key = gguf_find_key(metadata.get(), (architecture + suffix).constData());
            if (key < 0) return fallback;
            if (gguf_get_kv_type(metadata.get(), key) == GGUF_TYPE_UINT32) return gguf_get_val_u32(metadata.get(), key);
            if (gguf_get_kv_type(metadata.get(), key) == GGUF_TYPE_UINT64) return gguf_get_val_u64(metadata.get(), key);
            return fallback;
        };
        const auto heads = integer(".attention.head_count");
        const auto dim = heads ? integer(".embedding_length") / heads : 0;
        const auto context = detail::kvReservation(integer(".block_count"), integer(".attention.head_count_kv", heads),
            integer(".attention.key_length", dim), integer(".attention.value_length", dim), spec.contextTokens, contextSlots);
        if (context) { estimate.contextBytes = context; estimate.basis = QStringLiteral("GGUF weight file + f16 KV metadata + scratch reserve"); }
#endif
        return estimate;
    }
    std::shared_ptr<RuntimeModel> load(const ModelSpec& spec, const RuntimeDevice& device, const CancellationToken& cancel) override
    {
#ifdef IILOCALLLM_WITH_LLAMA
        if (!QFileInfo(spec.path).isFile()) throw Error(ErrorCode::NotFound, QStringLiteral("Local GGUF file not found"));
        for (auto it = spec.options.begin(); it != spec.options.end(); ++it) {
            if (it.key() != QStringLiteral("threads") && it.key() != QStringLiteral("chat_template")
                && it.key() != QStringLiteral("tool_grammar") && it.key() != QStringLiteral("enable_thinking"))
                throw Error(ErrorCode::InvalidArgument, QStringLiteral("Unsupported model option: ") + it.key());
        }
        (void)optionInt(spec, "threads", 4, 1, 512);
        if (spec.options.contains(QStringLiteral("chat_template")) && !spec.options.value(QStringLiteral("chat_template")).isString())
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("chat_template must be a string"));
        if (spec.options.contains("tool_grammar") && !spec.options.value("tool_grammar").isBool())
            throw Error(ErrorCode::InvalidArgument, "tool_grammar must be a boolean");
        if (spec.options.contains("enable_thinking") && !spec.options.value("enable_thinking").isBool())
            throw Error(ErrorCode::InvalidArgument, "enable_thinking must be a boolean");
        initializeLlama();
        auto selected = std::make_shared<std::vector<ggml_backend_dev_t>>();
        const bool accelerated = device.backend != ComputeBackend::Cpu;
        if (accelerated) {
            for (size_t n = 0; n < ggml_backend_dev_count(); ++n) {
                const auto candidate = ggml_backend_dev_get(n);
                if (deviceId(candidate) == device.deviceId && deviceBackend(candidate) == device.backend) {
                    ggml_backend_dev_props props{};
                    ggml_backend_dev_get_props(candidate, &props);
                    if (props.type == GGML_BACKEND_DEVICE_TYPE_GPU && device.backend != ComputeBackend::Metal) {
                        size_t free = 0, total = 0;
                        ggml_backend_dev_memory(candidate, &free, &total);
                        if (total && estimateMemory(spec, 1).totalBytes() > free)
                            throw Error(ErrorCode::ResourceLimit, QStringLiteral("Insufficient GPU memory for weights, one context and scratch"));
                    }
                    selected->push_back(candidate);
                    break;
                }
            }
            if (selected->empty()) throw Error(ErrorCode::RuntimeUnavailable, QStringLiteral("Selected llama.cpp device disappeared"));
        } else if (!device.deviceId.isEmpty()) {
            throw Error(ErrorCode::InvalidArgument, QStringLiteral("CPU device id must be empty"));
        }
        selected->push_back(nullptr); // Explicit empty list on CPU; nullptr would auto-select all GPUs.
        auto params = llama_model_default_params();
        params.devices = selected->data();
        params.n_gpu_layers = accelerated ? -1 : 0;
        params.split_mode = LLAMA_SPLIT_MODE_NONE;
        params.progress_callback = [](float, void* data) { return !static_cast<const CancellationToken*>(data)->isCancelled(); };
        params.progress_callback_user_data = const_cast<CancellationToken*>(&cancel);
        ModelPtr model(llama_model_load_from_file(spec.path.toUtf8().constData(), params),
                       [selected](llama_model* model) { llama_model_free(model); });
        cancel.throwIfCancelled();
        if (!model) throw Error(ErrorCode::RuntimeFailure, QStringLiteral("llama.cpp could not load GGUF model"));
        auto loaded = std::make_shared<LlamaModel>(std::move(model), spec, accelerated, selected->front());
        // Check context/KV allocation before advertising a successful GPU selection.
        if (accelerated) (void)loaded->createContext(cancel);
        return loaded;
#else
        Q_UNUSED(spec); Q_UNUSED(device); Q_UNUSED(cancel);
        throw Error(ErrorCode::RuntimeUnavailable, QStringLiteral("Rebuild with IILOCALLLM_WITH_LLAMA=ON"));
#endif
    }
};
}
namespace detail {
void probeLlamaHardware(HardwareInfo& hardware)
{
#ifdef IILOCALLLM_WITH_LLAMA
    initializeLlama();
    const auto native = hardware.gpus;
    for (size_t n = 0; n < ggml_backend_dev_count(); ++n) {
        const auto device = ggml_backend_dev_get(n);
        const auto backend = deviceBackend(device);
        if (!backend) continue;
        ggml_backend_dev_props props{};
        ggml_backend_dev_get_props(device, &props);
        if (props.type != GGML_BACKEND_DEVICE_TYPE_GPU && props.type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        GpuInfo gpu;
        gpu.id = deviceId(device);
        gpu.name = QString::fromUtf8(props.description);
        gpu.vendor = *backend == ComputeBackend::Cuda ? GpuVendor::Nvidia : gpuVendor(gpu.name);
        gpu.unifiedMemory = props.type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        for (const auto& observed : native) {
            if (observed.name == gpu.name || (props.device_id && observed.id == QStringLiteral("pci/") + QString::fromUtf8(props.device_id))) {
                gpu.vendor = observed.vendor;
                if (observed.unifiedMemory) gpu.unifiedMemory = observed.unifiedMemory;
                gpu.vramBytes = observed.vramBytes;
                gpu.recommendedWorkingSetBytes = observed.recommendedWorkingSetBytes;
                break;
            }
        }
        if (*backend == ComputeBackend::Metal) {
            // ggml reports a Metal working-set budget as memory_total, not dedicated VRAM.
            if (!gpu.recommendedWorkingSetBytes && props.memory_total) gpu.recommendedWorkingSetBytes = props.memory_total;
        } else if (!gpu.unifiedMemory.value_or(true) && props.memory_total) gpu.vramBytes = props.memory_total;
        if (gpu.unifiedMemory.value_or(false)) gpu.vramBytes.reset();
        if (const auto initialized = ggml_backend_dev_init(device, nullptr)) {
            gpu.availableBackends.append(*backend);
            ggml_backend_free(initialized);
        } else hardware.diagnostics.append(QStringLiteral("Backend initialization failed: ") + gpu.id);
        hardware.gpus.append(gpu);
    }
#else
    hardware.diagnostics.append(QStringLiteral("llama.cpp is not compiled; its CUDA/Vulkan/Metal devices are unavailable"));
#endif
}
}
bool llamaRuntimeAvailable() noexcept
{
#ifdef IILOCALLLM_WITH_LLAMA
    return true;
#else
    return false;
#endif
}
std::shared_ptr<Runtime> createLlamaRuntime() { return std::make_shared<LlamaRuntime>(); }
} // namespace iiLocalLLM
