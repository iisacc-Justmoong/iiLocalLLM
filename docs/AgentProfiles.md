# 에이전트 프로파일과 생명주기 (0.16.0)

프로파일은 자식 에이전트의 프롬프트·도구 범위·모델 선택·턴 상한·초기 지침과 스킬을 정의한다. C++ `Subagents` 실행기에 연결하며 API·MCP·CLI는 같은 구현을 호출한다. 추가 생산 의존성 없이 기존 Qt와 libyaml을 재사용한다. 전체 Claude Code 프로파일·서브에이전트 호환 상태는 **partial**이다.

## 탐색과 우선순위

`discoverAgentProfiles(workspace, options, hostDefinitions, token)`은 호출할 때마다 현재 파일을 읽는다. 같은 이름은 아래 순서에서 먼저 발견한 정의가 활성화된다.

1. `managedDirectory`
2. `overrides`: 이름을 키로 하는 JSON 정의
3. C++ `hostDefinitions`
4. `directories`: 앞의 디렉터리부터
5. 작업 디렉터리와 상위 프로젝트의 `.claude/agents`: 가까운 디렉터리부터
6. `userDirectory`
7. `pluginDirectories`: 앞의 디렉터리부터
8. 내장 `general-purpose`, `Explore`, `Plan`

호스트 정의와 명시 디렉터리는 참조의 실행 시 설정 계층에 해당한다. 관리 설정이 그 위에 있다. `projectBoundary`를 지정하면 그 디렉터리까지 탐색하며 작업 디렉터리를 포함해야 한다. 생략하면 가까운 `.git` 파일/디렉터리, 사용자 홈 또는 파일시스템 루트에서 멈춘다. 사용자·관리·플러그인 경로는 호스트가 명시해야 하며 홈 설정을 자동 활성화하지 않는다. git worktree의 원본 checkout으로 돌아가 탐색하는 기능은 아직 없다.

각 디렉터리의 `.md` 파일을 재귀적으로 읽는다. 파일명으로 정렬하고 디렉터리 심볼릭 링크를 따라가지 않는다. 파일의 실제 경로는 해당 프로파일 디렉터리 안에 있어야 한다. POSIX에서는 장치·inode로 같은 물리 파일의 중복을 감지한다. 기본 상한은 파일 128 KiB, 전체 파일·JSON override 1 MiB, 프로파일 256개, 탐색 엔트리 4,096개이다. UTF-8·NUL·YAML 구조·중복 키·깊이·이벤트 수를 검사하고 YAML alias를 거부한다. 모든 YAML 타입을 일반 YAML 라이브러리의 동작과 동일하게 지원한다고 주장하지 않는다.

카탈로그는 활성 `profiles`, 가려진 `shadowed`, 잘못된 `failed_files`를 반환한다. 이름 없는 Markdown은 참고 파일로 건너뛴다. 유효한 이름을 읽은 뒤 나머지 정의가 잘못됐으면 그 이름을 예약하여 더 낮은 계층의 정의를 대신 실행하지 않는다. 이름 자체를 읽을 수 없는 파일까지 대체 관계를 추정하지는 않는다. 전체 리소스 상한·잘못된 루트 설정은 명시적 오류이다.

`SubagentOptions::profiles.enabled`는 임베디드 호스트에서 기본 false이다. 비활성 상태에서는 기존 C++ 정의와 기본 `general-purpose`를 사용한다. 독립 데몬/MCP 실행기는 기본으로 프로젝트 탐색과 세 개의 내장 역할을 켠다. `Explore`는 읽기 전용 조사, `Plan`은 읽기 전용 계획 역할이다. 본문은 iiLocalLLM에서 작성했으며 참조의 번들 프롬프트나 모든 조건부 역할을 그대로 제공하지 않는다.

## 파일 형식과 실행

```markdown
---
name: reviewer
description: Inspect the requested workspace file and report evidence.
tools: [Read, Glob, Grep]
disallowedTools: [Glob]
model: inherit
maxTurns: 6
permissionMode: plan
skills: [inspect]
initialPrompt: Verify the available evidence before answering.
background: false
---
Read the requested files and report findings with their paths.
```

`name`, `description`, 비어 있지 않은 본문이 필요하다. JSON override는 본문 대신 `prompt`를 사용한다. `tools` 생략은 `*`, 빈 값·null·빈 배열은 도구 없음이다. 문자열은 쉼표/공백으로 나누며 배열도 허용한다. `disallowedTools`와 `readOnly`를 추가로 적용한다. 도구 이름의 wildcard 패턴은 지원하지만 `Bash(command:*)`처럼 인자별 규칙은 아직 지원하지 않는다.

