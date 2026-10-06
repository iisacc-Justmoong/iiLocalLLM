#pragma once
#include "../Export.h"
#include <iiDecision/ExactDecisionMaker.h>
#include <functional>
#include <optional>
#include <span>

namespace iiLocalLLM::agent {
struct DecisionCandidate {
    std::string id, name, description, arguments;
};
struct DecisionEstimate {
    std::string candidateId;
    long double successGain=0, failureLoss=0, cost=0;
    std::optional<long double> successProbability;
    // Optional finite joint model with outcome IDs "success" and "failure".
    // Use this instead of successProbability for evidence-conditioned inference.
    std::vector<iiDecision::JointState> states;
};
struct DecisionInput {
    std::string valueUnit;
    std::vector<DecisionEstimate> estimates;
    iiDecision::Context evidence;
};
struct DecisionRequest {
    std::string stage, sessionId, runId, agentId, goal, workingDirectory;
    std::vector<DecisionCandidate> candidates;
};
struct DecisionOptions {
    bool enabled=true; // Missing host estimates defer; they are never invented by the model.
    long double minSuccessProbability=0.65L;
    long double minExpectedValue=1; // In the host's declared value unit.
    std::string valueUnit;
    // Profiles match "run" or the actual tool name. The callback may instead
    // supply argument- and context-specific estimates for the candidate IDs.
    std::vector<DecisionEstimate> profiles;
    std::function<DecisionInput(const DecisionRequest&)> inputs;
};
struct DecisionAssessment {
    std::string candidateId, reason;
    bool eligible=false;
    bool evaluated=false;
    long double successProbability=0, expectedGain=0, expectedLoss=0, cost=0, expectedValue=0;
    iiDecision::ProbabilityReport inference;
};
struct DecisionReport {
    std::string selectedCandidateId, valueUnit, reason;
    std::vector<DecisionAssessment> assessments;
    bool execute()const noexcept{return !selectedCandidateId.empty();}
};
// Pure C++23 probability/value policy. No GUI, transport or Qt dependency.
class IILOCALLLM_EXPORT DecisionGate final {
public:
    explicit DecisionGate(DecisionOptions = {});
    DecisionInput inputs(const DecisionRequest&) const;
    DecisionReport evaluate(std::span<const DecisionCandidate>,const DecisionInput&) const;
    const DecisionOptions& options()const noexcept{return options_;}
private:
    DecisionOptions options_;
};
struct DecisionReceipt {
    std::string sessionId,runId;
    DecisionCandidate candidate;
    DecisionReport report;
};
}
