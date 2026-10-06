# 호스트 정량 입력을 사용하는 초기 의사결정

0.54는 iiDecision 0.0.3 이상의 `ExactDecisionMaker`를 하네스 실행 경계에 연결한다. `EngineOptions.decision.enabled`의 기본값은 true이다. 호스트의 평가가 없으면 `deferred`를 반환하고 모델 생성이나 도구 실행을 시작하지 않는다. 이전 정책으로 실행하려는 호스트는 `decision.enabled=false`를 명시한다. 공개 구조체와 패키지가 변경되므로 소비자는 0.54 헤더·라이브러리로 다시 빌드한다.

## 입력과 계산

호스트는 성공 시 수익 `success_gain`, 실패 시 손실 `failure_loss`, 실행 비용 `cost`, 단위 `value_unit`을 제공한다. 금액은 유한하고 음수가 아니어야 한다. 수익·손실은 성공/실패에 **조건부인 금액**이며 비용은 두 결과에 공통으로 지출되는 금액이다. 이미 확률을 적용한 수익·손실을 다시 입력하지 않는다.

확률 입력은 다음 둘 중 하나이다.

- `success_probability`: 호스트가 평가한 무조건부 성공 확률이다. iiDecision에 success/failure 사전 질량으로 전달한다.
- `states`와 `evidence`: 호스트의 유한 공동 확률모형과 관측 증거이다. iiDecision이 증거에 조건부인 성공·실패 확률을 계산한다. 상태의 outcome은 success/failure이고 log_weight는 자연로그 비정규화 질량이다. `"-inf"`는 0 질량이다. 두 outcome을 모두 포함하며 상태는 최대 4,096개, 관측은 최대 64개이다.

공통 증거를 제공했다면 평가할 모든 후보에 공동 모형을 제공한다. 무조건부 확률에서 증거를 몰래 무시하지 않는다. 0 질량 증거는 `zero_mass_evidence`로 보류한다. 잘못된 단위·금액·중복 ID·누락 outcome·모형·증거는 입력 오류로 거부한다. 평가가 없는 후보는 `missing_host_estimate`로 보류한다.

`EV = P(success | evidence) × success_gain − P(failure | evidence) × failure_loss − cost`이다. 실패 확률은 iiDecision의 별도 결과에서 사용하므로 아주 작은 실패 확률을 `1-p` 연산으로 잃지 않는다. 확률과 금액 계산은 `long double`이며 JSON 반환은 최대 유효 자릿수를 보존한 십진 문자열이다. JSON 숫자 입력은 이미 double로 반올림되므로 고정밀 호스트는 문자열을 사용한다.

기본 기준은 성공 확률 0.65 이상, 순기대가치 1 이상이다. 1은 **호스트가 지정한 단위의 1**이다. 통화 변환·단위 변환은 수행하지 않는다. 호스트는 min_success_probability/min_expected_value를 변경할 수 있다. 기준을 통과한 후보 중 EV가 가장 높은 하나를 선택하며 EV가 같으면 제출 순서를 유지한다. 기준 미달은 큰 예상 수익만으로 우회되지 않는다. 확률의 현실 정확도는 호스트 모형의 품질에 달려 있다. Exact는 유한 모형의 전수 추론이며 경험적 보정이나 미래 수익의 보증이 아니다.

## 실행 경계

1. 입력을 접수하면 SessionStart·사용자 훅·문맥 구성·모델 생성보다 먼저 run 후보를 평가한다. 준비된 입력이 바뀌면 재평가한다. 큐 입력 실행은 입력을 전달한 후, 모델·메모리 사전 검색 전에 평가한다.
2. 각 후속 모델 턴 전에 run 후보를 다시 평가한다. 호스트는 변경된 증거·비용·가치를 반영할 수 있다.
3. 모델 반환에 도구가 있으면 `tool_batch`에서 실제 ID/name/arguments를 평가한다. 가장 높은 EV인 실행 후보 하나를 선택하고 나머지 호출은 `not_executed:true` 반환으로 닫는다. 하네스가 도구의 서로 독립적인 작업 여부를 자동 추론하는 계약은 아니다. 병렬 작업은 별도 자식 Agent나 호스트의 명시적 작업 구성으로 나눈다.
4. ToolRunner는 최초 입력 검증 뒤, 도구 훅보다 먼저 평가한다. 훅 적용·입력 재검증 뒤 실제 인자를 다시 확인한다. 선택된 배치의 같은 session/run/ID/name/arguments이면 호스트 전용 receipt를 재사용한다. 훅이나 권한 응답이 인자를 바꾸면 receipt가 무효가 되어 재평가한다. 검증·준비·권한 검사는 계속 적용된다.
5. 초기 판단·도구 배치·수정 인자 판단이 보류되면 `RunStatus::Deferred`, JSON `status:"deferred"`, error_code:none 및 decision 보고서를 반환한다. 선택에서 제외한 호출은 실행하지 않으며 결과만 기록한다. 자동 재시도하지 않는다. 호스트가 평가를 바꾸고 새 실행을 시작한다.

