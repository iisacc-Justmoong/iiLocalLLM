# 로컬 스킬

0.17.0은 C++에서 `SKILL.md` 탐색, 메타데이터 목록, 인자 치환, 인라인 대화 주입과 `context: fork` 자식 실행을 제공한다. 본문에서 요구한 읽기·수정·명령 실행은 모델의 도구 호출과 기존 호스트 권한 정책을 거친다. 공개 구조체가 바뀌어 SOVERSION은 **0.17**이며 C++ 소비자는 다시 빌드해야 한다.

## 파일과 탐색

기본 경로는 세션 작업 디렉터리의 `.claude/skills/<name>/SKILL.md`이다. `SkillOptions::directories`에 지정한 디렉터리들을 나열 순서대로 먼저 조사하고 기본 경로를 마지막에 조사한다. 추가 경로는 `<name>/SKILL.md`를 직접 포함하는 디렉터리이다. 상대 경로는 세션 작업 디렉터리를 기준으로 해석한다. 사용자 홈·조상 디렉터리·네트워크를 자동 조사하지 않는다. 호스트가 명시한 추가 디렉터리는 작업 디렉터리 외부에도 둘 수 있으나 파일은 각 스킬 루트의 경계를 지켜야 한다. 경로 허용은 해당 경로에 대한 `Read`/`Bash` 등 도구의 접근 권한을 넓히지 않는다.

각 디렉터리의 바로 아래 항목만 이름순으로 확인한다. 이름은 영문자·숫자·밑줄로 시작하는 최대 128자의 영문자·숫자·밑줄·마침표·하이픈이다. 같은 이름이나 같은 canonical 경로는 먼저 찾은 항목이 우선이며 뒤의 항목은 `shadowed` 목록에 기록한다. 일반 `.md` 파일, 숨겨진 홈 설정, 플러그인 디렉터리는 자동 스킬로 취급하지 않는다. 목록과 모델 턴마다 새 스냅샷을 읽으므로 파일 추가·변경·삭제가 다음 탐색에 반영된다.

```markdown
---
name: File inspector
description: Read a file and report the exact observed value.
arguments: [filename]
argument-hint: '[filename]'
---
Use Read to open $filename, then report its exact contents.
```

호출 이름은 폴더 이름이며 `name`은 표시 이름이다. `description`, `argument-hint`, `arguments`, `when_to_use`, `version`, `disable-model-invocation`, `user-invocable`를 해석한다. 설명이 없으면 첫 비어 있지 않은 본문 줄에서 최대 512자를 사용한다. `license`, `compatibility`, `metadata`는 설명 정보이며 실행 동작을 바꾸지 않는다. `context: inline`이 기본이다. `context: fork`와 그때의 `agent`·`model`은 아래 별도 자식 실행 계약을 따르며, `model: inherit`과 빈 `allowed-tools: []`도 허용한다.

`disable-model-invocation: true`는 모델 목록과 `Skill` 호출을 차단하지만 직접 사용자 호출은 허용한다. `user-invocable: false`는 직접 호출을 차단하지만 모델 호출을 허용한다. 불리언 값은 true/false만 허용한다. BOM·CRLF와 구분자 뒤 공백·탭을 지원한다. YAML은 기존 libyaml 0.2.5를 재사용하며 신규 생산 의존성이나 Python 실행기를 추가하지 않았다. 별칭, 중복 최상위 키, 여러 YAML 문서, 잘못된 필드 형식, 비정상 UTF-8, NUL, 빈 본문은 명시적으로 실패한다. 현재는 하나의 잘못된 활성 스킬 파일이 전체 탐색을 실패시킬 수 있다.

## 인라인 실행과 복원

모델에는 호출 가능한 스킬의 이름·설명·인자 힌트·실행 컨텍스트를 임시 사용자 컨텍스트로 제공한다. 전체 본문은 호출 시 읽고 치환하여 사용자 역할 메시지로 주입한다. 시스템 프롬프트는 변경하지 않는다. 스킬 리소스 디렉터리와 세션 작업 디렉터리를 구분해 표시하며, 도구의 상대 경로 기준은 세션 작업 디렉터리로 유지한다. 각 호출의 경로·SHA-256·인자·호출 주체를 메시지 메타데이터에 저장한다. 주입된 본문에는 이미 로딩된 호출임을 표시한다. 모델에 전달하는 도구 결과 데이터는 success·status·commandName으로 제한하고 전체 메타데이터를 반복하지 않는다. 모델의 `Skill` 호출 결과와 같은 턴의 모든 도구 결과가 짝을 이룬 후 본문을 주입한다. 저장 직후 관찰자 오류나 프로세스 중단이 발생한 경우, 커밋된 도구 결과에서 누락된 본문을 한 번 복원한다. 복원을 위해 파일을 다시 읽거나 도구를 재실행하지 않는다.

