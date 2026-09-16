# 에이전트 하네스 구현·검증 대응표

목표는 Claude Code 분석본의 전체 하네스 기능과 iisacc 앱의 양방향 MCP/API 연동이다. C++을 기본 구현 언어로 사용하며 Python은 기존 MLX 런타임처럼 필요가 검증된 경계에서만 사용한다. 이 표는 범위를 줄이기 위한 목록이 아니며 완료 기준은 실제 기능·전송·앱 실행 증거이다.

기준 소스: Exhen/claude-code-2.1.88, 커밋 c8cd253554319f32ff64ff7000636199f720c9bc. 해당 저장소는 동작 분석 자료이며 iiLocalLLM의 구현 소스를 복사하지 않는다. 누락되거나 비활성인 내부 기능은 명칭만 보고 구현 완료로 표시하지 않는다.

전체 인벤토리는 catalog/harness-parity.json에 보관한다. 테스트가 통과해도 범위가 해당 요구 전체를 증명하지 않으면 partial로 유지한다.

| ID | 요구 | 현재 상태 |
|---|---|---|
| execution | 대화 루프·스트림·도구 결과 정합성·취소·복구·턴/비용 예산 | partial |
| providers | llama.cpp·MLX 네이티브 도구 템플릿, 로컬 OpenAI/Ollama 호환 연결, 공급자 어댑터 | partial |
| tools | 등록·별칭·JSON Schema 입력/출력·입력별 병렬 실행·큰 결과 보관 | partial |
| permissions | 정책 계층·모드·규칙·사용자/호스트 질문·자동 분류·OS 샌드박스 | partial |
| files | Read/Write/Edit/Glob/Grep·동시 변경 감지·백업·되돌리기 | partial |
| shell | Bash/PowerShell·환경·cwd·프로세스 트리 취소·백그라운드 작업 | partial |
| web | WebFetch/WebSearch·컨텐츠 변환·캐시·네트워크 정책 | partial |
| multimodal | 이미지·PDF·노트북·음성 입력과 모델별 지원 협상 | partial |
| context | 프롬프트 조합·CLAUDE/AGENTS 규칙·첨부·요약·microcompact·cache 관리 | partial |
| memory | 프로젝트 Markdown 메모리·검색·자동 정리·세션 메모리 | partial |
| sessions | JSONL 영속 기록·resume/fork·압축 경계·파일 rewind | partial |
| subagents | 전문 에이전트·부모 컨텍스트 fork·모델/도구/권한 범위·백그라운드 알림 | partial |
| teams | 동일 프로세스/별도 프로세스 팀·mailbox·작업·권한 전달·worktree | partial |
| mcp_client | stdio·Streamable HTTP·legacy SSE·초기화·버전/기능 협상·재연결·인증 | partial |
| mcp_features | 도구·리소스·프롬프트·roots·sampling·elicitation·진행/취소·구독·tasks | partial |
| mcp_server | iiLocalLLM 및 앱 기능을 MCP로 제공·세션 격리·권한·동시성·구조화 결과 | partial |
| api | C++ SDK·기존 native IPC·HTTP/SSE 에이전트 API·OpenAI 도구 호환 | partial |
| discovery | 앱 manifest·MCP/API 자동 인식·기능 협상·tool search·지연 공개 | partial |
| skills | SKILL.md·메타데이터·인라인/fork 실행·허용 도구·검색·설치 | partial |
| plugins | manifest·명령/스킬/에이전트/훅/MCP/LSP 등록·버전/캐시·설치/갱신 | pending |
| hooks | 전체 생명주기·C++ 콜백·명령·HTTP·모델/에이전트·입력 변경·결과/차단 | partial |
| tasks | 계획·Todo/Task·작업 의존성·입력 큐·백그라운드 작업/알림 | partial |
| git | 작업 디렉터리·worktree·브랜치·변경 이력·복구 | partial |
| editor | LSP·IDE 통신·파일 변경 알림·진단·심볼/정의/참조 | partial |
| frontends | CLI interactive/headless·구조화 입출력·앱용 상태/이벤트·LVRS UI 바인딩 | partial |
| remote | 원격/bridge 실행·인증·연결 복원·메시지 라우팅 | pending |
| settings | 프로젝트/사용자/관리 설정 우선순위·기능 gate·환경·migration | partial |
| observability | 구조화 로그·실행 trace·사용량/비용·성능·오류 진단 | partial |
| iisacc_apps | Society·Dreamscapes·Congregation·Thinking Space 실제 consumer 연동 검증 | partial |
| packaging | 공개 헤더·CMake export·daemon/CLI·설치 consumer·플랫폼 검증 | partial |
| conditional | 분석본의 내부/조건부 기능: 실제 구현 확보 범위와 iiLocalLLM 대응을 개별 검증 | pending |

