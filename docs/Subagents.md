# 로컬 서브에이전트

0.24.0은 대화 초기화 시 background 자식의 실행·기록·미전달 완료 알림을 새 소유자에게 넘긴다. 소유권 커밋과 알림 큐는 독립 저장이며 재시도/재개 계약은 [SessionClear.md](SessionClear.md)를 따른다.

0.16.0의 `Subagents`는 기존 C++ `Engine` 위에서 자식 대화를 실행한다. 동기 실행, 백그라운드 실행, 부모 컨텍스트 분기, 자식 대화 재개, 도구·모델 범위, 취소·기한, 결과 조회와 완료 알림을 제공한다. 실행기 이름을 등록한 상태와 모델이 실제 작업을 끝낸 상태는 구분한다. 전체 하네스의 subagents 영역은 **partial**이다.

## 호스트와 프로파일

호스트가 `SubagentOptions`의 작업 디렉터리와 전용 상태 디렉터리를 지정한다. 두 디렉터리는 겹칠 수 없다. 자식 JSONL과 결과 기록은 상태 디렉터리에 저장하고 전체 인스턴스는 `agents.lock`으로 소유권을 유지한다. 자식 저장소의 심볼릭 링크와 잘못된 레코드는 거부한다. 부모와 같은 작업 디렉터리에서 실행하며 임의의 cwd 변경이나 git worktree는 아직 지원하지 않는다.

`SubagentDefinition`은 이름·설명·시스템 프롬프트·모델·허용 도구 패턴·금지 도구 패턴·읽기 전용 제한·턴 상한을 정의한다. 프로파일이 비어 있으면 `general-purpose`를 제공한다. C++ 호스트 정의와 파일 기반 정의를 함께 사용할 수 있다. `.claude/agents`와 명시한 사용자·관리·플러그인 디렉터리의 계층, 스킬 사전 로딩, 모델 별칭, 새 실행 시 갱신과 훅 계약은 [AgentProfiles.md](AgentProfiles.md)에 기록한다.

기본 허용 도구는 `*`이며 금지 패턴과 `readOnly` 제한이 추가로 적용된다. 모델에 보이는 정의, ToolSearch가 검색하는 목록, 실제 실행 권한을 모두 제한한다. 부모의 권한 정책과 권한 콜백을 이어받으며 프로파일은 부모의 허용 범위를 넓히지 않는다. 재개 시에는 원래 저장된 프로파일과 현재 호스트 프로파일의 도구 범위를 모두 만족해야 한다. 부모의 `EngineOptions::toolFilter`도 자식에게 적용한다. 지연 MCP 도구 검색을 사용할 프로파일에는 `ToolSearch`를 허용한다.

자식은 새 UUID와 별도의 Engine 실행 풀을 사용하므로 부모의 실행 슬롯을 기다리는 교착을 피한다. 모델·런타임·MCP 앱 도구 구현은 기존 호스트를 공유한다. 도구 스냅샷은 자식 실행을 시작할 때 고정하며, 다음 실행·재개에서는 현재 호스트 레지스트리를 다시 확인한다. 자식에 `Agent`와 서브에이전트 제어 도구를 제공하지 않는다. 자식이 시작한 백그라운드 셸도 자식 실행이 끝날 때 중단하고 종료 상태를 확인한다.

호출 시 모델을 생략하면 프로파일 모델 또는 부모 모델을 사용한다. 명시적 모델 변경은 부모 모델·신뢰된 C++ 호스트 프로파일의 모델·호스트 `allowedModels` 또는 `modelAliases`의 대상이어야 한다. 파일 프로파일의 `model` 자체는 모델 사용 권한을 부여하지 않는다. 모델 별칭 `sonnet`/`opus`/`haiku`를 임의로 로컬 모델에 매핑하지 않는다. 실제 로컬 모델 URI와 기존 Service의 로딩·메모리 정책을 사용한다. 생성 설정은 호스트의 `SubagentOptions::generation`으로 지정한다.

## 실행·분기·재개

`Agent`는 `prompt`를 필수로 받는다. `description`, `subagent_type`, `model`, `run_in_background`, `fork_context`, `max_turns`, `resume`는 선택적이다. 일반 실행은 새 대화를 만들며 선택한 프로파일의 시스템 프롬프트를 사용한다. 동기 호출의 `status=completed`에는 이미 최종 답변이 들어 있으므로 완료된 호출을 계속 조회할 필요가 없다.

`fork_context=true`이면 도구 묶음 시작 시점의 부모 스냅샷을 복사한다. 부모의 원본 메시지·압축 경계·시스템 프롬프트를 유지하고, 미완료 부모 도구 호출에는 아직 결과를 모른다는 표시를 넣어 자식의 대화 형식을 완성한다. 부모 기록을 수정하거나 부모 도구를 재실행하지 않는다. 자식 요청에는 부모와 자식의 역할을 구분하는 지침을 덧붙인다. 부모 artifacts가 있으면 아직 복사할 수 없어 명시적으로 실패한다. 기본 분기는 같은 작업 디렉터리를 공유하므로 파일 변경도 공유한다.