세션 재개·분기는 원래의 본문을 유지한다. 이후 스킬 파일이 바뀌거나 삭제되어도 이전 실행 내용은 바뀌지 않는다. 압축은 일반 대화와 같은 원본 보존·요약 규칙을 적용하므로 본문이 영구적으로 모델 컨텍스트에 고정되는 것은 아니다. 이 버전에는 참조 구현의 활성 스킬 전용 재주입 정책이 없다.

`$ARGUMENTS`, `$ARGUMENTS[0]`, `$0`, frontmatter에 선언한 `$filename`, `${CLAUDE_SKILL_DIR}`, `${CLAUDE_SESSION_ID}`를 지원한다. 작은따옴표·큰따옴표로 묶은 인자는 하나의 값이며 작은따옴표 외부의 역슬래시는 다음 문자를 이스케이프한다. 셸 변수·명령·연산자는 실행하거나 확장하지 않는다. 닫히지 않은 따옴표·이스케이프는 실패한다. 존재하지 않는 위치 인자는 빈 문자열이고, 인자 자리표시자가 없으면 비어 있지 않은 전체 인자를 `ARGUMENTS:`로 덧붙인다. 치환은 원문을 한 번만 순회하여 인자 안의 `$ARGUMENTS` 등을 다시 치환하지 않는다. 이는 참조의 shell-quote·단계별 재치환·오류 시 공백 분할과 구별되는 계약이다.

```cpp
agent::EngineOptions options;
options.sessionsDirectory = "/private/agent-state/sessions";
options.skills.directories = {"/explicit/shared-skills"};
agent::Engine engine(model, tools, policy, options);
auto session = engine.createSession("model://local-model", "/workspace/project");
auto catalog = engine.skills(session.id).toJson();
agent::RunRequest request{session.id};
request.skill = "inspect";
request.skillArguments = "'file with spaces.txt'";
auto run = engine.run(request);
```

직접 호출은 `skill`과 `skill_arguments`를 지정한다. 선택적인 `prompt`를 함께 주면 본문 뒤에 추가 사용자 요청으로 저장한다. 일반 프롬프트의 `/inspect` 문자열은 자동 슬래시 명령으로 해석하지 않는다. 입력 큐는 아직 직접 스킬 호출 필드를 받지 않는다. `SkillOptions::enabled = false`이면 새 목록과 호출이 비활성화된다. 이미 저장된 대화 내용은 지우지 않는다.

## API·MCP·CLI

인증 API `agent.skills.list`는 `session_id`를 받아 메타데이터와 `shadowed`를 반환한다. `agent.run`은 `skill`, `skill_arguments`를 선택적으로 받으며 `skill`이 있으면 `prompt`를 생략할 수 있다. 다른 앱의 세션 ID는 기존 정책대로 404이다. `agent.info.skills_enabled`로 호스트 설정을 확인한다. 목록 조회에는 모델 로딩이 필요 없다.

MCP에서는 엔진을 연결한 서버가 `iiLocalLLM.agent.skills.list`와 `iiLocalLLM.agent.run`의 동일한 스킬 인자를 제공한다. 연결마다 다른 대화를 사용한다. `iillm-mcp`의 `--skills-dir DIR`, `--no-skills`와 데몬의 `--agent-skills-dir DIR`, `--agent-no-skills`로 호스트가 탐색을 제어한다. 이 버전은 MCP 서버에서 내려온 원격 스킬을 소비하거나 `prompts/list`로 로컬 스킬을 공개하지 않는다.

```sh
iillm --auth-file /private/token agent skills list SESSION_ID
iillm --auth-file /private/token agent skills run SESSION_ID skill-request.json
```

`skill-request.json`은 `{"skill":"inspect","skill_arguments":"input.txt"}` 형식이다. 실행 실패 시 위 CLI 명령은 종료 코드 1을 반환한다. `iillm`은 기존의 얇은 IPC 클라이언트이며 로컬 파일 탐색·추론은 데몬에서만 실행된다.

## 제한과 참조 범위

기본 제한은 파일당 128 KiB, 읽은 파일 합계 512 KiB, 스킬 128개, 조사 항목 4,096개, 추가 디렉터리 64개이다. YAML 헤더는 16 KiB·깊이 16·이벤트 4,096개 이내이고 인자는 65,536자, 확장된 본문은 512 Ki 문자 이내이다. POSIX에서는 정규 파일 여부와 각 경로 구성요소를 `openat`/`O_NOFOLLOW`로 다시 확인한다. 비-POSIX 구현은 정규 파일과 canonical 경계 검사를 사용하며 동일한 디스크립터 기반 경합 방어를 제공하지 않는다.

비어 있지 않은 `allowed-tools`에 따른 권한 추가, 인라인 스킬의 모델 변경·agent 선택, effort, `hooks`, `paths`, `shell`, 알 수 없는 실행 속성, `!` 백틱 또는 실행 코드 블록은 `unsupported_features`에 표시하고 호출 시 `runtime_unavailable`로 실패한다. 실행하지 않은 기능을 조용히 무시하지 않는다. 참조와 같은 사용자/관리/프로젝트 계층, legacy commands, 동적 하위 경로 활성화, MCP 스킬 수신, 번들·플러그인·설치/갱신·검색 순위, 활성 스킬 재주입도 아직 남아 있다. 전체 skills 영역은 **partial**이다.

