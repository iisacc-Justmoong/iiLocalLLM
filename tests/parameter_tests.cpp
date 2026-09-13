#include <Parameters.h>
#include <QtTest/QtTest>
#include <QtCore/QJsonArray>
#include <limits>

using namespace iiLocalLLM;

class ParameterTests : public QObject {
    Q_OBJECT
private slots:
    void catalogCoversInferenceTrainingAndAdapters()
    {
        const auto catalog = ParameterCatalog::builtin();
        for (const auto& group : {"llama.common_params_sampling", "transformers.GenerationConfig",
             "transformers.TrainingArguments", "peft.LoraConfig", "trl.GRPOConfig", "vllm.SamplingParams"}) {
            QVERIFY2(catalog.contains(group), group);
            const auto definition = catalog.group(group);
            QVERIFY(!definition.parameters.isEmpty());
            for (const auto& field : definition.parameters) {
                QVERIFY(!field.name.isEmpty());
                QVERIFY(!field.nativeType.isEmpty());
                QVERIFY(!field.source.url.isEmpty());
                QVERIFY(!field.description.isEmpty());
            }
        }
    }
    void typedValuesRejectUnknownNamesAndWrongTypes()
    {
        ParameterObject training("transformers.TrainingArguments");
        training.set("learning_rate", 0.0002);
        training.set("per_device_train_batch_size", 2);
        QCOMPARE(training.value("learning_rate").toDouble(), 0.0002);
        QVERIFY_THROWS_EXCEPTION(Error, training.set("learning_rate", "fast"));
        QVERIFY_THROWS_EXCEPTION(Error, training.set("per_device_train_batch_size", 1.5));
        QVERIFY_THROWS_EXCEPTION(Error, training.set("imaginary_parameter", true));
        QCOMPARE(training.value("learning_rate").toDouble(), 0.0002);
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
        QCOMPARE(restored.toJson(), wire);
        QCOMPARE(restored.object("peft.LoraConfig").toNativeJson().value("r").toInt(), 16);
        QVERIFY(!restored.object("peft.LoraConfig").definition("r").description.isEmpty());
    }
    void trainingConfigurationDoesNotClaimAnInferenceBinding()
    {
        ParameterObject training("transformers.TrainingArguments");
        training.set("learning_rate", 0.0002);
        QVERIFY(!training.definition("learning_rate").nativeBindings.contains("llama.cpp"));
        QVERIFY_THROWS_EXCEPTION(Error, training.requireNativeBinding("llama.cpp"));
    }
    void inheritedTrainingControlsAndOptimizerTuples()
    {
        ParameterObject grpo("trl.GRPOConfig");
        grpo.set("per_device_train_batch_size", 2);
        grpo.set("gradient_checkpointing", true);
        grpo.set("beta", .04);
        QCOMPARE(grpo.value("per_device_train_batch_size"), QJsonValue(2));
        ParameterObject adam("torch.AdamW");
        adam.set("betas", QJsonArray{.9, .999});
        QVERIFY_THROWS_EXCEPTION(Error, adam.set("betas", QJsonArray{.9, "bad"}));
        QVERIFY_THROWS_EXCEPTION(Error, adam.set("betas", QJsonArray{.9}));
        ParameterObject server("sglang.ServerArgs");
        server.set("model_path", "local/model");
        server.set("context_length", 8192);
        QVERIFY_THROWS_EXCEPTION(Error, server.set("dtype", "imaginary_dtype"));
    }
    void defaultsNullUnsetAndValidationBeforeExport()
    {
        ParameterObject training("transformers.TrainingArguments");
        QVERIFY(!training.isSet("learning_rate"));
        QVERIFY(!training.toNativeJson().contains("learning_rate"));
        QVERIFY(training.toNativeJson(true).contains("learning_rate"));
        training.set("hub_token", "test-secret");
        QCOMPARE(training.toNativeJson(false, true).value("hub_token"), QJsonValue("[REDACTED]"));
        training.set("fp16", true); training.set("bf16", true);
        QVERIFY(!training.validate().isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error, training.toNativeJson());
        training.unset("bf16");
        QVERIFY(training.validate().isEmpty());
        QVERIFY_THROWS_EXCEPTION(Error, training.set("learning_rate", std::numeric_limits<double>::infinity()));
        ParameterObject generation("transformers.GenerationConfig");
        generation.set("temperature", QJsonValue::Null);
        QVERIFY(generation.isSet("temperature"));
        QVERIFY(generation.toNativeJson().value("temperature").isNull());
        generation.unset("temperature");
        QVERIFY(!generation.toNativeJson().contains("temperature"));
    }
    void nativeGenerationBindingsAreStrictAndRoundTrip()
    {
        const QJsonObject settings{{"min_p", .05}, {"repetition_penalty", 1.1}, {"frequency_penalty", .2},
            {"logit_bias", QJsonObject{{"42", -5.0}}}, {"stop", QJsonArray{"END"}}};
        const auto options = generationOptionsFromJson(settings);
        QCOMPARE(options.minP, .05);
        QCOMPARE(options.repetitionPenalty, 1.1);
        QCOMPARE(generationOptionsFromJson(generationOptionsToJson(options)).logitBias, options.logitBias);
        QVERIFY_THROWS_EXCEPTION(Error, generationOptionsFromJson({{"min_p", 1.1}}));
        QVERIFY_THROWS_EXCEPTION(Error, generationOptionsFromJson({{"stop", QJsonArray{""}}}));
        QVERIFY_THROWS_EXCEPTION(Error, generationOptionsFromJson({{"logit_bias", QJsonObject{{"bad", 1}}}}));
        QVERIFY_THROWS_EXCEPTION(Error, generationOptionsFromJson({{"imaginary", 1}}));
        ParameterObject llama("llama.common_params_sampling");
        llama.set("min_p", .1); llama.set("penalty_repeat", 1.2);
        const auto bridged = generationOptionsFromParameters(llama);
        QCOMPARE(bridged.minP, .1); QCOMPARE(bridged.repetitionPenalty, 1.2);
        llama.set("mirostat", 2);
        QVERIFY_THROWS_EXCEPTION(Error, generationOptionsFromParameters(llama));
        auto mlx = options; mlx.typicalP = .5;
        QVERIFY_THROWS_EXCEPTION(Error, validateGenerationOptions(mlx, "mlx"));
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
        QVERIFY_THROWS_EXCEPTION(Error, object.set("ratio",2));
        QVERIFY_THROWS_EXCEPTION(Error, object.set("nested",QJsonObject{}));
        QVERIFY_THROWS_EXCEPTION(Error, object.set("nested",QJsonObject{{"rank",2},{"unknown",true}}));
        object.set("nested",QJsonObject{{"rank",2}});
        definition.insert("parameters",QJsonArray{field("bad",{{"type","typo"}})});
        QVERIFY_THROWS_EXCEPTION(Error, ParameterCatalog::fromJson({{"schema_version",1},{"groups",QJsonArray{definition}}}));
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
        QCOMPARE(redacted.value("token"),QJsonValue("[REDACTED]"));
        QCOMPARE(object.toNativeJson().value("children").toArray().first().toObject().value("token"),QJsonValue("test-secret"));
    }
};

QTEST_GUILESS_MAIN(ParameterTests)
#include "parameter_tests.moc"