`resume=agent-ID`는 같은 부모가 소유한 자식 대화에서 새 요청을 실행한다. 자식의 기존 모델·시스템 프롬프트·본문·관측은 유지한다. 같은 자식의 동시 재개, 다른 부모의 ID 접근, 재개와 프로파일·모델·fork 변경의 조합은 거부한다. 각 실행의 권한은 현재 호스트에서도 다시 확인한다. 응답을 잃은 뒤 같은 실행 요청을 다시 전송하는 경우의 등록 중복 방지는 아직 제공하지 않는다.

턴 상한과 분기 지침을 합친 입력 길이는 자식 대화를 저장하기 전에 검증한다. 재개 수락 레코드의 저장이 실패하면 메모리의 기존 결과도 유지한다. 자식 종료 시 셸 이력을 페이지별로 확인하며 조회와 중단 사이 자연 종료된 셸은 최종 상태를 다시 확인한다.

`AgentOutput`은 `agent_id`, 선택적 `block`, `timeout_ms`를 받는다. `finished=true`와 `retrieval_status=success`는 해당 실행의 결과가 확정됐음을 나타낸다. 조회 자체의 성공과 자식의 `status=failed/cancelled/turn_limit/interrupted`는 별개의 값이다. 모델에 반환하는 결과에는 프로파일 본문이나 원래 요청을 반복하지 않는다. C++ `Subagents::output()`은 원래 레코드도 제공한다.

`AgentStop`은 실행 중인 자식의 취소를 요청하며, `AgentList`는 같은 부모의 자식 상태를 반환한다. C++ 소유자의 `close()`·소멸은 수락한 자식을 취소하고 실행 풀의 종료를 기다린다. 동기 실행은 부모 취소를 따르며 이벤트 콜백의 수명이 끝나기 전에 자식이 종료되도록 기다린다. 백그라운드 실행은 수락 후 호출자의 취소와 수명을 분리한다. MCP 연결 종료와 새 대화 전환은 이전 대화의 자식에게 중단을 요청한다.

기한과 취소는 모델·도구 구현의 협력을 요구한다. 이미 발생한 파일·앱 변경은 되돌리지 않는다. 정상 종료 기록 없이 이전 호스트가 끝나면 다음 소유자는 `interrupted`로 복구하며 작업을 자동 반복하지 않는다. 기본 상한은 동시 자식 4개, 기록 1,024개, 실행당 32턴, 실행 시간 300,000ms이다. 프로파일 턴 상한도 함께 적용된다. 이력 정리·삭제와 전체 프로세스 격리는 별도 미완료 항목이다.

백그라운드 완료는 부모 `InputQueue`의 `notification/later`로 전달한다. 부모가 실행 중이면 기존 입력 경계에서 전달하고, 유휴 대화는 명시적 `agent.inputs.run`으로 처리한다. 현재 유휴 자동 기동은 없다. 큐가 가득 차거나 기록 저장이 실패하면 `delivery_error`로 구분한다. 완료 알림과 기록 저장 전체가 하나의 원자적 트랜잭션은 아니므로 강제 종료 경계에서의 정확히 한 번 전달은 보장하지 않는다.

## C++·API·MCP·CLI

```cpp
auto model = std::make_shared<agent::ServiceModel>(service);
auto policy = std::make_shared<agent::RulePolicy>(agent::PermissionMode::Default,
    QList<agent::PermissionRule>{{"Agent", agent::PermissionBehavior::Allow},
                                {"AgentStop", agent::PermissionBehavior::Allow}});
agent::EngineOptions engineOptions;
engineOptions.sessionsDirectory = "/data/private/parents";
agent::SubagentOptions options;
options.workingDirectory = "/data/workspace";
options.stateDirectory = "/data/private/children";
auto children = std::make_shared<agent::Subagents>(model, registry, policy, engineOptions, options);
agent::Subagents::attach(engineOptions, children);
agent::Engine engine(model, registry, policy, engineOptions);
auto parent = engine.createSession("model://local-model", options.workingDirectory);
auto result = engine.runSubagentTool(parent.id, "Agent", {{"prompt", "Read input.txt and report its contents."}});
```

`EngineOptions::additionalTools`는 기존 레지스트리의 변경을 유지하면서 각 모델 턴에 호스트 도구를 합친다. `toolFilter`는 도구 검색 전후에 적용한다. 추가 도구 이름 충돌은 실패한다. `ToolContext::sessionSnapshot`은 해당 도구 묶음의 불변 부모 스냅샷이다. 구조체 레이아웃이 바뀌어 ABI는 **0.18**이며 공개 헤더와 라이브러리를 함께 갱신해 소비자를 다시 빌드한다.

