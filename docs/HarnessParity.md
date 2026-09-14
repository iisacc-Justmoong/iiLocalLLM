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
| web | WebFetch/WebSearch·컨텐츠 변환·캐시·네트워크 정책 | pending |
| multimodal | 이미지·PDF·노트북·음성 입력과 모델별 지원 협상 | pending |
| context | 프롬프트 조합·CLAUDE/AGENTS 규칙·첨부·요약·microcompact·cache 관리 | partial |
| memory | 프로젝트 Markdown 메모리·검색·자동 정리·세션 메모리 | pending |
| sessions | JSONL 영속 기록·resume/fork·압축 경계·파일 rewind | partial |
| subagents | 전문 에이전트·부모 컨텍스트 fork·모델/도구/권한 범위·백그라운드 알림 | partial |
| teams | 동일 프로세스/별도 프로세스 팀·mailbox·작업·권한 전달·worktree | pending |
| mcp_client | stdio·Streamable HTTP·legacy SSE·초기화·버전/기능 협상·재연결·인증 | partial |
| mcp_features | 도구·리소스·프롬프트·roots·sampling·elicitation·진행/취소·구독·tasks | partial |
| mcp_server | iiLocalLLM 및 앱 기능을 MCP로 제공·세션 격리·권한·동시성·구조화 결과 | partial |
| api | C++ SDK·기존 native IPC·HTTP/SSE 에이전트 API·OpenAI 도구 호환 | partial |
| discovery | 앱 manifest·MCP/API 자동 인식·기능 협상·tool search·지연 공개 | partial |
| skills | SKILL.md·메타데이터·인라인/fork 실행·허용 도구·검색·설치 | partial |
| plugins | manifest·명령/스킬/에이전트/훅/MCP/LSP 등록·버전/캐시·설치/갱신 | pending |
| hooks | 전체 생명주기·C++ 콜백·명령·HTTP·모델/에이전트·입력 변경·결과/차단 | partial |
| tasks | 계획·Todo/Task·작업 의존성·입력 큐·백그라운드 작업/알림 | partial |
| git | 작업 디렉터리·worktree·브랜치·변경 이력·복구 | pending |
| editor | LSP·IDE 통신·파일 변경 알림·진단·심볼/정의/참조 | pending |
| frontends | CLI interactive/headless·구조화 입출력·앱용 상태/이벤트·LVRS UI 바인딩 | pending |
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
