#include "DecisionProtocol.h"
#include "Procedures.h"
#include <QtCore/QJsonDocument>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace iiLocalLLM::agent {
namespace {
void require(bool ok,const QString& text){if(!ok)throw Error(ErrorCode::InvalidArgument,text);}
void fields(const QJsonObject& object,const QStringList& allowed){for(auto i=object.begin();i!=object.end();++i)require(allowed.contains(i.key()),"Unknown decision field: "+i.key());}
QString decimal(long double value){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(std::numeric_limits<long double>::max_digits10)<<value;return QString::fromStdString(out.str());}
long double number(const QJsonValue& value,bool log=false){
    if(log&&value.isString()&&value.toString()=="-inf")return -std::numeric_limits<long double>::infinity();
    long double result;
    if(value.isDouble())result=value.toDouble();
    else {require(value.isString()&&value.toString().size()<=128,"Decision quantity must be a number or decimal string");
        std::istringstream in(value.toString().toStdString());in.imbue(std::locale::classic());in>>result;require(bool(in),"Invalid decision quantity");in>>std::ws;require(in.eof(),"Invalid decision quantity suffix");}
    require(std::isfinite(result),"Decision quantities must be finite");return result;
}
QJsonArray observations(const iiDecision::Context& context){QJsonArray a;for(const auto& o:context.observations)a.append(QJsonObject{{"key",QString::fromStdString(o.key)},{"value",QString::fromStdString(o.value)}});return a;}
iiDecision::Context evidence(const QJsonValue& value){
    require(value.isArray()&&value.toArray().size()<=64,"Evidence must contain at most 64 observations");iiDecision::Context c;
    for(const auto& entry:value.toArray()){require(entry.isObject(),"Evidence observation must be an object");auto o=entry.toObject();fields(o,{"key","value"});
        require(o["key"].isString()&&o["value"].isString()&&!o["key"].toString().isEmpty()&&o["key"].toString().size()<=1024&&o["value"].toString().size()<=4096,"Invalid evidence observation");
        c.observations.push_back({o["key"].toString().toStdString(),o["value"].toString().toStdString()});}return c;
}
QJsonObject jsonInput(const DecisionInput& input){
    QJsonArray estimates;
    for(const auto& e:input.estimates){QJsonObject value{{"candidate_id",QString::fromStdString(e.candidateId)},{"success_gain",decimal(e.successGain)},{"failure_loss",decimal(e.failureLoss)},{"cost",decimal(e.cost)}};
        if(e.successProbability)value["success_probability"]=decimal(*e.successProbability);
        if(!e.states.empty()){QJsonArray states;for(const auto& s:e.states)states.append(QJsonObject{{"outcome",QString::fromStdString(s.candidate_id)},{"observations",observations({s.observations})},{"log_weight",decimal(s.log_weight)}});value["states"]=states;}
        estimates.append(value);}
    return {{"value_unit",QString::fromStdString(input.valueUnit)},{"estimates",estimates},{"evidence",observations(input.evidence)}};
}
DecisionInput parseInput(const QJsonObject& o){
    fields(o,{"value_unit","estimates","evidence"});require(o["value_unit"].isString()&&o["estimates"].isArray()&&o["estimates"].toArray().size()<=64,"Invalid decision input");DecisionInput input;input.valueUnit=o["value_unit"].toString().toStdString();
    input.evidence=evidence(o.value("evidence").isUndefined()?QJsonValue(QJsonArray{}):o["evidence"]);
    for(const auto& entry:o["estimates"].toArray()){
        require(entry.isObject(),"Estimate must be an object");auto e=entry.toObject();fields(e,{"candidate_id","success_gain","failure_loss","cost","success_probability","states"});
        require(e["candidate_id"].isString()&&!e["candidate_id"].toString().isEmpty()&&e["candidate_id"].toString().size()<=1024,"Estimate candidate ID is required");
        DecisionEstimate estimate;estimate.candidateId=e["candidate_id"].toString().toStdString();estimate.successGain=number(e["success_gain"]);estimate.failureLoss=number(e["failure_loss"]);estimate.cost=number(e["cost"]);
        if(e.contains("success_probability"))estimate.successProbability=number(e["success_probability"]);
        if(e.contains("states")){require(e["states"].isArray()&&e["states"].toArray().size()<=4096,"Joint states must be a bounded array");
            for(const auto& row:e["states"].toArray()){require(row.isObject(),"Joint state must be an object");auto s=row.toObject();fields(s,{"outcome","observations","log_weight"});require(s["outcome"].isString(),"Joint outcome must be a string");
                estimate.states.push_back({s["outcome"].toString().toStdString(),evidence(s["observations"]).observations,number(s["log_weight"],true)});}}
        input.estimates.push_back(std::move(estimate));
    }return input;
}
}
DecisionOptions decisionOptionsFromJson(const QJsonObject& o){
    fields(o,{"enabled","min_success_probability","min_expected_value","value_unit","profiles"});DecisionOptions options;
    if(o.contains("enabled")){require(o["enabled"].isBool(),"Decision enabled must be boolean");options.enabled=o["enabled"].toBool();}
    if(o.contains("min_success_probability"))options.minSuccessProbability=number(o["min_success_probability"]);
    if(o.contains("min_expected_value"))options.minExpectedValue=number(o["min_expected_value"]);
    if(o.contains("value_unit")){require(o["value_unit"].isString(),"Decision value unit must be a string");options.valueUnit=o["value_unit"].toString().toStdString();}
    if(o.contains("profiles"))options.profiles=parseInput({{"value_unit",QString::fromStdString(options.valueUnit)},{"estimates",o["profiles"]}}).estimates;
    try {DecisionGate validate(options);if(!options.profiles.empty()){
        std::vector<DecisionCandidate> candidates;for(const auto& e:options.profiles)candidates.push_back({e.candidateId});
        validate.evaluate(candidates,{options.valueUnit,options.profiles,{}});
    }}catch(const std::exception& error){throw Error(ErrorCode::InvalidArgument,QString::fromUtf8(error.what()));}return options;
}
QJsonObject toJson(const DecisionReport& r){
    QJsonArray assessments;for(const auto& a:r.assessments){QJsonArray execution;for(const auto& e:a.inference.execution.records)execution.append(QJsonObject{{"backend",QString::fromStdString(e.backend_id)},{"completed",e.completed},{"rows",qint64(e.row_count)}});
        assessments.append(QJsonObject{{"candidate_id",QString::fromStdString(a.candidateId)},{"eligible",a.eligible},{"reason",QString::fromStdString(a.reason)},
            {"success_probability",a.evaluated?QJsonValue(decimal(a.successProbability)):QJsonValue(QJsonValue::Null)},{"evaluated",a.evaluated},{"expected_gain",decimal(a.expectedGain)},{"expected_loss",decimal(a.expectedLoss)},{"cost",decimal(a.cost)},{"expected_value",decimal(a.expectedValue)},
            {"inference",QJsonObject{{"engine","iiDecision.ExactDecisionMaker"},{"normalization_error",decimal(a.inference.normalization_error)},{"underflowed_candidates",qint64(a.inference.underflowed_candidates)},
                {"cpu_rows",qint64(a.inference.execution.cpu_rows)},{"execution",execution}}}});}
    return {{"schema","iisacc.decision/1"},{"action",r.execute()?"execute":"defer"},{"selected_candidate_id",QString::fromStdString(r.selectedCandidateId)},{"value_unit",QString::fromStdString(r.valueUnit)},{"reason",QString::fromStdString(r.reason)},{"assessments",assessments}};
}
namespace detail {
bool decisionControl(const ToolDefinition& t){
    const auto source=t.metadata["source"].toString();
    return (source=="builtin.subagent"&&QStringList{"AgentOutput","AgentStop","AgentList","AgentProfiles"}.contains(t.name))
        ||(source=="builtin.shell.control"&&QStringList{"TaskOutput","TaskStop","ShellTaskList"}.contains(t.name))
        ||(source=="builtin.team"&&QStringList{"TeamStop","TeamStatus","TeamInbox","TeamWait"}.contains(t.name))
        ||(source=="builtin.hook.output"&&t.name=="StructuredOutput")
        // These native MCP wrappers delegate executable work into Engine,
        // where the same policy evaluates the actual run/tool identity.
        ||QStringList{"builtin.agent.control","builtin.session.control","builtin.session.fork","builtin.input.control","builtin.subagent.control","builtin.subagent.run","builtin.team.control","builtin.plan.control","builtin.memory.control","builtin.checkpoint.control","builtin.lsp.control","builtin.worktree.control"}.contains(source);
}
DecisionCandidate decisionCandidate(const ToolCall& call,QString description){return {call.id.toStdString(),call.name.toStdString(),description.toStdString(),QJsonDocument(call.arguments).toJson(QJsonDocument::Compact).toStdString()};}
DecisionReport decide(const DecisionRequest& request,const ToolContext& context,const EventCallback& events){
    context.cancellation.throwIfCancelled();const auto& gate=*context.decisionGate;
    QJsonArray candidates;for(const auto& c:request.candidates)candidates.append(QJsonObject{{"id",QString::fromStdString(c.id)},{"name",QString::fromStdString(c.name)},{"description",QString::fromStdString(c.description)},{"arguments",QString::fromStdString(c.arguments)}});
    QJsonObject immutable{{"stage",QString::fromStdString(request.stage)},{"goal",QString::fromStdString(request.goal)},{"candidates",candidates},
        {"min_success_probability",decimal(gate.options().minSuccessProbability)},{"min_expected_value",decimal(gate.options().minExpectedValue)}};
    ProcedureScope input("decision_input",context,immutable,events);
    DecisionInput supplied;try{supplied=gate.inputs(request);}catch(const std::exception& e){throw Error(ErrorCode::ConsumerFailure,QString::fromUtf8(e.what()));}
    auto output=input.returned(jsonInput(supplied),[owner=context.decisionGate,c=request.candidates](const QJsonObject& o){try{owner->evaluate(c,parseInput(o));}catch(const std::exception& e){throw Error(ErrorCode::InvalidArgument,QString::fromUtf8(e.what()));}});
    context.cancellation.throwIfCancelled();ProcedureScope inference("decision",context,immutable,events);
    DecisionReport report;try{report=gate.evaluate(request.candidates,parseInput(output));}catch(const std::exception& e){throw Error(ErrorCode::InvalidArgument,QString::fromUtf8(e.what()));}
    auto result=toJson(report);result["stage"]=QString::fromStdString(request.stage);inference.returned(result);return report;
}
}
}