`Subagents::attach(engineOptions, owner)`는 상태 제어 도구를 고정 등록하고 `EngineOptions::additionalToolsProvider`를 통해 각 턴과 직접 실행 시 현재 Agent 설명·타입 목록을 만든다. 기존 `Subagents::tools()`는 호출 당시의 정의 목록을 반환하므로 계속 바뀌는 파일 프로파일에는 `attach`를 사용한다. 호스트의 다른 provider도 합성하며 이름 충돌은 오류이다. 프로파일 전체를 읽을 수 없는 경우 Agent 정의에 `profile_discovery_error`를 표시하고 `AgentProfiles` 호출이 오류를 반환한다. 기존 자식의 조회·중단과 MCP 연결 종료는 계속 사용할 수 있다.

새 자식은 선택한 정의의 프롬프트·출처·실제 경로·파일 SHA-256을 비공개 기록에 저장한다. 실행 중 파일 변경은 그 자식의 프롬프트를 바꾸지 않는다. 다음 실행과 재개는 현재 정의를 다시 선택하며 재개 권한은 저장된 범위와 현재 범위의 교집합이다. 재개 시 대화·모델·기존 스킬 본문을 유지하고 `initialPrompt`를 다시 추가하지 않는다. 현재 프로파일 삭제나 미지원 필드 추가는 재개를 실패시킨다.

`model: inherit`는 부모 모델을 사용한다. 파일의 모델 지정은 사용 권한이 아니다. 부모 모델, 호스트 `allowedModels`, `modelAliases`의 대상, 신뢰된 C++ 호스트 프로파일의 모델만 선택할 수 있다. 별칭은 명시적 문자열 매핑이며 `sonnet`/`opus`/`haiku`를 임의로 로컬 모델에 대응시키지 않는다. 재개에서도 현재 호스트의 모델 허용 여부를 확인한다.

0.26은 자식 접수 시 승인된 부모의 런타임 권한을 독립 복사하고 재개 때 현재 부모 상태를 다시 받는다. 완료 후 자식 메모리 갱신은 버리며 파일 갱신은 유지한다. [PermissionUpdates.md](PermissionUpdates.md)의 수명 계약을 따른다.

부모 정책의 Deny와 Ask는 프로파일이 Allow로 바꾸지 못한다. `dontAsk`는 남은 Ask를 Deny로 바꾸며 `plan`은 기존 C++ Plan 정책의 제한을 더한다. `default`, `inherit`, `acceptEdits`, `bypassPermissions`도 부모 정책을 상한으로 유지한다. 참조의 전체 권한 설정 병합이나 자동 분류 모드와 동일하다는 뜻은 아니다. `auto`는 실행 미지원이다.

`background: true`는 호출 인자의 false와 관계없이 백그라운드 실행을 선택한다. 실행 중인 동기 작업을 자동으로 백그라운드로 바꾸는 기능은 아직 없다. `skills`는 새 자식에 한 번 인라인으로 사전 로딩한다. 기존 스킬 검증·본문 크기 제한을 적용하고 새 자식 세션 ID로 `${CLAUDE_SESSION_ID}`를 치환한 뒤 대화를 원자적으로 저장한다. 로딩 실패 시 대화와 작업 기록을 남기지 않는다. 모델 호출을 금지한 스킬, 없는 스킬, 미지원 기능이 있는 스킬은 실패한다. 참조가 일부 누락된 스킬을 경고 후 건너뛰는 것과 구별된다. 스킬 로딩 자체는 도구 권한을 넓히지 않으며 `Skill` 도구가 없는 프로파일도 사전 로딩은 사용할 수 있다.

`SessionStore::createFromSnapshot`의 초기화 콜백은 새 ID와 메시지 목록을 저장 전에 제공한다. 이 단계의 예외는 디스크에 대화를 만들지 않는다. 아직 미해결인 도구 호출이 추가되면 실패한다.

`hooks`, `mcpServers`, `memory`, `isolation`, `effort` 등 실행 미지원 필드는 `unsupported_features`에 표시한다. 이러한 프로파일은 모델의 Agent 타입 목록에서 제외하고 직접 실행·재개도 거부한다. `color`는 메타데이터로만 보존한다. 공개 JSON 카탈로그는 프롬프트·초기 지침·미지원 필드의 원문을 포함하지 않는다. 특히 MCP나 훅 정의에 포함될 수 있는 비공개 값을 모델 카탈로그에 노출하지 않는다. C++ 정의와 비공개 기록에는 원문 메타데이터가 남는다.

## C++ 훅

부모 호스트의 `EngineOptions::hooks`로 `SubagentStart`와 `SubagentStop`을 받는다. `HookInput.context`에는 `agent_id`, `agent_type`, `parent_session_id`가 들어간다. 모든 자식 훅은 이 문맥을 포함하고 자식의 일반 `Stop` 이벤트는 `SubagentStop`으로 바꾼다. 부모 Engine의 Stop과 혼동하지 않는다.

