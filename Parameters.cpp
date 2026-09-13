#include "Parameters.h"
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <QtCore/QRegularExpression>
#include <algorithm>
#include <cmath>

namespace iiLocalLLM {
namespace {
[[noreturn]] void fail(const QString& message) { throw Error(ErrorCode::InvalidArgument, message); }
QStringList strings(const QJsonValue& value)
{
    QStringList result;
    for (const auto& item : value.toArray()) result.append(item.toString());
    return result;
}
ParameterKind kind(const QJsonObject& schema)
{
    if (schema.contains("anyOf")) return ParameterKind::Union;
    if (schema.contains("enum")) return ParameterKind::Enumeration;
    const auto type = schema.value("type").toString();
    if (type == "boolean") return ParameterKind::Boolean;
    if (type == "integer") return ParameterKind::Integer;
    if (type == "number") return ParameterKind::Number;
    if (type == "string") return ParameterKind::String;
    if (type == "array") return ParameterKind::Array;
    if (type == "object" || schema.contains("ref")) return ParameterKind::Object;
    if (type == "null") return ParameterKind::Null;
    if (type == "external") return ParameterKind::External;
    return ParameterKind::Opaque;
}
ParameterPhase phase(const QString& value)
{
    if (value == "loading") return ParameterPhase::Loading;
    if (value == "training") return ParameterPhase::Training;
    if (value == "fine_tuning") return ParameterPhase::FineTuning;
    if (value == "quantization") return ParameterPhase::Quantization;
    if (value == "serving") return ParameterPhase::Serving;
    if (value == "infrastructure") return ParameterPhase::Infrastructure;
    if (value.isEmpty() || value == "generation") return ParameterPhase::Generation;
    fail("Unknown parameter phase: " + value);
}
QJsonValue redactValue(const ParameterCatalog& catalog, const QJsonObject& schema, QJsonValue value, int depth = 0)
{
    if (depth > 32) return "[REDACTED]";
    for (const auto& branch : schema.value("anyOf").toArray())
        value = redactValue(catalog, branch.toObject(), value, depth + 1);
    if (value.isObject()) {
        auto object = value.toObject();
        const auto reference = schema.value("ref").toString();
        if (catalog.contains(reference)) for (const auto& field : catalog.group(reference).parameters) {
            if (!object.contains(field.name)) continue;
            object.insert(field.name, field.sensitive ? QJsonValue("[REDACTED]")
                : redactValue(catalog, field.schema, object.value(field.name), depth + 1));
        }
        const auto properties = schema.value("properties").toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto child = properties.value(it.key()).isObject() ? properties.value(it.key()).toObject()
                : schema.value("additionalProperties").toObject();
            if (!child.isEmpty()) it.value() = redactValue(catalog,child,it.value(),depth+1);
        }
        return object;
    }
    if (value.isArray()) {
        auto array = value.toArray();
        const auto prefix = schema.value("prefixItems").toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            const auto child = i < prefix.size() ? prefix[i].toObject() : schema.value("items").toObject();
            if (!child.isEmpty()) array[i] = redactValue(catalog,child,array[i],depth+1);
        }
        return array;
    }
    return value;
}
}
QString enumName(ParameterKind value)
{
    switch (value) {
    case ParameterKind::Boolean: return "boolean";
    case ParameterKind::Integer: return "integer";
    case ParameterKind::Number: return "number";
    case ParameterKind::String: return "string";
    case ParameterKind::Array: return "array";
    case ParameterKind::Object: return "object";
    case ParameterKind::Union: return "union";
    case ParameterKind::Enumeration: return "enum";
    case ParameterKind::Null: return "null";
    case ParameterKind::External: return "external";
    case ParameterKind::Opaque: return "opaque";
    }
    return {};
}
QString enumName(ParameterPhase value)
{
    switch (value) {
    case ParameterPhase::Generation: return "generation";
    case ParameterPhase::Loading: return "loading";
    case ParameterPhase::Training: return "training";
    case ParameterPhase::FineTuning: return "fine_tuning";
    case ParameterPhase::Quantization: return "quantization";
    case ParameterPhase::Serving: return "serving";
    case ParameterPhase::Infrastructure: return "infrastructure";
    }
    return {};
}
class ParameterCatalog::Impl {
public:
    QJsonObject document;
    QMap<QString, ParameterGroupDefinition> groups;
    void checkSchema(const QJsonObject& schema, int depth = 0) const
    {
        if (depth > 32) fail("Parameter schema exceeds nesting limit");
        static const QSet<QString> keys = {"type", "enum", "anyOf", "nullable", "ref", "items", "prefixItems",
            "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "minItems", "maxItems",
            "minLength", "maxLength", "additionalProperties", "properties", "required", "pattern"};
        for (auto it = schema.begin(); it != schema.end(); ++it) {
            if (!keys.contains(it.key())) fail("Unsupported parameter schema keyword: " + it.key());
            if (it.key() == "type" && !QStringList{"boolean", "integer", "number", "string", "array", "object", "null", "opaque", "external"}.contains(it.value().toString()))
                fail("Unknown parameter schema type");
            if (it.key() == "ref" && (!it.value().isString() || !groups.contains(it.value().toString()))) fail("Unknown parameter schema reference");
            if (it.key() == "nullable" && !it.value().isBool()) fail("nullable must be boolean");
            if (it.key() == "enum" && (!it.value().isArray() || it.value().toArray().isEmpty())) fail("enum must contain choices");
            if (it.key() == "anyOf" || it.key() == "prefixItems") {
                if (!it.value().isArray() || (it.key() == "anyOf" && it.value().toArray().isEmpty())) fail("Invalid schema alternatives");
                for (const auto& child : it.value().toArray()) {
                    if (!child.isObject()) fail("Schema alternatives must be objects");
                    checkSchema(child.toObject(), depth + 1);
                }
            }
            if (it.key() == "items" || it.key() == "additionalProperties") {
                if (it.value().isObject()) checkSchema(it.value().toObject(), depth + 1);
                else if (it.key() != "additionalProperties" || !it.value().isBool()) fail("Invalid item schema");
            }
            if (it.key() == "properties") {
                if (!it.value().isObject()) fail("properties must be an object");
                const auto properties = it.value().toObject();
                for (const auto& child : properties) {
                    if (!child.isObject()) fail("Property schema must be an object");
                    checkSchema(child.toObject(), depth + 1);
                }
            }
            if (it.key() == "required") {
                if (!it.value().isArray()) fail("required must be an array");
                for (const auto& field : it.value().toArray()) if (!field.isString()) fail("required entries must be field names");
            }
            if (it.key() == "pattern" && (!it.value().isString() || !QRegularExpression(it.value().toString()).isValid())) fail("Invalid string pattern");
            if (it.key().startsWith("min") || it.key().startsWith("max") || it.key().startsWith("exclusive")) {
                if (!it.value().isDouble() || !std::isfinite(it.value().toDouble())) fail("Schema bounds must be finite numbers");
                if ((it.key().endsWith("Length") || it.key().endsWith("Items"))
                    && (it.value().toDouble() < 0 || std::trunc(it.value().toDouble()) != it.value().toDouble())) fail("Invalid size bound");
            }
        }
    }
    bool accepts(const QJsonObject& schema, const QJsonValue& value, int depth = 0) const
    {
        if (depth > 32 || value.isUndefined()) return false;
        if (value.isNull() && schema.value("nullable").toBool()) return true;
        if (schema.contains("anyOf")) {
            bool matched = false;
            for (const auto& branch : schema.value("anyOf").toArray())
                matched |= accepts(branch.toObject(), value, depth + 1);
            if (!matched) return false;
        }
        if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) return false;
        const auto type = schema.value("type").toString();
        if (type == "external") return false;
        if (type == "null") return value.isNull();
        if (type == "boolean" && !value.isBool()) return false;
        if (type == "string" && !value.isString()) return false;
        if (type == "integer" || type == "number") {
            if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
            if (type == "integer" && QJsonValue(value.toInteger()) != value) return false;
        }
        if (value.isDouble()) {
            const auto number = value.toDouble();
            for (const auto* key : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"}) {
                if (!schema.contains(key)) continue;
                const auto bound = schema.value(key).toDouble();
                if ((QLatin1String(key) == "minimum" && number < bound)
                    || (QLatin1String(key) == "maximum" && number > bound)
                    || (QLatin1String(key) == "exclusiveMinimum" && number <= bound)
                    || (QLatin1String(key) == "exclusiveMaximum" && number >= bound)) return false;
            }
        }
        if (type == "array" && !value.isArray()) return false;
        if (value.isArray()) {
            const auto array = value.toArray();
            if (schema.contains("minItems") && array.size() < schema.value("minItems").toInt()) return false;
            if (schema.contains("maxItems") && array.size() > schema.value("maxItems").toInt()) return false;
            if (schema.value("items").isObject()) for (const auto& item : array)
                if (!accepts(schema.value("items").toObject(), item, depth + 1)) return false;
            if (schema.value("prefixItems").isArray()) {
                const auto items = schema.value("prefixItems").toArray();
                if (items.size() != array.size()) return false;
                for (qsizetype n = 0; n < items.size(); ++n)
                    if (!accepts(items[n].toObject(), array[n], depth + 1)) return false;
            }
        }
        if (type == "object" || schema.contains("ref")) {
            if (!value.isObject()) return false;
        }
        if (value.isObject()) {
            const auto object = value.toObject();
            const auto properties = schema.value("properties").toObject();
            for (const auto& name : schema.value("required").toArray()) if (!object.contains(name.toString())) return false;
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (properties.contains(it.key())) {
                    if (!accepts(properties.value(it.key()).toObject(), it.value(), depth + 1)) return false;
                } else if (schema.value("additionalProperties").isObject()) {
                    if (!accepts(schema.value("additionalProperties").toObject(), it.value(), depth + 1)) return false;
                } else if (schema.value("additionalProperties") == false) return false;
            }
            const auto reference = schema.value("ref").toString();
            if (groups.contains(reference)) {
                const auto& fields = groups[reference].parameters;
                for (const auto& field : fields)
                    if (field.required && !field.hasDefault && !field.readOnly && !object.contains(field.name)) return false;
                for (auto it = object.begin(); it != object.end(); ++it) {
                    const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.name == it.key(); });
                    if (field == fields.end() || field->readOnly || !accepts(field->schema, it.value(), depth + 1)) return false;
                }
            }
        }
        if (value.isString()) {
            const auto length = value.toString().size();
            if (schema.contains("minLength") && length < schema.value("minLength").toInt()) return false;
            if (schema.contains("maxLength") && length > schema.value("maxLength").toInt()) return false;
            if (schema.contains("pattern") && !QRegularExpression(schema.value("pattern").toString()).match(value.toString()).hasMatch()) return false;
        }
        // Opaque backend values are retained as JSON, never interpreted as executable code.
        return true;
    }
};
ParameterCatalog::ParameterCatalog(std::shared_ptr<const Impl> impl) : d(std::move(impl)) {}
ParameterCatalog ParameterCatalog::builtin()
{
    static const auto catalog = [] {
        QFile input(":/iiLocalLLM/parameters.json");
        if (!input.open(QIODevice::ReadOnly)) throw Error(ErrorCode::StorageFailure, input.errorString());
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(input.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !json.isObject()) fail("Invalid built-in parameter catalog");
        return fromJson(json.object());
    }();
    return catalog;
}
ParameterCatalog ParameterCatalog::fromJson(const QJsonObject& document)
{
    if (document.value("schema_version") != 1 || !document.value("groups").isArray()) fail("Parameter catalog requires schema_version=1 and groups");
    auto data = std::make_shared<Impl>(); data->document = document;
    for (const auto& item : document.value("groups").toArray()) {
        if (!item.isObject()) fail("Parameter group must be an object");
        const auto group = item.toObject();
        ParameterGroupDefinition result;
        result.id = group.value("id").toString(); result.description = group.value("description").toString();
        result.bases = strings(group.value("bases")); result.phase = phase(group.value("phase").toString());
        if (result.id.isEmpty() || data->groups.contains(result.id) || !group.value("parameters").isArray()) fail("Invalid or duplicate parameter group");
        QSet<QString> names;
        for (const auto& field : group.value("parameters").toArray()) {
            if (!field.isObject() || !field.toObject().value("schema").isObject()) fail("Parameter definition requires a schema object");
            const auto object = field.toObject(); ParameterDefinition p;
            p.name = object.value("name").toString(); p.nativeType = object.value("native_type").toString();
            p.description = object.value("description").toString(); p.schema = object.value("schema").toObject(); p.kind = kind(p.schema);
            p.phase = result.phase; p.hasDefault = object.contains("default"); p.defaultValue = object.value("default");
            p.defaultExpression = object.value("default_expression").toString(); p.required = object.value("required").toBool();
            p.readOnly = object.value("read_only").toBool(); p.sensitive = object.value("sensitive").toBool();
            p.nativeBindings = strings(object.value("native_bindings")); p.bindingNote = object.value("binding_note").toString();
            p.bindingKey = object.value("binding_key").toString();
            const auto source = object.value("source").toObject();
            p.source = {source.value("provider").toString(), source.value("revision").toString(), source.value("path").toString(),
                        source.value("url").toString(), source.value("sha256").toString(), source.value("line").toInt()};
            if (p.name.isEmpty() || names.contains(p.name) || p.nativeType.isEmpty() || p.description.isEmpty()) fail("Invalid parameter definition in " + result.id);
            names.insert(p.name); result.parameters.append(p);
        }
        data->groups.insert(result.id, result);
    }
    for (const auto& group : data->groups)
        for (const auto& field : group.parameters) data->checkSchema(field.schema);
    return ParameterCatalog(data);
}
QStringList ParameterCatalog::groups() const { return d->groups.keys(); }
bool ParameterCatalog::contains(const QString& group) const { return d->groups.contains(group); }
ParameterGroupDefinition ParameterCatalog::group(const QString& id) const
{
    const auto found = d->groups.constFind(id);
    if (found == d->groups.cend()) fail("Unknown parameter group: " + id);
    return found.value();
}
ParameterDefinition ParameterCatalog::parameter(const QString& id, const QString& name) const
{
    const auto definition = group(id);
    for (const auto& field : definition.parameters) if (field.name == name) return field;
    fail("Unknown parameter: " + id + "." + name);
}
QJsonObject ParameterCatalog::toJson() const { return d->document; }
QList<ParameterIssue> ParameterCatalog::validateValue(const QString& group, const QString& name, const QJsonValue& value) const
{
    const auto field = parameter(group, name);
    if (field.readOnly) return {{group + "." + name, "Parameter is derived/read-only"}};
    if (!d->accepts(field.schema, value)) return {{group + "." + name, "Expected " + field.nativeType + " satisfying "
        + QString::fromUtf8(QJsonDocument(field.schema).toJson(QJsonDocument::Compact))}};
    return {};
}
ParameterObject::ParameterObject(QString group, ParameterCatalog catalog) : group_(std::move(group)), catalog_(std::move(catalog))
{ (void)catalog_.group(group_); }
ParameterGroupDefinition ParameterObject::definition() const { return catalog_.group(group_); }
ParameterDefinition ParameterObject::definition(const QString& name) const { return catalog_.parameter(group_, name); }
bool ParameterObject::isSet(const QString& field) const { (void)definition(field); return values_.contains(field); }
QJsonValue ParameterObject::value(const QString& field) const
{
    const auto p = definition(field);
    return values_.contains(field) ? values_.value(field) : p.defaultValue;
}
void ParameterObject::set(const QString& field, const QJsonValue& value)
{
    const auto issues = catalog_.validateValue(group_, field, value);
    if (!issues.isEmpty()) fail(issues.first().path + ": " + issues.first().message);
    values_.insert(field, value);
}
void ParameterObject::setNumber(const QString& field, double value)
{
    if (!std::isfinite(value)) fail(group_ + "." + field + ": Number must be finite");
    set(field, QJsonValue(value));
}
void ParameterObject::unset(const QString& field) { (void)definition(field); values_.remove(field); }
QList<ParameterIssue> ParameterObject::validate(bool required) const
{
    QList<ParameterIssue> issues;
    for (const auto& field : definition().parameters) {
        if (values_.contains(field.name)) issues.append(catalog_.validateValue(group_, field.name, values_.value(field.name)));
        else if (required && field.required && !field.hasDefault) issues.append({group_ + "." + field.name, "Required parameter is unset"});
    }
    if (group_.startsWith("transformers.") || group_.startsWith("trl.")) {
    if (values_.value("fp16").toBool() && values_.value("bf16").toBool())
        issues.append({group_, "fp16 and bf16 are mutually exclusive"});
    }
    return issues;
}
QJsonObject ParameterObject::toNativeJson(bool defaults, bool redact) const
{
    const auto issues = validate();
    if (!issues.isEmpty()) fail(issues.first().path + ": " + issues.first().message);
    QJsonObject object = values_;
    for (const auto& field : definition().parameters) {
        if (defaults && field.hasDefault && !field.readOnly && !object.contains(field.name)) object.insert(field.name, field.defaultValue);
        if (redact && object.contains(field.name)) object.insert(field.name, field.sensitive ? QJsonValue("[REDACTED]")
            : redactValue(catalog_,field.schema,object.value(field.name)));
    }
    return object;
}
ParameterObject ParameterObject::fromNativeJson(const QString& group, const QJsonObject& values, ParameterCatalog catalog)
{
    ParameterObject object(group, std::move(catalog));
    for (auto it = values.begin(); it != values.end(); ++it) object.set(it.key(), it.value());
    const auto issues = object.validate();
    if (!issues.isEmpty()) fail(issues.first().message);
    return object;
}
void ParameterObject::requireNativeBinding(const QString& runtime) const
{
    const auto issues = validate();
    if (!issues.isEmpty()) fail(issues.first().path + ": " + issues.first().message);
    for (auto it = values_.begin(); it != values_.end(); ++it)
        if (!definition(it.key()).nativeBindings.contains(runtime))
            throw Error(ErrorCode::RuntimeUnavailable, group_ + "." + it.key() + " has no execution binding for " + runtime);
}
ControlParameters::ControlParameters(ParameterCatalog catalog) : catalog_(std::move(catalog)) {}
void ControlParameters::set(const ParameterObject& object)
{
    objects_.insert(object.group(), ParameterObject::fromNativeJson(object.group(), object.toNativeJson(), catalog_));
}
bool ControlParameters::contains(const QString& group) const { return objects_.contains(group); }
ParameterObject ControlParameters::object(const QString& group) const
{
    const auto found = objects_.constFind(group);
    return found == objects_.cend() ? ParameterObject(group, catalog_) : found.value();
}
QStringList ControlParameters::groups() const { return objects_.keys(); }
QJsonObject ControlParameters::toJson(bool defaults, bool redact) const
{
    QJsonObject objects;
    for (auto it = objects_.begin(); it != objects_.end(); ++it) objects.insert(it.key(), it.value().toNativeJson(defaults, redact));
    return {{"schema_version", 1}, {"objects", objects}};
}
ControlParameters ControlParameters::fromJson(const QJsonObject& document, ParameterCatalog catalog)
{
    if (document.value("schema_version") != 1 || !document.value("objects").isObject()) fail("Control parameters require schema_version=1 and objects");
    for (auto it = document.begin(); it != document.end(); ++it)
        if (it.key() != "schema_version" && it.key() != "objects") fail("Unknown control document field: " + it.key());
    ControlParameters result(catalog);
    const auto objects = document.value("objects").toObject();
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        if (!it.value().isObject()) fail("Control parameter groups must be objects");
        result.set(ParameterObject::fromNativeJson(it.key(), it.value().toObject(), catalog));
    }
    return result;
}
QJsonObject generationOptionsToJson(const GenerationOptions& o)
{
    return {{"max_tokens",o.maxTokens},{"temperature",o.temperature},{"top_p",o.topP},{"top_k",o.topK},
        {"seed",qint64(o.seed)},{"stop",QJsonArray::fromStringList(o.stop)},{"min_p",o.minP},
        {"typical_p",o.typicalP},{"min_keep",o.minKeep},{"repetition_penalty",o.repetitionPenalty},
        {"repetition_context_size",o.repetitionContextSize},{"presence_penalty",o.presencePenalty},
        {"frequency_penalty",o.frequencyPenalty},{"xtc_probability",o.xtcProbability},
        {"xtc_threshold",o.xtcThreshold},{"logit_bias",o.logitBias}};
}
GenerationOptions generationOptionsFromJson(const QJsonObject& values)
{
    const auto p = ParameterObject::fromNativeJson("iiLocalLLM.GenerationOptions",values);
    GenerationOptions o;
    o.maxTokens = p.value("max_tokens").toInt(); o.temperature = p.value("temperature").toDouble();
    o.topP = p.value("top_p").toDouble(); o.topK = p.value("top_k").toInt(); o.seed = quint32(p.value("seed").toInteger());
    o.stop = strings(p.value("stop")); o.minP = p.value("min_p").toDouble(); o.typicalP = p.value("typical_p").toDouble();
    o.minKeep = p.value("min_keep").toInt(); o.repetitionPenalty = p.value("repetition_penalty").toDouble();
    o.repetitionContextSize = p.value("repetition_context_size").toInt(); o.presencePenalty = p.value("presence_penalty").toDouble();
    o.frequencyPenalty = p.value("frequency_penalty").toDouble(); o.xtcProbability = p.value("xtc_probability").toDouble();
    o.xtcThreshold = p.value("xtc_threshold").toDouble(); o.logitBias = p.value("logit_bias").toObject();
    for (auto it = o.logitBias.begin(); it != o.logitBias.end(); ++it) {
        bool valid = false; const auto id = it.key().toLongLong(&valid);
        if (!valid || id < 0 || id > std::numeric_limits<qint32>::max() || QString::number(id) != it.key())
            fail("logit_bias keys must be canonical non-negative int32 token IDs");
    }
    return o;
}
GenerationOptions generationOptionsFromParameters(const ParameterObject& object)
{
    QJsonObject mapped;
    const auto values = object.toNativeJson();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto definition = object.definition(it.key());
        if (definition.bindingKey.isEmpty() || definition.nativeBindings.isEmpty())
            throw Error(ErrorCode::RuntimeUnavailable, object.group() + "." + it.key() + " has no generation binding");
        if (mapped.contains(definition.bindingKey)) fail("Conflicting generation bindings: " + definition.bindingKey);
        mapped.insert(definition.bindingKey,it.value());
    }
    return generationOptionsFromJson(mapped);
}
void validateGenerationOptions(const GenerationOptions& options, const QString& runtime)
{
    (void)generationOptionsFromJson(generationOptionsToJson(options));
    if (!runtime.isEmpty() && runtime != "llama.cpp" && runtime != "mlx") fail("Unknown generation runtime: " + runtime);
    if (runtime == "mlx" && options.typicalP != 1)
        throw Error(ErrorCode::RuntimeUnavailable, "MLX does not support typical_p; use 1 to disable it");
}
} // namespace iiLocalLLM