## 현재 구현 순서

0.4.0에서 C++ stdio 클라이언트, 도구·리소스·프롬프트, roots, 진행·취소, 호스트 요청 콜백 및 에이전트 어댑터를 추가했다. 프로토콜 버전 2025-11-25/2025-06-18/2025-03-26을 지원하며, C++ 내장/POSIX stdio 서버와 연결별 로컬 에이전트 실행도 제공한다. 0.7.0에서는 공통 C++ 클라이언트에 Streamable HTTP, bearer/header 공급자, SSE 복원과 세션 404 재초기화를 연결했다. 0.8.0은 인증된 HTTP 서버와 CLI, 요청별 SSE 재개·취소를 추가한다. legacy SSE·OAuth·원격 앱 발견과 최신 규격은 남아 있다. 2025-03 배열 수신·응답 결합을 양쪽에 구현했다. 계약과 교차 검증 절차는 [MCP.md](MCP.md), [MCPHTTP.md](MCPHTTP.md), [MCPHTTPServer.md](MCPHTTPServer.md), [MCPServer.md](MCPServer.md)에 기록한다.

1. C++ 대화/도구/권한/취소 계약, JSON Schema 검증, JSONL 복원, 기존 로컬 추론 연결을 종단까지 구현한다.
2. 같은 실행 계층에 MCP client/server와 IPC/HTTP/CLI를 연결한다.
3. 컨텍스트·메모리·스킬·훅·서브에이전트·팀·앱 발견을 확장한다.
4. 남은 기본 도구·에디터·멀티모달·원격·조건부 기능을 개별 검증하고 실제 앱 consumer에서 호환성을 확인한다.

## 의존성 결정

- Qt 6.8.3 Core/Network 및 기존 cpp-httplib를 재사용한다.
- JSON Schema 전체 표준 검증은 jsoncons 1.9.0의 C++ 헤더 구현을 사용한다. 2026-08-07 릴리스를 확인했으며 2020-12와 이전 dialect를 지원한다. Boost-1.0 및 포함된 A5HASH MIT 고지를 패키지에 보존한다. 런타임 외부 서비스 및 Python 프로세스가 추가되지 않는다.
- 공식 MCP SDK 목록에는 C++이 없다. 프로토콜은 버전 고정된 공식 규격으로 검증하며 기존 Qt 전송을 활용한다. MCP 2025-11-25 호환과 최신 2026-07-28의 차이를 별도로 추적한다.

출처: https://github.com/danielaparker/jsoncons/releases/tag/v1.9.0 , https://modelcontextprotocol.io/docs/2026-07-28/sdk , https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle

0.5.0은 workspace 지침·Markdown import·YAML 경로 규칙·실제 모델 입력 조합과 인증된 API 조회를 추가한다. MCP 실행도 `context_paths`를 지원한다. [ProjectContext.md](ProjectContext.md)에 대응 범위를 기록하며 managed/user 지침·첨부·요약·microcompact·캐시는 남아 있다.

0.6.0은 실제 네이티브 예산 측정, 오래된 도구 결과 축소, 여러 묶음의 로컬 모델 요약, 원본 보존 체크포인트·재개·분기, 원문 조회 도구, 압축 훅 및 API/MCP 수동 호출을 추가한다. [Compaction.md](Compaction.md)에 정확한 범위와 실패 계약을 기록한다. 세션 메모리·첨부 재주입·reactive overflow 복구·서버 캐시 편집 및 전체 앱 연동은 남아 있으므로 context/sessions는 partial을 유지한다.