Start는 각 새 실행·재개가 수락된 뒤, 자식 Engine의 run을 시작하기 전에 호출하므로 아직 `runId`가 비어 있다. `sessionId`와 `context.agent_id`는 이미 유효하다. 피드백은 크기를 검사해 자식 대화에 저장한다. 차단·예외·취소는 실패 또는 취소된 작업으로 기록하며 모델을 호출하지 않는다. 부모 저장소나 Subagents 뮤텍스를 잡은 채 호스트 콜백을 호출하지 않는다.

Stop은 모델이 최종 답변을 반환할 때 실행한다. 차단하면 피드백을 자식 대화에 추가하고 턴 상한 안에서 계속한다. `context.stop_hook_active`는 이전 Stop 차단으로 계속 작업한 상태인지 알려 준다. 취소·실행 오류·턴 상한을 포함하는 무조건적인 종료 알림 훅은 아니며 모든 실패에 Stop이 호출된다고 가정하면 안 된다. 콜백은 동기적으로 실행되며 취소 토큰에 협력해야 한다. 해당 자식을 기다리거나 소유자를 close/파괴하는 콜백은 지원하지 않는다.

이 단계는 C++ 콜백 계약이다. 파일의 `hooks`를 외부 명령·HTTP·모델 실행으로 해석하지 않는다.

## 호스트 설정과 인터페이스

데몬과 `iillm-mcp`는 `--agent-profiles FILE`로 다음 JSON을 받는다. 파일은 현재 사용자 소유의 비공개 일반 파일이어야 하며 최대 1 MiB이다. 경로는 이 JSON 파일의 디렉터리를 기준으로 해석한다. 알려지지 않은 키와 잘못된 타입은 런타임 초기화 전에 실패한다.

```json
{
  "include_builtins": true,
  "include_project": true,
  "project_boundary": "/data/workspace",
  "user_directory": "agents/user",
  "managed_directory": "agents/managed",
  "directories": ["agents/launch"],
  "plugin_directories": ["agents/plugin"],
  "model_aliases": {"small": "model://local-small"},
  "allowed_models": ["model://local-other"],
  "overrides": {
    "reviewer": {"description": "Inspect input", "prompt": "Read and report evidence.", "tools": ["Read"]}
  }
}
```

필드는 모두 선택적이다. `--no-agent-profiles`는 파일 탐색과 내장 역할 확장을 끄며 `--agent-profiles`와 동시에 지정할 수 없다. 에이전트 API 비활성(`--agent-no-subagents`)이나 MCP `--no-subagents`와는 별도 설정이다. 플러그인 디렉터리를 직접 읽는 기능은 플러그인 설치·검증·네임스페이스 관리 기능의 완료 증거가 아니다.

인증 API `agent.agents.profiles`는 `session_id`를 받으며 다른 앱의 대화 ID를 거부한다. MCP `iiLocalLLM.agent.agents.profiles`는 해당 연결의 대화를 사용하고 별도 인자를 받지 않는다. CLI는 `iillm --auth-file TOKEN agent agents profiles SESSION`이다. 응답의 `{text, result, is_error}` 또는 MCP `structuredContent/isError` 계약은 기존과 같다.

공개 구조체·SessionStore 서명이 바뀌어 ABI는 **0.18**이다. 소비자는 헤더와 라이브러리를 함께 갱신하고 다시 빌드해야 한다. 검사·설치 소비자·실제 모델 결과는 [Verification.md](Verification.md)에 기록한다.

## 참조와 남은 범위

분석 기준은 `c8cd253554319f32ff64ff7000636199f720c9bc`의 [loadAgentsDir.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/AgentTool/loadAgentsDir.ts), [markdownConfigLoader.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/utils/markdownConfigLoader.ts), [runAgent.ts](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/tools/AgentTool/runAgent.ts)이다. 공개 미러의 구현을 관찰한 것이며 원본 배포본의 진위나 활성화된 서버 설정 전체를 증명하지 않는다.

스킬 `context: fork`는 선택한 자식에 스킬 본문을 전달하는 별도 실행 경로이며 현재의 스킬 사전 로딩·`Agent.fork_context`와 구별된다. 이 경로는 0.17.0에 구현되었다([Skills.md](Skills.md)). 전체 내장 역할과 조건부 선택, 플러그인 네임스페이스와 생명주기, 에이전트별 MCP·메모리·외부 훅·worktree·remote·팀 통신은 계속 구현해야 한다.

호출 시 부모의 현재 도구 권한을 자식에 전달한다. 사전 로딩된 스킬의 본문은 권한을 추가하지 않으며 재개는 현재 호출자의 권한을 따른다. [권한 수명](Permissions.md)을 참조한다.
