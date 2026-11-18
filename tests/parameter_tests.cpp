#include <Parameters.h>
#include "CTestSupport.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <limits>
#include <future>

using namespace iiLocalLLM;

class ParameterTests {
public:
    void immutableIndexesAndCompiledPatternsPreserveValidation()
    {
        auto field = [](const char* name, QJsonObject schema) {
            return QJsonObject{{"name",name},{"native_type","custom"},{"description","Indexed field"},{"schema",schema}};
        };
        auto required = field("required", {{"type","string"},{"pattern","^[a-z]+$"}});
        required["required"] = true;
        auto derived = field("derived", {{"type","integer"}}); derived["read_only"] = true;
        auto optional = field("optional", {{"type","integer"}}); optional["required"] = true; optional["default"] = 4;
        const QJsonObject nested{{"id","nested"},{"parameters",QJsonArray{required,derived,optional}}};
        const auto payload = field("payload", {{"ref","nested"}});
        const auto unionField = field("union", {{"anyOf",QJsonArray{QJsonObject{{"type","string"}},QJsonObject{{"type","null"}}}},{"minLength",2}});
        const QJsonObject root{{"id","root"},{"parameters",QJsonArray{payload,unionField}}};
        const QJsonObject document{{"schema_version",1},{"groups",QJsonArray{nested,root}}};
        const auto catalog = ParameterCatalog::fromJson(document);
        CTEST_COMPARE(catalog.toJson(),document);
        const QJsonObject good{{"required","hello"}};
        CTEST_CHECK(catalog.validateValue("root","payload",good).isEmpty());
        CTEST_CHECK(!catalog.validateValue("root","payload",QJsonObject{}).isEmpty());
        CTEST_CHECK(!catalog.validateValue("root","payload",QJsonObject{{"required","123"}}).isEmpty());
        CTEST_CHECK(!catalog.validateValue("root","payload",QJsonObject{{"required","hello"},{"derived",1}}).isEmpty());
        CTEST_CHECK(!catalog.validateValue("root","payload",QJsonObject{{"required","hello"},{"unknown",1}}).isEmpty());
        CTEST_CHECK(!catalog.validateValue("root","union","x").isEmpty());
        CTEST_CHECK(catalog.validateValue("root","union","xx").isEmpty());
        CTEST_THROWS(Error,catalog.parameter("root","missing"));
        CTEST_THROWS(Error,catalog.parameter("missing","payload"));
        std::vector<std::future<bool>> readers;
        for (int worker=0; worker<4; ++worker) readers.push_back(std::async(std::launch::async,[catalog,good] {
            for (int i=0;i<100;++i) if (!catalog.validateValue("root","payload",good).isEmpty()) return false;
            return true;
        }));
        for (auto& reader:readers) CTEST_CHECK(reader.get());
        auto otherRequired=required;
        otherRequired["schema"]=QJsonObject{{"type","string"},{"pattern","^[0-9]+$"}};
        const auto other=ParameterCatalog::fromJson({{"schema_version",1},{"groups",QJsonArray{QJsonObject{{"id","nested"},{"parameters",QJsonArray{otherRequired}}},root}}});
        CTEST_CHECK(!other.validateValue("root","payload",good).isEmpty());
        CTEST_CHECK(catalog.validateValue("root","payload",good).isEmpty());
        auto invalid=field("bad",{{"anyOf",QJsonArray{QJsonObject{{"type","string"}},QJsonObject{{"type","string"},{"pattern","["}}}}});
        CTEST_THROWS(Error,ParameterCatalog::fromJson({{"schema_version",1},{"groups",QJsonArray{QJsonObject{{"id","invalid"},{"parameters",QJsonArray{invalid}}}}}}));
    }
    void catalogCoversInferenceTrainingAndAdapters()
    {
        const auto catalog = ParameterCatalog::builtin();
        for (const auto& group : {"llama.common_params_sampling", "transformers.GenerationConfig",
             "transformers.TrainingArguments", "peft.LoraConfig", "trl.GRPOConfig", "vllm.SamplingParams"}) {
            CTEST_CHECK_MESSAGE(catalog.contains(group), group);
            const auto definition = catalog.group(group);
            CTEST_CHECK(!definition.parameters.isEmpty());
            for (const auto& field : definition.parameters) {
                CTEST_CHECK(!field.name.isEmpty());
                CTEST_CHECK(!field.nativeType.isEmpty());
                CTEST_CHECK(!field.source.url.isEmpty());
                CTEST_CHECK(!field.description.isEmpty());
            }
        }
    }
    void typedValuesRejectUnknownNamesAndWrongTypes()
    {
        ParameterObject training("transformers.TrainingArguments");
        training.set("learning_rate", 0.0002);
        training.set("per_device_train_batch_size", 2);
        CTEST_COMPARE(training.value("learning_rate").toDouble(), 0.0002);
        CTEST_THROWS(Error, training.set("learning_rate", "fast"));
        CTEST_THROWS(Error, training.set("per_device_train_batch_size", 1.5));
        CTEST_THROWS(Error, training.set("imaginary_parameter", true));
        CTEST_COMPARE(training.value("learning_rate").toDouble(), 0.0002);
    }
    void configurationRoundTripPreservesNativeNames()
    {
        ControlParameters control;
        ParameterObject training("transformers.TrainingArguments");
        training.set("learning_rate", 0.0002);
        ParameterObject lora("peft.LoraConfig");
        lora.set("r", 16);
        lora.set("lora_alpha", 32);
        lora.set("target_modules", QJsonArray{"q_proj", "v_proj"});
        control.set(training);
        control.set(lora);
        const auto wire = control.toJson();
        const auto restored = ControlParameters::fromJson(wire);
        CTEST_COMPARE(restored.toJson(), wire);
        CTEST_COMPARE(restored.object("peft.LoraConfig").toNativeJson().value("r").toInt(), 16);
        CTEST_CHECK(!restored.object("peft.LoraConfig").definition("r").description.isEmpty());
    }
    void trainingConfigurationDoesNotClaimAnInferenceBinding()
    {
        ParameterObject training("transformers.TrainingArguments");
        training.set("learning_rate", 0.0002);
        CTEST_CHECK(!training.definition("learning_rate").nativeBindings.contains("llama.cpp"));
        CTEST_THROWS(Error, training.requireNativeBinding("llama.cpp"));
    }
    void inheritedTrainingControlsAndOptimizerTuples()
    {
        ParameterObject grpo("trl.GRPOConfig");
        grpo.set("per_device_train_batch_size", 2);
        grpo.set("gradient_checkpointing", true);
        grpo.set("beta", .04);
        CTEST_COMPARE(grpo.value("per_device_train_batch_size"), QJsonValue(2));
        ParameterObject adam("torch.AdamW");
        adam.set("betas", QJsonArray{.9, .999});
        CTEST_THROWS(Error, adam.set("betas", QJsonArray{.9, "bad"}));
        CTEST_THROWS(Error, adam.set("betas", QJsonArray{.9}));
        ParameterObject server("sglang.ServerArgs");
        server.set("model_path", "local/model");
        server.set("context_length", 8192);
        CTEST_THROWS(Error, server.set("dtype", "imaginary_dtype"));
    }
    void defaultsNullUnsetAndValidationBeforeExport()
    {
        ParameterObject training("transformers.TrainingArguments");
        CTEST_CHECK(!training.isSet("learning_rate"));
        CTEST_CHECK(!training.toNativeJson().contains("learning_rate"));
        CTEST_CHECK(training.toNativeJson(true).contains("learning_rate"));
        training.set("hub_token", "test-secret");
        CTEST_COMPARE(training.toNativeJson(false, true).value("hub_token"), QJsonValue("[REDACTED]"));
        training.set("fp16", true); training.set("bf16", true);
        CTEST_CHECK(!training.validate().isEmpty());
        CTEST_THROWS(Error, training.toNativeJson());
        training.unset("bf16");
        CTEST_CHECK(training.validate().isEmpty());
        CTEST_THROWS(Error, training.set("learning_rate", std::numeric_limits<double>::infinity()));
        ParameterObject generation("transformers.GenerationConfig");
        generation.set("temperature", QJsonValue::Null);
        CTEST_CHECK(generation.isSet("temperature"));
        CTEST_CHECK(generation.toNativeJson().value("temperature").isNull());
        generation.unset("temperature");
        CTEST_CHECK(!generation.toNativeJson().contains("temperature"));
    }
    void nativeGenerationBindingsAreStrictAndRoundTrip()
    {
        const QJsonObject settings{{"min_p", .05}, {"repetition_penalty", 1.1}, {"frequency_penalty", .2},
            {"logit_bias", QJsonObject{{"42", -5.0}}}, {"stop", QJsonArray{"END"}}};
        const auto options = generationOptionsFromJson(settings);
        CTEST_COMPARE(options.minP, .05);
        CTEST_COMPARE(options.repetitionPenalty, 1.1);
        CTEST_COMPARE(generationOptionsFromJson(generationOptionsToJson(options)).logitBias, options.logitBias);
        CTEST_THROWS(Error, generationOptionsFromJson({{"min_p", 1.1}}));
        CTEST_THROWS(Error, generationOptionsFromJson({{"stop", QJsonArray{""}}}));
        CTEST_THROWS(Error, generationOptionsFromJson({{"logit_bias", QJsonObject{{"bad", 1}}}}));
        CTEST_THROWS(Error, generationOptionsFromJson({{"imaginary", 1}}));
        ParameterObject llama("llama.common_params_sampling");
        llama.set("min_p", .1); llama.set("penalty_repeat", 1.2);
        const auto bridged = generationOptionsFromParameters(llama);
        CTEST_COMPARE(bridged.minP, .1); CTEST_COMPARE(bridged.repetitionPenalty, 1.2);
        llama.set("mirostat", 2);
        CTEST_THROWS(Error, generationOptionsFromParameters(llama));
        auto mlx = options; mlx.typicalP = .5;
        CTEST_THROWS(Error, validateGenerationOptions(mlx, "mlx"));
    }
    void importedSchemasEnforceUnionBoundsAndNestedRequiredFields()
    {
        const auto field = [](QString name, QJsonObject schema) { return QJsonObject{{"name",name},{"native_type","custom"},
            {"description","Custom control"},{"schema",schema}}; };
        QJsonObject definition{{"id","custom.Options"},{"parameters",QJsonArray{
            field("ratio", {{"anyOf",QJsonArray{QJsonObject{{"type","number"}}, QJsonObject{{"type","null"}}}}, {"minimum",0},{"maximum",1}}),
            field("nested", {{"type","object"}, {"properties",QJsonObject{{"rank",QJsonObject{{"type","integer"},{"minimum",1}}}}},
                {"required",QJsonArray{"rank"}}, {"additionalProperties",false}})}}};
        const auto catalog = ParameterCatalog::fromJson({{"schema_version",1},{"groups",QJsonArray{definition}}});
        ParameterObject object("custom.Options",catalog);
        object.set("ratio", .5);
        CTEST_THROWS(Error, object.set("ratio",2));
        CTEST_THROWS(Error, object.set("nested",QJsonObject{}));
        CTEST_THROWS(Error, object.set("nested",QJsonObject{{"rank",2},{"unknown",true}}));
        object.set("nested",QJsonObject{{"rank",2}});
        definition.insert("parameters",QJsonArray{field("bad",{{"type","typo"}})});
        CTEST_THROWS(Error, ParameterCatalog::fromJson({{"schema_version",1},{"groups",QJsonArray{definition}}}));
    }
    void redactionFollowsNestedObjectReferences()
    {
        const QJsonObject token{{"name","token"},{"native_type","str"},{"description","Secret"},
            {"sensitive",true},{"schema",QJsonObject{{"type","string"}}}};
        const QJsonObject children{{"name","children"},{"native_type","list[Secrets]"},{"description","Children"},
            {"schema",QJsonObject{{"type","array"},{"items",QJsonObject{{"type","object"},{"ref","custom.Secrets"}}}}}};
        const auto catalog = ParameterCatalog::fromJson({{"schema_version",1},{"groups",QJsonArray{
            QJsonObject{{"id","custom.Secrets"},{"parameters",QJsonArray{token}}},
            QJsonObject{{"id","custom.Parent"},{"parameters",QJsonArray{children}}}}}});
        ParameterObject object("custom.Parent",catalog);
        object.set("children",QJsonArray{QJsonObject{{"token","test-secret"}}});
        const auto redacted = object.toNativeJson(false,true).value("children").toArray().first().toObject();
        CTEST_COMPARE(redacted.value("token"),QJsonValue("[REDACTED]"));
        CTEST_COMPARE(object.toNativeJson().value("children").toArray().first().toObject().value("token"),QJsonValue("test-secret"));
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    iillm_ctest::CTestRunner runner;
    ParameterTests tests;
    runner.run("immutableIndexesAndCompiledPatternsPreserveValidation", [&] { tests.immutableIndexesAndCompiledPatternsPreserveValidation(); });
    runner.run("catalogCoversInferenceTrainingAndAdapters", [&] { tests.catalogCoversInferenceTrainingAndAdapters(); });
    runner.run("typedValuesRejectUnknownNamesAndWrongTypes", [&] { tests.typedValuesRejectUnknownNamesAndWrongTypes(); });
    runner.run("configurationRoundTripPreservesNativeNames", [&] { tests.configurationRoundTripPreservesNativeNames(); });
    runner.run("trainingConfigurationDoesNotClaimAnInferenceBinding", [&] { tests.trainingConfigurationDoesNotClaimAnInferenceBinding(); });
    runner.run("inheritedTrainingControlsAndOptimizerTuples", [&] { tests.inheritedTrainingControlsAndOptimizerTuples(); });
    runner.run("defaultsNullUnsetAndValidationBeforeExport", [&] { tests.defaultsNullUnsetAndValidationBeforeExport(); });
    runner.run("nativeGenerationBindingsAreStrictAndRoundTrip", [&] { tests.nativeGenerationBindingsAreStrictAndRoundTrip(); });
    runner.run("importedSchemasEnforceUnionBoundsAndNestedRequiredFields", [&] { tests.importedSchemasEnforceUnionBoundsAndNestedRequiredFields(); });
    runner.run("redactionFollowsNestedObjectReferences", [&] { tests.redactionFollowsNestedObjectReferences(); });
    return runner.result();
}