인증 API는 `agent.agents.run`, `.output`, `.stop`, `.list`, `.profiles`를 제공한다. 모두 부모 `session_id`를 받으며 다른 필드는 대응 도구와 같다. 응답은 기존 `{text, result, is_error}` 계약을 사용한다. C++ `ApiOptions::subagentsEnabled`는 기본 false이고 독립 데몬은 에이전트 API 설정 시 기본으로 켠다. `--agent-no-subagents`로 끌 수 있다. 자식 상태 디렉터리는 인증된 클라이언트마다 분리하고 외부 요청에서 지정하지 못하게 한다. 실행 중 조회·중단은 별도 제어 작업 풀을 사용한다.

데몬의 `--agent-subagent-options FILE`은 호스트가 자식의 `GenerationOptions`를 지정하는 경로이다. 예를 들어 사용자 소유의 비공개 JSON 파일에 `{"temperature":0,"max_tokens":512}`를 저장한다. 기존 생성 파라미터 검증기를 재사용하며 잘못된 JSON·타입·범위·알 수 없는 필드는 런타임 초기화 전에 거부한다. 이 파일을 생략하면 기존 `GenerationOptions` 기본값(temperature 0.7, max_tokens 256)을 사용한다. 부모의 요청별 생성 옵션은 자식의 호스트 설정을 덮어쓰지 않는다.

데몬에서 자식 실행을 허용하려면 `--agent-allow Agent`, 사용자 요청에 따른 중단을 허용하려면 `--agent-allow AgentStop`을 지정한다. 자식이 사용하는 파일·셸·앱 도구에는 기존 도구별 권한이 그대로 적용된다.

MCP는 `iiLocalLLM.agent.agents.run`, `.output`, `.stop`, `.list`, `.profiles`로 공개한다. 부모 대화는 연결마다 분리된다. `iillm-mcp --model ... --models ... --state DIR`로 작업 디렉터리 외부의 전용 상태 디렉터리를 지정하면 서브에이전트를 켠다. stdio에도 `--state`를 사용할 수 있으며 HTTP는 기존처럼 자격증명과 전용 상태가 필수이다. `--no-subagents`로 끌 수 있다. `--allow Agent`, 필요한 경우 `--allow AgentStop`은 해당 네이티브 도구만 허용하며 자식의 파일·셸·앱 도구 권한은 기존 규칙을 따른다.

```sh
iillm --auth-file /data/private/token agent agents run PARENT_SESSION request.json
iillm --auth-file /data/private/token agent agents list PARENT_SESSION
iillm --auth-file /data/private/token agent agents output PARENT_SESSION output.json
```

`request.json` 예시는 `{"prompt":"Read input.txt and report its contents.","run_in_background":true}`이고 `output.json`은 `{"agent_id":"agent-UUID","block":true,"timeout_ms":30000}`이다. CLI는 IPC만 사용하며 모델이나 서브에이전트 실행기를 링크하지 않는다.

## 참조 범위와 남은 항목

분석 기준은 `c8cd253554319f32ff64ff7000636199f720c9bc`의 [AgentTool.tsx](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/AgentTool/AgentTool.tsx), [runAgent.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/AgentTool/runAgent.ts), [forkSubagent.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/AgentTool/forkSubagent.ts), [resumeAgent.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/AgentTool/resumeAgent.ts)이다. TypeScript나 번들 에이전트 프롬프트를 SDK에 포함하지 않는다.

참조의 조건부 implicit fork는 타입 생략 시 부모 프롬프트·컨텍스트를 이어받고 백그라운드로 강제 전환한다. iiLocalLLM은 `fork_context`와 `run_in_background`를 명시적으로 구분한다. 참조의 재개 실행 경로와 iiLocalLLM의 `Agent.resume` 필드도 동일한 wire schema라고 주장하지 않는다.

내장 전문 역할 전체와 조건부 선택, 에이전트 파일의 외부 훅 실행, 추가 MCP 서버·메모리, 실행 도중 자동 백그라운드 전환, 팀·SendMessage·mailbox, worktree·remote 격리, 공통 TaskOutput/TaskStop으로의 통합, 전체 trace·비용 집계와 광범위한 실제 앱 작업 검증은 남아 있다. 실제 모델 결과와 실패 기록은 [Verification.md](Verification.md)에 분리해 기록한다.

0.17.0은 `Subagents::runSkill`과 `attach`의 fork 콜백으로 스킬을 별도 자식에서 실행한다. `Agent.fork_context`는 부모 대화 복사이고 스킬 `context: fork`는 부모 이력 없이 스킬 본문을 실행하는 경로이다. 구체적인 결과·제한은 [Skills.md](Skills.md)를 따른다.

호출 시 부모의 현재 도구 권한을 자식에 전달한다. 사전 로딩된 스킬의 본문은 권한을 추가하지 않으며 재개는 현재 호출자의 권한을 따른다. [권한 수명](Permissions.md)을 참조한다.