## 별도 자식에서 실행

```markdown
---
description: Inspect a file in an isolated reader conversation
context: fork
agent: Explore
model: inherit
---
Read $0 and return the observed result.
```

`Subagents::attach(options, owner)`가 `EngineOptions::forkedSkill` 콜백도 연결한다. 엔진은 하위 계층에서 Subagents를 참조하지 않는다. C++ 호스트는 `SkillForkExecutor`를 교체할 수 있으며 호출은 동기식이고 취소 토큰에 협력해야 한다. 실행기가 없으면 목록에 `fork-executor-unavailable`을 표시하고 모델용 목록에서 제외하며 직접 호출은 실패한다. 데몬·MCP 실행기는 기존 서브에이전트 설정을 통해 연결한다.

스킬 본문·인자·경로·SHA-256을 호출하는 부모 세션에서 한 번 읽어 고정한다. `${CLAUDE_SESSION_ID}`는 부모 ID이다. 이 스냅샷과 추가 사용자 요청을 자식의 첫 실행 입력으로 저장한다. 자식은 선택한 프로파일의 시스템 프롬프트·초기 프롬프트·사전 로딩 스킬을 받으며 부모 대화·압축 이력·도구 결과는 복사하지 않는다. `Agent`의 `fork_context`와 구별되는 동작이다. 프로파일의 사전 로딩 스킬은 본문 로딩이므로 그 스킬에 `context: fork`가 있어도 재귀 실행하지 않는다. 자식 엔진에는 fork 실행기를 전달하지 않아 반복 위임을 막는다.

`agent`가 없으면 `general-purpose`를 선택한다. 지정한 이름을 찾지 못하면 `general-purpose`, 그마저 없으면 첫 활성 프로파일을 선택한다. 해당 이름의 잘못된 파일이 있거나 선택한 프로파일에 미지원 실행 속성이 있으면 우회하지 않고 실패한다. `model`이 없으면 프로파일 모델을 따르고 `inherit`이면 부모 모델을 사용한다. 다른 값은 호스트의 모델 별칭·허용 목록을 거치며 파일이 모델 사용 권한을 추가하지 않는다. 프로파일의 도구 범위와 부모의 Deny/Ask/Plan 정책은 그대로 적용한다.

직접 `RunRequest.skill`/`agent.run` 호출은 부모 모델을 호출하지 않고 자식의 최종 텍스트·상태·턴 수·사용량을 반환한다. 반환 `run_id`와 `session_id`는 부모 호출의 ID이고 자식 식별자는 마지막 메시지의 `iilocal.skill_fork`에 있다. 부모에는 간단한 호출 메시지와 최종 응답을 저장한다. 모델이 `Skill` 도구를 호출하면 `status: forked`, `success`, `commandName`, `agentId`, `result`, `execution`을 도구 결과로 받고 부모 모델이 후속 턴을 진행한다. 자식 사용량은 `execution.result.usage`에 있으며 부모 모델 실행의 사용량에 중복 합산하지 않는다. 본문을 인라인 주입하는 복원 마커는 생성하지 않는다.

양쪽 경로 모두 부모 실행의 generation 설정과 명시적 contextPaths를 전달한다. 자식 턴 수는 부모 요청·프로파일·호스트의 최소 한도이며 시간·동시 실행·기록 제한은 Subagents 설정을 따른다. 프로파일의 `background: true`도 fork 호출은 동기 실행한다. 별도 완료 알림을 만들지 않는다. 직접 호출 중 새 큐 입력은 다음 부모 실행까지 대기한다. 모델 도구 호출 중 긴급 큐 입력은 기존 인터럽트 경로로 자식을 취소한다. 취소·기한 초과·자식 실패·턴 한도를 그대로 보고하고, 진행 콜백이 실패해도 자식이 끝날 때까지 기다린다. 재개·재시작은 기존 기록을 읽을 뿐 이 스킬 호출을 자동 재실행하지 않는다.

비교 근거는 고정 참조 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/utils/forkedAgent.ts`와 `tools/SkillTool/SkillTool.ts`, `utils/processUserInput/processSlashCommand.tsx`이다. 일반 동기 경로·대화 분리·에이전트 fallback·결과 반환을 구현했으며 KAIROS 예약 호출의 백그라운드 경로, effort 병합, allowed-tools 권한 추가, 터미널 진행 UI의 동일성은 포함하지 않는다. 참조 프롬프트를 복사하거나 TypeScript 실행에 의존하지 않는다.

관찰한 참조는 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`의 [loadSkillsDir.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/skills/loadSkillsDir.ts), [SkillTool.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/SkillTool/SkillTool.ts), [argumentSubstitution.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/utils/argumentSubstitution.ts), [SkillTool/prompt.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/SkillTool/prompt.ts)이다. TypeScript 소스와 번들 스킬 본문은 SDK에 포함하지 않았다. 검증 결과는 [Verification.md](Verification.md)에 별도로 기록한다.