같은 새 자식 요청의 재사용 계약은 [Procedures.md](Procedures.md)를 따른다. 자식·팀·검증용 Engine은 호스트 DecisionOptions를 상속하며 실제 자식 agent_id와 후보를 콜백에 전달한다. 취소·조회·절차 응답 같은 호스트 제어 경로는 계속 사용할 수 있다. 네이티브 AgentStop/Output/List/Profiles, 셸 TaskStop/Output/List, TeamStop/Status/Inbox/Wait, 검증 결과 반환은 경제적 실행 후보에서 제외한다. MCP 제어 래퍼는 실행을 담당하는 내부 Engine/ToolRunner에서 실제 작업을 평가한다.

직접 `Service::generate/converse`, Engine을 바인딩하지 않은 독립 ToolRunner, 별도 명시적 메모리 유지 관리 API는 이 정책의 적용 대상이 아니다. 자동 초기 메모리 사전 검색은 run 판단 이후 시작한다. 이 정책은 iiLocalLLM 전체의 호출 금지 장치나 OS 권한 정책을 대체하지 않는다.

## 절차 반환값 개입

`decision_input`의 input은 stage, goal, candidates 및 실제 정책 임계값이다. stage는 run/tool_batch/tool이다. 후보 ID·인자·임계값은 하네스가 고정한다. 호스트는 output의 value_unit/estimates/evidence를 교체한다. 후보 ID를 변경하거나 확률 보고서를 제출하여 추론을 건너뛸 수 없다.

`decision`의 output은 `iisacc.decision/1`, action:execute/defer, selected_candidate_id, value_unit, reason, assessments이다. 각 후보에 evaluated, eligible, reason, success_probability, expected_gain/loss, cost, expected_value와 iiDecision 추론 진단을 반환한다. 평가하지 않은 확률은 null이다. reason은 missing_host_estimate/zero_mass_evidence/low_probability/low_value/eligible/lower_expected_value이다. 최상위 reason은 highest_expected_value/no_eligible_action/missing_host_estimate이다. 이 반환은 관측·진행·취소할 수 있으며 교체할 수 없다. 호스트는 앞선 decision_input에서 사실을 바꾼다.

IPC/HTTP는 기존 `agent.procedures.list/respond`, MCP는 `iisacc/procedures/list/respond`를 사용한다. API는 인증 클라이언트마다 채널을 나누고 MCP는 연결의 소유 세션에 묶는다. 모델 arguments와 wire run params에는 금액·정책 권한을 추가하지 않는다. `ProcedureOptions.intercept={"decision_input"}`이면 평가 입력 반환에서 대기한다. `{ "action":"replace", "output": { ... } }` 응답은 호스트 사실만 검증하여 다음 iiDecision 단계에 전달한다.

## 호스트 설정 예

```cpp
agent::EngineOptions options;
options.decision.valueUnit = "USD";
options.decision.minSuccessProbability = 0.7L;
options.decision.minExpectedValue = 2;
options.decision.inputs = [](const agent::DecisionRequest& request) {
    agent::DecisionInput input{"USD"};
    for (const auto& candidate : request.candidates) {
        // 실제 호스트의 수익/손실 평가 저장소에서 가져온다.
        const auto facts = hostAssessment(candidate, request.goal);
        input.estimates.push_back({candidate.id, facts.gain, facts.loss,
            facts.cost, facts.successProbability});
    }
    return input;
};
```

고정 profiles는 candidate_id에 실제 도구 이름 또는 run을 지정한다. 콜백은 후보별 실제 ID를 사용하며 goal·인자·세션에 따라 평가를 달리할 수 있다. 고정 프로필이 모든 인자에 동일한 숫자를 적용한다는 점을 고려하여 앱의 실제 작업에는 동적 콜백이나 반환 채널을 사용한다.

```json
{
  "enabled": true,
  "value_unit": "USD",
  "min_success_probability": "0.65",
  "min_expected_value": "1",
  "profiles": [
    {"candidate_id":"run","success_probability":"0.9","success_gain":"100","failure_loss":"10","cost":"5"},
    {"candidate_id":"Read","success_probability":"0.95","success_gain":"10","failure_loss":"1","cost":"1"}
  ]
}
```

daemon은 `--agent-decision FILE`, agent-enabled MCP는 `--decision FILE`이다. 파일은 agent workspace 밖의 private host 설정이다. 미지정 시 정책은 활성화되고 평가가 없어 실행을 보류한다. 절차 반환으로 입력할 호스트는 별도 `--agent-procedures`/`--procedures` 설정에서 decision_input을 대기 경계로 선택한다. 정책을 명시적으로 끌 때에는 `{ "enabled":false }`를 사용한다.

판단 코어 `DecisionGate.h/.cpp`는 표준 C++23과 iiDecision만 사용한다. 기존 Qt JSON ABI와 전송 연결은 DecisionProtocol에서 처리한다. 현재 판단은 CpuOnly 증거 일치와 호스트 확률 계산으로 수행한다. 모델 추론의 GPU/Metal 사용과 iiDecision의 실제 실행 장치를 구분한다.
