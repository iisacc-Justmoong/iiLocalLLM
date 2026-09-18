#pragma once
#include "Types.h"
#include <QtCore/QJsonArray>
#include <QtCore/QMap>
#include <optional>
#include <concepts>

namespace iiLocalLLM {

enum class ParameterKind { Boolean, Integer, Number, String, Array, Object, Union, Enumeration, Null, External, Opaque };
enum class ParameterPhase { Generation, Loading, Training, FineTuning, Quantization, Serving, Infrastructure };
IILOCALLLM_EXPORT QString enumName(ParameterKind kind);
IILOCALLLM_EXPORT QString enumName(ParameterPhase phase);

struct ParameterSource {
    QString provider;
    QString revision;
    QString file;
    QString url;
    QString sha256;
    int line = 0;
};

// Default expressions are preserved separately: an unevaluated backend expression is not JSON null.
struct ParameterDefinition {
    QString name;
    QString nativeType;
    QString description;
    ParameterKind kind = ParameterKind::Opaque;
    ParameterPhase phase = ParameterPhase::Generation;
    QJsonObject schema;
    bool hasDefault = false;
    QJsonValue defaultValue = QJsonValue::Undefined;
    QString defaultExpression;
    bool required = false;
    bool readOnly = false;
    bool sensitive = false;
    QStringList nativeBindings;
    QString bindingKey;
    QString bindingNote;
    ParameterSource source;
};

struct ParameterGroupDefinition {
    QString id;
    QString description;
    QStringList bases;
    ParameterPhase phase = ParameterPhase::Generation;
    QList<ParameterDefinition> parameters;
};

struct ParameterIssue {
    QString path;
    QString message;
};

class IILOCALLLM_EXPORT ParameterCatalog {
public:
    static ParameterCatalog builtin();
    // A caller may import another published catalog without losing provider-specific field names.
    static ParameterCatalog fromJson(const QJsonObject& document);
    QStringList groups() const;
    bool contains(const QString& group) const;
    ParameterGroupDefinition group(const QString& id) const;
    ParameterDefinition parameter(const QString& group, const QString& name) const;
    QJsonObject toJson() const;
    QList<ParameterIssue> validateValue(const QString& group, const QString& name, const QJsonValue& value) const;
private:
    class Impl;
    explicit ParameterCatalog(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> d;
};

// An instance of one provider's detailed configuration object. Unset, null and default are distinct.
class IILOCALLLM_EXPORT ParameterObject {
public:
    explicit ParameterObject(QString group, ParameterCatalog catalog = ParameterCatalog::builtin());
    QString group() const { return group_; }
    ParameterGroupDefinition definition() const;
    ParameterDefinition definition(const QString& field) const;
    bool isSet(const QString& field) const;
    QJsonValue value(const QString& field) const;
    void set(const QString& field, const QJsonValue& value);
    // QJsonValue converts NaN/Infinity to null; floating-point setters reject them first.
    template<std::floating_point T> void set(const QString& field, T value) { setNumber(field, double(value)); }
    void setNumber(const QString& field, double value);
    void unset(const QString& field);
    QList<ParameterIssue> validate(bool requireRequiredFields = false) const;
    // Explicitly-set values only by default; optional defaults are resolved only when literal.
    QJsonObject toNativeJson(bool includeDefaults = false, bool redactSecrets = false) const;
    static ParameterObject fromNativeJson(const QString& group, const QJsonObject& values,
                                         ParameterCatalog catalog = ParameterCatalog::builtin());
    // Configuration export is distinct from execution. Reject every explicitly-set unbound field.
    void requireNativeBinding(const QString& runtime) const;
private:
    QString group_;
    ParameterCatalog catalog_;
    QJsonObject values_;
};

class IILOCALLLM_EXPORT ControlParameters {
public:
    explicit ControlParameters(ParameterCatalog catalog = ParameterCatalog::builtin());
    void set(const ParameterObject& object);
    bool contains(const QString& group) const;
    ParameterObject object(const QString& group) const;
    QStringList groups() const;
    QJsonObject toJson(bool includeDefaults = false, bool redactSecrets = false) const;
    static ControlParameters fromJson(const QJsonObject& document, ParameterCatalog catalog = ParameterCatalog::builtin());
private:
    ParameterCatalog catalog_;
    QMap<QString, ParameterObject> objects_;
};

// These bridges apply only fields with a verified generation binding; unbound fields fail.
IILOCALLLM_EXPORT GenerationOptions generationOptionsFromJson(const QJsonObject& values);
IILOCALLLM_EXPORT GenerationOptions generationOptionsFromParameters(const ParameterObject& object);
IILOCALLLM_EXPORT QJsonObject generationOptionsToJson(const GenerationOptions& options);
IILOCALLLM_EXPORT void validateGenerationOptions(const GenerationOptions& options, const QString& runtime = {});

} // namespace iiLocalLLM
