#include "DecisionGate.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <limits>

namespace iiLocalLLM::agent {
namespace {
void require(bool ok,const char* text){if(!ok)throw std::invalid_argument(text);}
void money(long double value){require(std::isfinite(value)&&value>=0,"Host gain, loss and cost must be finite and nonnegative");}
}
DecisionGate::DecisionGate(DecisionOptions options):options_(std::move(options)) {
    require(std::isfinite(options_.minSuccessProbability)&&options_.minSuccessProbability>=0&&options_.minSuccessProbability<=1,"Invalid minimum success probability");
    require(std::isfinite(options_.minExpectedValue)&&options_.minExpectedValue>=0,"Invalid minimum expected value");
    std::set<std::string> names;
    for(const auto& profile:options_.profiles)require(!profile.candidateId.empty()&&names.insert(profile.candidateId).second,"Duplicate or empty decision profile");
    require(options_.modelRoutes.size()<=64,"Too many model routes");
    names.clear();
    for(const auto& route:options_.modelRoutes) {
        require(!route.id.empty()&&route.id.size()<=64&&names.insert(route.id).second,"Duplicate or invalid model route ID");
        require(std::all_of(route.id.begin(),route.id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';}),"Invalid model route ID syntax");
        require(route.model.starts_with("model://")&&route.model.size()>8,"Model route requires a model:// URI");
        require(route.model.size()<=136&&std::all_of(route.model.begin()+8,route.model.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';}),"Invalid model route URI syntax");
        require((route.inputModalities&~allModalities)==0&&supportsModalities(route.inputModalities,modality(Modality::Text)),"Invalid route modalities");
        require(!(route.inputModalities&modality(Modality::Video))||supportsModalities(route.inputModalities,modality(Modality::Image)),"Video routes require image support");
    }
}
std::vector<DecisionCandidate> DecisionGate::modelCandidates(Modalities required)const {
    require(required!=0&&(required&~allModalities)==0,"Invalid required modalities");
    std::vector<DecisionCandidate> result;
    for(const auto& route:options_.modelRoutes)if(supportsModalities(route.inputModalities,required))
        result.push_back({route.id,"model:"+route.id,route.model,"{\"required_modalities\":"+std::to_string(required)+"}"});
    return result;
}
DecisionInput DecisionGate::inputs(const DecisionRequest& request)const {
    if(options_.inputs)return options_.inputs(request);
    DecisionInput input;input.valueUnit=options_.valueUnit;
    for(const auto& candidate:request.candidates)for(const auto& profile:options_.profiles)if(profile.candidateId==candidate.name){
        auto copy=profile;copy.candidateId=candidate.id;input.estimates.push_back(std::move(copy));break;
    }
    return input;
}
DecisionReport DecisionGate::evaluate(std::span<const DecisionCandidate> candidates,const DecisionInput& input)const {
    require(!candidates.empty()&&candidates.size()<=64,"Decision requires 1 to 64 candidates");
    require(input.estimates.size()<=candidates.size(),"Too many decision estimates");
    std::set<std::string> ids,estimated;
    for(const auto& c:candidates)require(!c.id.empty()&&ids.insert(c.id).second,"Candidate IDs must be nonempty and unique");
    for(const auto& e:input.estimates)require(ids.contains(e.candidateId)&&estimated.insert(e.candidateId).second,"Unknown or duplicate candidate estimate");
    if(!input.estimates.empty())require(!input.valueUnit.empty()&&input.valueUnit.size()<=64,"Host value unit is required");
    if(!options_.valueUnit.empty()&&!input.estimates.empty())require(input.valueUnit==options_.valueUnit,"Host value unit changed");
    DecisionReport report;report.valueUnit=input.valueUnit;report.reason="no_eligible_action";
    long double best=-std::numeric_limits<long double>::infinity();
    for(const auto& c:candidates) {
        DecisionAssessment a;a.candidateId=c.id;
        const auto found=std::find_if(input.estimates.begin(),input.estimates.end(),[&](const auto& e){return e.candidateId==c.id;});
        if(found==input.estimates.end()){a.reason="missing_host_estimate";report.assessments.push_back(std::move(a));continue;}
        const auto& e=*found;money(e.successGain);money(e.failureLoss);money(e.cost);
        require(e.successProbability.has_value()!=!e.states.empty(),"Supply either success probability or a joint model");
        auto states=e.states;
        if(e.successProbability) {
            require(std::isfinite(*e.successProbability)&&*e.successProbability>=0&&*e.successProbability<=1,"Invalid host success probability");
            // A host probability is an unconditional prior. Evidence belongs to
            // a supplied joint model and must never be silently discarded.
            require(input.evidence.observations.empty(),"Evidence requires a joint model for every estimated candidate");
            const auto p=*e.successProbability;
            states={{"success",{},p==0?-std::numeric_limits<long double>::infinity():std::log(p)},
                {"failure",{},p==1?-std::numeric_limits<long double>::infinity():std::log1p(-p)}};
        }
        require(states.size()<=4096,"Joint model exceeds 4096 states");
        for(const auto& state:states)require(state.candidate_id=="success"||state.candidate_id=="failure","Joint outcome must be success or failure");
        const std::array outcomes{iiDecision::Candidate{"success"},iiDecision::Candidate{"failure"}};
        iiDecision::AccelerationOptions cpu;cpu.mode=iiDecision::ExecutionMode::CpuOnly;
        iiDecision::ExactDecisionMaker maker(std::move(states),std::make_shared<iiDecision::EvidenceMatcher>(cpu));
        try{a.inference=maker.evaluate(input.evidence,outcomes);}
        catch(const std::domain_error&){a.reason="zero_mass_evidence";report.assessments.push_back(std::move(a));continue;}
        a.evaluated=true;
        a.successProbability=a.inference.probabilities[0].probability;
        a.expectedGain=a.successProbability*e.successGain;a.expectedLoss=a.inference.probabilities[1].probability*e.failureLoss;a.cost=e.cost;
        a.expectedValue=a.expectedGain-a.expectedLoss-a.cost;
        require(std::isfinite(a.expectedValue),"Expected value overflow");
        if(a.successProbability<options_.minSuccessProbability)a.reason="low_probability";
        else if(a.expectedValue<options_.minExpectedValue)a.reason="low_value";
        else {a.eligible=true;a.reason="eligible";if(a.expectedValue>best){best=a.expectedValue;report.selectedCandidateId=c.id;}}
        report.assessments.push_back(std::move(a));
    }
    if(report.execute())report.reason="highest_expected_value";
    else if(estimated.empty())report.reason="missing_host_estimate";
    for(auto& a:report.assessments)if(a.eligible&&a.candidateId!=report.selectedCandidateId)a.reason="lower_expected_value";
    return report;
}
}
