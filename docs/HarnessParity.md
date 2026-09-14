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
| subagents | 전문 에이전트·부모 컨텍스트 fork·모델/도구/권한 범위·백그라운드 알림 | pending |
| teams | 동일 프로세스/별도 프로세스 팀·mailbox·작업·권한 전달·worktree | pending |
| mcp_client | stdio·Streamable HTTP·legacy SSE·초기화·버전/기능 협상·재연결·인증 | partial |
| mcp_features | 도구·리소스·프롬프트·roots·sampling·elicitation·진행/취소·구독·tasks | partial |
| mcp_server | iiLocalLLM 및 앱 기능을 MCP로 제공·세션 격리·권한·동시성·구조화 결과 | partial |
| api | C++ SDK·기존 native IPC·HTTP/SSE 에이전트 API·OpenAI 도구 호환 | partial |
| discovery | 앱 manifest·MCP/API 자동 인식·기능 협상·tool search·지연 공개 | pending |
| skills | SKILL.md·메타데이터·인라인/fork 실행·허용 도구·검색·설치 | pending |
| plugins | manifest·명령/스킬/에이전트/훅/MCP/LSP 등록·버전/캐시·설치/갱신 | pending |
| hooks | 전체 생명주기·C++ 콜백·명령·HTTP·모델/에이전트·입력 변경·결과/차단 | partial |
| tasks | 계획·Todo/Task·작업 의존성·입력 큐·백그라운드 작업/알림 | pending |
| git | 작업 디렉터리·worktree·브랜치·변경 이력·복구 | pending |
| editor | LSP·IDE 통신·파일 변경 알림·진단·심볼/정의/참조 | pending |
| frontends | CLI interactive/headless·구조화 입출력·앱용 상태/이벤트·LVRS UI 바인딩 | pending |
| remote | 원격/bridge 실행·인증·연결 복원·메시지 라우팅 | pending |
| settings | 프로젝트/사용자/관리 설정 우선순위·기능 gate·환경·migration | pending |
| observability | 구조화 로그·실행 trace·사용량/비용·성능·오류 진단 | pending |
| iisacc_apps | Society·Dreamscapes·Congregation·Thinking Space 실제 consumer 연동 검증 | pending |
| packaging | 공개 헤더·CMake export·daemon/CLI·설치 consumer·플랫폼 검증 | partial |
| conditional | 분석본의 내부/조건부 기능: 실제 구현 확보 범위와 iiLocalLLM 대응을 개별 검증 | pending |

## 현재 구현 순서

0.4.0에서 C++ stdio 클라이언트, 도구·리소스·프롬프트, roots, 진행·취소, 호스트 요청 콜백 및 에이전트 어댑터를 추가했다. 프로토콜 버전 2025-11-25/2025-06-18/2025-03-26을 지원하며, C++ 내장/POSIX stdio 서버와 연결별 로컬 에이전트 실행도 제공한다. 0.7.0에서는 공통 C++ 클라이언트에 Streamable HTTP, bearer/header 공급자, SSE 복원과 세션 404 재초기화를 연결했다. legacy SSE·OAuth·HTTP 서버·자동 앱 발견과 최신 규격은 남아 있다. 2025-03 배열 수신·응답 결합을 양쪽에 구현했다. 계약과 교차 검증 절차는 [MCP.md](MCP.md), [MCPHTTP.md](MCPHTTP.md), [MCPServer.md](MCPServer.md)에 기록한다.

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