0.9.0은 호스트가 지정한 MCP 설정 연결·복구, 목록 변경 반영, 대화별 ToolSearch와 선택 상태의 재개·분기·압축 복구, 스키마·연결 변경 시 선택 무효화, 인증된 MCP 상태 API/CLI를 추가한다. 0.9.0 시점에는 앱 manifest·설치 앱 자동 발견, 전체 설정 계층, 공급자별 검색 최적화 및 실제 Society/Dreamscapes endpoint 연동이 남아 있었다. [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

0.10.0은 같은 OS 사용자의 실행 중인 앱 endpoint 등록·발견·토큰 인증, QObject 주 스레드 호출과 취소/종료를 추가한다. 실제 Society·Dreamscapes 데스크톱 컨트롤러를 연결하고 MCP 구조화 결과가 모델 입력에서 누락되던 문제를 수정했다. Society는 실제 Qwen 0.5B의 eager 도구 호출 후 컨테이너 ID 소비까지, Dreamscapes는 프로토콜 fixture로 실제 앱 생성 큐·결과 PNG·취소까지 확인한다. 앱 재시작은 새 인스턴스로 발견된다. iisacc_apps는 partial이며 나머지 앱·플랫폼과 광범위한 자율 작업은 남아 있다. [LocalApplications.md](LocalApplications.md), [Verification.md](Verification.md)를 참조한다.

0.11.0은 영속 Task/Todo 저장, 원자적 의존 관계·삭제·담당자 선점, 대화별 최신 상태 주입, TaskCreated/TaskCompleted 게시 전 훅, C++·인증된 API·MCP·CLI를 추가한다. 작업 실행기나 백그라운드 작업, 계획 모드 전환, 팀 알림은 별도 미완료 항목이다. [Tasks.md](Tasks.md), [Verification.md](Verification.md)를 참조한다.

0.12.0은 C++ 백그라운드 Bash, TaskOutput·TaskStop·ShellTaskList, 원시 출력 보존·페이지화, 소유 세션 격리, 시간/출력/동시 실행 상한과 정상 종료·재시작 복구를 추가한다. API·CLI 및 MCP의 실행 중 조회·중단을 연결하고 각 모델 턴에 현재 상태를 반영한다. 자동 배경 전환, 지속 환경, Windows/모바일, 백그라운드 에이전트, 완료 알림·입력 큐와 MCP 비동기 tasks 규격은 남아 있으므로 shell/tasks/API의 partial 상태를 유지한다. [BackgroundTasks.md](BackgroundTasks.md), [Verification.md](Verification.md)를 참조한다.

0.12.1은 llama.cpp의 명시적 추론 모드 제어를 일반·구조화 대화에 연결하고, 추론만 있는 응답과 빈 응답을 구분한다. 명시한 Qwen3 8B 조건에서 지연 Task 실행은 소스·설치본 전체 검사, 지연 MCP 실행은 소스 단독 대조·설치본 전체 검사에서 통과했다. 기존 모델 실패와 최초 MCP 연결 실패는 별도로 유지한다. 원문의 도구 모양 문자열을 임의로 실행하지 않으며, 네이티브 provider와 전체 실행 범위는 partial을 유지한다. [NativeThinking.md](NativeThinking.md), [Verification.md](Verification.md)를 참조한다.

0.13.0은 prompt/notification의 대화별 영속 입력 큐, now/next/later 우선순위, 같은 종류 묶음, 중단된 도구 이력 복구와 저장 후 확인 실패의 중복 방지를 추가한다. C++·인증된 API·CLI·MCP에서 실행 중 입력을 받고 유휴 큐는 명시적으로 시작한다. 자동 유휴 기동, 셸 완료 알림 생산, 첨부·slash/bash 입력 모드·수신 에이전트 지정·Sleep 깨우기와 팀 mailbox는 남아 있어 tasks/execution/API는 partial이다. [InputQueue.md](InputQueue.md), [Verification.md](Verification.md)를 참조한다.

0.13.1은 ServiceModel에서 누락하던 도구 data와 오류 상태를 JSON 관측으로 전달하고 동일 입력으로 예산을 측정한다. 실제 입력·KV 캐시 대조와 모델 수락은 [Verification.md](Verification.md)에 분리 기록하며 execution/providers/tools는 partial을 유지한다.

0.13.2는 MCP 로컬 요청 기한의 구조화 오류와 연결·도구 발견 단계별 상태 진단 및 서버별 설정 기한을 추가한다. 관측성은 이 범위에 한해 partial이며 초기화 간헐 실패의 해결이나 전체 trace·비용 진단 완료를 뜻하지 않는다. [MCP.md](MCP.md), [ToolDiscovery.md](ToolDiscovery.md), [Verification.md](Verification.md)를 참조한다.

0.14.0은 로컬 스킬의 메타데이터 목록·인자 치환·인라인 대화 주입·복원을 C++로 구현하고 인증 API·MCP·CLI에 연결한다. 허용 도구에 따른 권한 추가, 훅, 조건부/원격 스킬, 번들/플러그인 설치와 갱신은 아직 남아 있다. [Skills.md](Skills.md)의 구체적 지원 범위를 따른다.

0.15.0은 별도 C++ Engine의 자식 대화, 범위 제한, 동기·백그라운드 실행, 명시적 부모 컨텍스트 분기, 재개·취소·기한과 결과 제어·완료 알림을 API·MCP·CLI에 연결한다. 프로파일 파일 계층·전체 전문 역할·전용 훅·에이전트별 확장·팀·worktree·remote와 전체 앱 검증은 남아 있다. [Subagents.md](Subagents.md)를 참조한다.

0.16.0은 파일·JSON 프로파일 계층과 변경 인식, `general-purpose`/`Explore`/`Plan`의 로컬 역할, 스킬 사전 로딩, 호스트 모델 별칭·추가 제한, SubagentStart/SubagentStop C++ 훅과 프로파일 조회 API·MCP·CLI를 추가한다. 참조의 모든 전문 역할·플러그인 생명주기·에이전트별 MCP/메모리·외부 훅은 계속 미완료이다. 세부 계약은 [AgentProfiles.md](AgentProfiles.md)를 따른다.

0.17.0은 C++ `context: fork` 스킬의 사용자·모델 호출, 자식 격리, 모델 별칭과 동기 결과 반환을 추가한다. skills와 subagents의 전체 상태는 계속 partial이며 effort·KAIROS 예약 fork 등은 남아 있다. [Skills.md](Skills.md)에 구현 범위와 차이를 기록한다.

0.18.0은 호출 범위의 allowed-tools, 권한 판정/실행 스냅샷, 인자 규칙과 C Bash AST 검사를 추가한다. 권한의 설정 계층·자동 분류·전체 BashSecurity·OS 샌드박스 등은 남아 있어 permissions/skills/subagents는 계속 partial이다. [Permissions.md](Permissions.md)에 구체적인 차이를 기록한다.

0.19.0은 파일 기반 권한 설정의 계층·관리 규칙·출처 경로·실시간 반영·호스트 설정과 인증 조회를 구현했다. settings와 permissions는 partial이다. 추가 디렉터리·외부 관리 공급자·일반 환경 설정·마이그레이션·자동 분류·OS 샌드박스는 남아 있다. [PermissionSettings.md](PermissionSettings.md)가 현재 계약이다.

0.20.0은 추가 작업 디렉터리를 C++ 파일 도구·Bash 리다이렉션·자식 정책·CLI·API·MCP에 연결한다. 출처별 canonical 바인딩과 비공개 호스트 파일 보호를 포함한다. 원격 디렉터리 변경·추가 instruction 로딩·전체 검색/BashSecurity는 남아 있으며 전체 목표는 partial이다. [WorkingDirectories.md](WorkingDirectories.md)를 참조한다.

0.21.0은 명시적 호스트 설정을 받는 데스크톱 POSIX C++ 명령 훅을 추가한다. 도구·모델·Stop·압축·Task·Subagent 콜백, JSON 입력/결과·차단·중단·입력 변경·일회 권한·병렬/once·취소를 API/CLI/MCP에 연결한다. HTTP/prompt/agent/async 훅, 나머지 생명주기와 스킬·에이전트·플러그인 설정 병합은 남아 있어 hooks 및 전체 목표는 partial이다. [CommandHooks.md](CommandHooks.md)를 따른다.

0.22.0은 C++ UserPromptSubmit·SessionStart, 원본 판정 보존·모델 문맥 제외, 큐 준비/확인 분리·재진입·취소·확인 실패 복구, 사용자 스킬과 내부 자식 지시 구분을 API/CLI/MCP에 연결한다. SessionEnd·clear·watchPaths·PermissionRequest/Denied 및 나머지 훅 실행기와 전체 앱 검증은 남아 있다. hooks와 전체 목표는 partial이며 [InputLifecycle.md](InputLifecycle.md)를 따른다.

0.23.0은 SessionEnd와 C++ endSession/close, 인증 API 세션 종료, MCP 교체·연결 종료·stdio 신호 정리를 구현한다. API 접수 요청과 Engine 대기 작업을 취소하고 활성화별 중복을 방지한다. [SessionEnd.md](SessionEnd.md)의 cooperative 시간 예산과 진단/자식 정리 한계를 적용한다. 이 문단은 0.23 당시 범위이다.

0.24.0은 C++/API/MCP 실제 clear, 즉시 SessionStart(clear), 새 ID/부모 기록, 백그라운드 셸·자식·완료 알림의 전환과 복구를 구현한다. 세부 경계는 [SessionClear.md](SessionClear.md)를 따른다. 참조의 전체 UI/팀/git/LSP/worktree/플러그인 캐시 초기화, watchPaths, 권한/나머지 훅과 실제 제품 consumer 검증은 남아 있으며 전체 목표는 partial이다.

0.25.0은 Ask 전용 PermissionRequest, allow/deny/interrupt, 입력 변경 뒤 스키마·준비·호스트 거부 재검사, C++ 구조화 응답과 명시적 권한 갱신 처리기를 API/IPC/MCP 실행 경로에 연결한다. [PermissionRequest.md](PermissionRequest.md)를 따른다. 이 문단은 0.25 당시 범위이다.

0.26.0은 SettingsPermissionPolicy의 여섯 갱신 연산, 사용자/프로젝트/로컬 파일 저장, session/cliArg 격리와 clear/fork/자식 접수 상속을 추가한다. CLI·API·MCP의 승인 응답이 기본 갱신을 적용한다. 개별 파일 잠금·원자적 교체와 여러 파일의 부분 커밋 경계는 [PermissionUpdates.md](PermissionUpdates.md)를 따른다. 자동 제안·원격 승인 중개·분류기/PermissionDenied·전체 보안/설정/앱 검증은 남아 있으므로 전체 목표는 partial이다.

## 0.27 앱 권한 요청

인증 API/native IPC와 MCP 연결별 승인 채널, 독립 제어 처리, 훅·앱 경쟁, 취소·기한·중복 응답을 추가한다. [PermissionRequests.md](PermissionRequests.md)에 활성화·요청·응답 계약과 참조 차이를 기술한다. 전체 하네스와 실제 앱 통합 완료를 의미하지 않는다.

## 0.28 HTTP 제어 용량

일반 HTTP 응답·MCP SSE의 활성/보관 용량과 승인 제어 용량을 분리한다. 느린 JSON/SSE 응답, 연결 종료와 재접속도 원래 분류의 한도에 포함한다. [ControlCapacity.md](ControlCapacity.md)에 C++·CLI 설정, 실제 전송 검사와 자원 한계를 기록한다. 입력 큐 등 임의 메서드에 제어 분류를 부여하지 않으며 영속 승인 복구·제품 승인 UI·나머지 하네스는 계속 partial이다.

## 0.29 HTTP 훅

C++ HTTP/HTTPS POST·JSON 응답을 공통 생명주기와 권한 경로에 연결한다. URL/환경 허용 목록, 중복 URL/조건 처리, 직접 연결 DNS 주소 고정·TLS 검증, 프록시, 취소·기한·응답 한도를 제공한다. [HTTPHooks.md](HTTPHooks.md)에 참조와의 차이 및 미검증 플랫폼/프록시 조합을 기술한다. 비동기 응답 접수 확인은 로컬 비동기 실행기 완료를 뜻하지 않는다. 전체 생명주기·prompt/agent/async 훅, 설정/스킬/플러그인 병합과 실제 앱 전체 검증은 남아 있으며 hooks 상태는 partial이다.

0.30.0은 C++ 프롬프트 훅의 단일 모델 판단, 스키마·기한·사용량 진단과 호스트 스냅샷 전달을 Engine·ToolRunner·API·MCP에 연결한다. 네이티브 JSON 출력 문법과 요청별 추론 모드 제어도 추가한다. 도구를 실행하는 agent 훅·전체 설정 병합·남은 생명주기와 앱 검증은 미완료이며 hooks/providers 상태는 partial이다. [PromptHooks.md](PromptHooks.md)에 입력 경계와 참조 차이를 기록한다.

0.31.0은 실제 도구를 사용하는 C++ agent 훅을 추가한다. 새 대화·dontAsk·정확한 transcript 읽기·StructuredOutput·50번째 메시지 경계·취소 정리를 구현하고 Task 게시 전 검증 잠금과 MCP 소유 대화 연결을 보완한다. 스킬·지연 도구 검색·서브에이전트 프로필을 기존 Engine에서 재사용한다. 전체 설정 병합·남은 생명주기·앱/플랫폼 검증은 미완료이며 전체 상태는 partial이다. 계약과 참조 차이는 [AgentHooks.md](AgentHooks.md)에 기록한다.

0.32.0은 PostToolUse의 updatedMCPToolOutput을 C++ 도구 경로에 연결한다. 실제 가져오기 MCP 도구만 문자열·MCP 콘텐츠 배열로 관측을 교체하고, 원격 출력 스키마를 먼저 검사한 뒤 모델·transcript·API·MCP로 전달한다. 훅이 설정된 가져오기 도구를 MCP로 재전달할 때는 원격 outputSchema를 공개하지 않는다. 새 런타임 의존성은 없다. 전체 설정 병합·남은 생명주기·실제 앱/플랫폼 검증은 계속 partial이다. [McpOutputHooks.md](McpOutputHooks.md)에 참조 unknown/공급자 콘텐츠와의 차이를 기록한다.

0.33.0은 C++ 비동기 명령 훅, 첫 stdout 행의 async 선언, 완료 문맥의 세션 전달과 asyncRewake 유휴 실행을 추가한다. 세션/연결별 수명과 취소, 자동 실행 횟수 제한, API/CLI/MCP 제어를 제공한다. 변경된 공개 구조체 때문에 소비자 재빌드가 필요하다. 자식 실행 후 재기동·환경 캐시 무효화·전체 생명주기 및 설정 병합은 계속 partial이다. [비동기 훅](AsyncHooks.md)을 따른다.

## 0.34–0.35 계획 검토와 사용자 질문

0.34는 세션 계획 파일·Enter/ExitPlanMode·호스트 수정 검토·변경 충돌 검사·API/MCP 소유자 제어를 추가했다. 0.35는 C++ AskUserQuestion·호스트 답변과 주석·질문 불변 검증·API/IPC/MCP 노출을 추가한다. 기존 JSON Schema 및 PermissionRequests를 재사용해 새 생산 의존성은 없다. [PlanMode.md](PlanMode.md)·[UserQuestions.md](UserQuestions.md)·[Verification.md](Verification.md)에 계약과 증거를 기록한다. 실제 앱 질문/승인 화면, 인터뷰와 이미지 첨부 등 전체 호환성은 진행 중이며 31개 영역의 21 partial·10 pending 상태를 유지한다.

0.36은 C++ QuestionInbox와 LVRS 질문 화면, 로컬 앱 MCP 질문과 자동 검증 도구 필터를 추가한다. 선택·여러 줄 자유 입력/메모·부분 답변·대기열·포커스·취소 계약은 [QuestionUI.md](QuestionUI.md)를 따른다. 전체 채팅/승인 화면·인터뷰·이미지·서식 미리보기·모바일/Windows 라우팅은 남아 있다. 현재 31개 영역 중 22 partial·9 pending이며 전체 하네스 완료가 아니다.

## 0.37 프로젝트 메모리

C++ 프로젝트별 Markdown 저장, MEMORY.md 자동 문맥, 목록·검색, 기존 파일 도구 연결과 해시를 확인하는 삭제·백업을 추가한다. 새 세션·분기·clear·압축과 자식 실행, 인증 API·MCP·CLI의 범위를 [ProjectMemory.md](ProjectMemory.md)에 기록한다. 다른 앱의 API 메모리와 비공개 저장 영역을 구분한다. 자동 추출·dream 정리, 모델 기반 회상·세션 검색, 전체 설정·팀·전문 에이전트·worktree·원격과 실제 앱/플랫폼 전체 검증은 미완료이다. memory는 partial이며 전체 목표의 완료를 뜻하지 않는다.

## 0.38 모델 기반 회상

관련 주제 메모의 로컬 모델 선택, 비동기 선행 실행과 문맥 전달, 중복·UTF-8 크기·전체 문맥 예산·오래된 정보 관리, 읽기 기록과 해시 검사, 인증 API·MCP·CLI를 추가한다. 대화 종료와 새 입력은 남은 선택을 취소하며 회상 내용을 새 사용자 요구사항으로 보존하지 않는다. 정확한 계약과 남은 자동 추출·dream 정리·세션 검색·전문 에이전트·설정·worktree·원격 범위는 [MemoryRecall.md](MemoryRecall.md)에 기록한다. 전체 하네스는 partial 상태이다.

## 0.39 대화 종료 메모리 추출

부모 모델 문맥과 native 읽기 기록을 복사한 C++ 작업, 세션별 커서·주기·최신 요청 병합, 메모리 쓰기 범위, 보수적인 Unix 읽기 셸 판정, 완료 통계와 인증 API·MCP·CLI를 추가한다. 주 대화에 추출 transcript를 붙이지 않으며 종료·clear·취소 수명을 연결한다. [MemoryExtraction.md](MemoryExtraction.md)에 세부 계약과 남은 셸·dream·세션 검색·전문 에이전트·설정·앱/플랫폼 범위를 명시한다. 전체 대응표 상태는 유지한다.

0.40은 C++ `SessionSearch`로 과거 대화의 리터럴 검색, 줄/바이트 근거, 큰 JSONL의 제한된 읽기와 이어 읽기, 소유권·파일 변경 검증을 구현하고 Engine/API/MCP/CLI에 연결한다. 참조의 의미 검색·태그/제목/브랜치 순위와 자동 dream 정리는 미완료이며 sessions와 전체 목표는 partial을 유지한다. [SessionHistory.md](SessionHistory.md)를 따른다.

## 0.41 프로젝트 메모리 정리

C++ MemoryDream은 기본 OFF인 자동 실행, 24시간·5개 대화·10분 목록 주기, QLockFile의 프로세스 간 배제와 성공 시각 저장을 추가한다. 부모 문맥·도구 정의·읽기 캐시를 유지하고 추출과 공통 실행 코드를 사용하며, 주 transcript와 분리된 진행/완료 상태를 API·MCP·CLI로 제공한다. 수동 요청도 동일 권한·잠금·한도를 적용한다. [MemoryDream.md](MemoryDream.md)에 참조의 낙관적 타임스탬프/기능 플래그와의 차이, 부분 쓰기와 협력적 취소, 남은 앱 UI·worktree·팀·원격·플랫폼 범위를 명시한다. 대응표는 23 partial·8 pending·0 complete를 유지한다.


0.42는 WebFetch의 조회·HTML5/인코딩 변환·캐시·도메인 권한·로컬 모델 추출과 API/MCP/CLI를 구현한다. WebSearch, 렌더링·로그인·프록시, 완전한 앱/플랫폼 검증은 남아 있어 web 영역은 partial이다. [WebFetch.md](WebFetch.md).

0.42의 전체 대응표는 24 partial·7 pending·0 complete이다.

0.43은 호스트가 구성한 stdio 언어 서버, LSP 9개 연산, 버전별 문서 동기화·진단, 소유 세션과 읽기 권한, API/MCP/CLI를 구현한다. IDE 미저장 버퍼·전체 프로젝트 watcher·동적 등록·플러그인 자동 설정과 전체 앱/플랫폼 검증은 남아 있다. [Lsp.md](Lsp.md). 전체 대응표는 25 partial·6 pending·0 complete이다.


0.44는 C++ 소유 작업 트리와 같은 대화의 파일·셸·권한 경로 전환, 생성·보존·재개·삭제 및 인증 API/MCP/CLI를 연결한다. [Worktrees.md](Worktrees.md)에 활성 clear 제한, 팀/서브에이전트·설정 훅·자동 복구와 실제 앱/플랫폼의 남은 범위를 명시한다. 대응표는 26 partial·5 pending·0 complete이며 전체 목표는 미완료이다.


0.45는 nbformat 4 셀 편집과 C++/인증 API/CLI/MCP 전송을 추가한다. [노트북 계약](Notebooks.md)과 [검증 기록](Verification.md)을 기준으로 files·permissions·multimodal·api·mcp_server·packaging의 근거를 보강한다. 현재 합계는 27 partial·4 pending·0 complete이다. 셀 이미지 출력·PDF/이미지/음성 입력과 전체 앱/플랫폼 검증이 남아 있으므로 multimodal은 partial이다.

## 파일 체크포인트 (0.46)

C++ 네이티브 Write·Edit·NotebookEdit의 원본을 사용자 메시지 경계에 기록하고 목록·수동 생성·파일 복원을 제공한다. 인증 API의 `agent.checkpoints.*`, CLI의 `agent checkpoints`, 연결 소유자에 묶인 MCP `iiLocalLLM.agent.checkpoints.*`가 같은 Engine을 사용한다. 저장 한도, 세션 잠금, preview·권한·작업 트리 경계와 남은 참조 기능은 [FileCheckpoints.md](FileCheckpoints.md)에 기록한다. 전체 대응 상태는 27 partial·4 pending·0 complete를 유지한다.

## 세션 분기 복제 (0.47)

C++ 세션 분기에 artifact·체크포인트의 독립 복사, 대화 경로 갱신, 게시 전 권한/계획 준비와 실패 정리를 추가한다. 자식 문맥 분기도 참조 artifact를 복제한다. API·CLI 및 연결 전환 MCP를 제공한다. [SessionFork.md](SessionFork.md)에 범위와 제한을 기록한다. 대화 rewind UI, 팀 공동 이력과 전체 앱/플랫폼 검증은 남아 있으며 27 partial·4 pending·0 complete를 유지한다.

0.49는 종료·리더·동료 메시지 우선순위와 한 건 단위 실행, 결과 본문 없는 유휴 알림, 시작·유휴 상태의 원자적 자동 Task 선점을 더한다. 0.48에서 제공한 이름 있는 C++ 팀원, 독립 대화, 공유 Task, 유휴 팀원의 후속 메시지 실행, 요청 ID에 묶인 종료, 부모 clear 이전과 API/MCP/CLI 제어를 유지한다. 유휴 리더 자동 실행, 중단 팀원 재개, 별도 프로세스·원격·계획 승인·worktree·실제 앱 소비자는 남아 있다. [Teams.md](Teams.md). 대응표는 28 partial·3 pending·0 complete이며 전체 목표는 완료하지 않았다.
