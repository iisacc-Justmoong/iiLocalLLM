# 구현 검증 기록

## 2026-09-14 백그라운드 셸과 실행 중 API/MCP 제어 (0.12.0)

C++ `ShellTasks`에 실제 백그라운드 Bash, TaskOutput·TaskStop·ShellTaskList, 소유 대화 격리, 원시 출력 보존·바이트 페이징, 시간/출력/동시 실행 상한과 정상 종료·재시작 복구를 구현했다. 매 모델 턴의 현재 상태, 인증된 HTTP/native IPC, 얇은 CLI와 MCP에 연결한다. 기존 Qt QProcess·QLockFile·QSaveFile과 표준 C++ 스레드를 재사용하며 생산 Python 실행기나 새 런타임 의존성을 추가하지 않았다.

| 검증 | 최종 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 빌드 성공, **49/52 통과**, 370.22초. 실패 항목은 아래에 그대로 기록 |
| ASan·UBSan | llama 비활성 Debug 전체 **33/33 통과**, 67.15초. 아래의 명시적 ASan 실행 조건 적용 |
| 셸 C++ 회귀 | 실제 프로세스와 자식 그룹 종료, 완료/중단 중 최종 출력, 조회만 취소, 시간/출력/용량 제한, 세션 격리, 파일 보호·페이지화, 손상 거부·복구, 비정상 종료 코드의 null 처리. 9개 동작 사례 |
| 새 설치 소비자 | 별도 `shell-stage`와 `shell-consumer/build`에서 **17/20 통과**, 232.28초. 설치된 C++ 셸 실행·출력·중단·재시작 기록 포함 |
| 설치된 API·CLI·공식 MCP | **3/3 통과**. API·CLI 28개 수락 조건. 공식 Python MCP 1.26.0의 stdio/HTTP에서 16개 도구, 백그라운드 시작·출력·중단, 연결 격리와 호스트 비활성화 확인 |
| 실제 Qwen3 8B 셸 실행 | 소스 **통과, 62.91초**. 설치본 **통과, 62.87초**. 아래 eager 조건에서 실제 Bash→TaskOutput→임의 파일 값 답변 확인 |
| 설치·ABI·로더 | 실제 `shell-stage/lib/libiiLocalLLM.0.12.0.dylib` 로드. 소스/설치 UUID `302A6AC5-F8FA-35F1-B279-35A1B78A442A` 및 SHA-256 일치. daemon·CLI·MCP 버전 0.12.0. iillm은 iiLocalLLM/llama/ggml을 링크하지 않음 |

최종 Release 실패는 `iiLocalLLM.tasks_inference`, `iiLocalLLM.tasks_catalog_discovery`, `iiLocalLLM.discovery_inference`이다. 설치 소비자 실패는 `iiLocalLLM.installed_tasks_inference`, `iiLocalLLM.installed_tasks_discovery`, `iiLocalLLM.installed_discovery_inference`이다. 전체 결과와 별도 재실행은 서로 다른 관측이며 합쳐서 전체 통과로 표시하지 않는다. 모델별 실제 도구 실행과 의도한 저장 값·출력 소비를 검증하며, 잘못된 답이나 빈 모델 턴을 호스트가 대신 만들어 통과시키지 않는다.

0.5B 모델은 TaskGet 호출 없이 답하거나 MCP ToolSearch 뒤 실제 도구 실행을 생략했다. 8B 모델은 지연 공개 검사에서 TaskGet 정의를 검색한 뒤 빈 모델 턴을 반환했다. 세 실패는 새 셸 수락 검사의 성공과 별도로 남아 있다.

Qwen3 8B Q4_K_M은 `model://qwen3-8b-q4`, 컨텍스트 8,192, 턴당 2,048토큰·최대 6턴, seed 0, temperature 0.7·top_p 0.8·top_k 20, `/no_think`, `tool_grammar:false`로 검사했다. 셸 제어 도구는 이 수락 검사에서 처음부터 제공한다. 모델은 정확히 한 번 `sleep 0.1; cat observation.txt`를 백그라운드로 실행하고 실제 반환된 작업 ID로 TaskOutput을 호출해야 한다. 임의 파일 값은 프롬프트와 상태 미리보기에 없으며 최종 답·완료 상태·도구 결과 쌍까지 검사한다. 기본 지연 공개나 광범위한 자율 작업의 성공 증거로 확대하지 않는다. 모델의 고정 원본 revision·크기·SHA-256은 [Tasks.md](Tasks.md)에 있다.

초기 ASan 기본 설정의 전체 결과는 **32/33**, 64.34초였다. daemon의 첫 셸이 실행 직전 종료되는 실패를 단독·격리 실행에서도 재현했다. macOS 27의 충돌 보고서는 `fork` 자식의 `_objc_atfork_child → wrap_free → ASan StackStore → os_unfair_lock` 내부 잠금 오류를 기록한다. 신호 호출 추적에서는 SDK 정리가 시도되기 전에 해당 프로세스가 이미 사라졌음을 확인했다. 생산 실행기의 중단 로직이나 시스템 보호 설정을 바꾸지 않았다.

최종 sanitizer 실행은 `ASAN_OPTIONS=malloc_context_size=0`을 명시한다. 이 옵션은 할당/해제 시 보관하는 호출 스택 깊이이며, 해당 실행에서는 할당 이력 스택 진단을 포기한다. [Sanitizer 공식 옵션 설명](https://github.com/google/sanitizers/wiki/SanitizerCommonFlags)을 확인했고, 같은 설정의 별도 오류 probe가 heap-use-after-free와 signed integer overflow를 각각 비정상 종료로 감지했다. ASan·UBSan 계측은 유지한다. 이 조건의 통과를 기본 sanitizer 설정의 통과로 바꾸어 해석하지 않는다. 재현 로그와 축약 충돌 프레임은 `build/shell-sanitizer-first-*`, `build/shell-sanitizer-daemon-focused-tests.log`, `build/shell-sanitizer-crash-diagnosis.json`, `build/shell-sanitizer-probe-result.json`에 있다.

TDD에서는 공개 헤더 부재, 손상된 종료 코드의 재수용, API 대기 기한 누락, 중단 중 마지막 출력 누락, 비정상 종료 시 정의되지 않은 종료 코드 노출을 확인했다. 각각 구현·재검증으로 수정했다. API 선택 인수 `block`을 읽다가 null을 삽입하던 회귀도 수정하고 실제 daemon에서 확인했다. 근거는 `build/shell-red-build.log`, `build/shell-recovery-red-tests.log`, `build/shell-deadline-red-tests.log`, `build/shell-focused-tests.log`, `build/shell-final-output-red-tests.log`, `build/shell-exit-code-red-tests.log`이다.

종료 코드 보완 전 전체 실행은 Release **45/52**, 492.95초, 설치 소비자 **17/20**, 228.86초였다. Release에서 MCP 시험 상대 연결 실패 3건과 8B RAM 보호 거절 1건, 모델의 도구 호출·검색 실패 3건을 관측했다. MCP 연결 3건만 별도 대조한 실행은 **3/3**, 8.62초였다. 이후 최종 코드로 전체 검사를 다시 수행한 값이 위 표이며, 초기 실패는 `build/shell-pre-exit-code-*`와 `build/shell-final-focused-tests.log`에 보존한다.

최종 라이브러리 SHA-256은 `c1db1f71442599ed3a50fb0e79fed7227e6483c2ae6c62ff3093cdaf00fd7ece`이다. SDK의 별도 설치·외부 소비자 검증이며 기본 SDK 위치나 Society·Dreamscapes 기기 앱을 재배포한 결과가 아니다. iPhone은 사용자 지시로 제외한다. 자동 배경 전환, 지속 셸 환경, Windows/모바일, 백그라운드 에이전트·원격 실행, 입력 큐·완료 알림·팀 mailbox 및 MCP 비동기 tasks 규격은 남아 있다. 전체 하네스 목표는 진행 중이다.

계약은 [BackgroundTasks.md](BackgroundTasks.md), 범위는 [HarnessParity.md](HarnessParity.md)이다. 최종 증거는 `build/shell-final-{release,sanitizer,consumer}-tests.log`, 해당 JUnit XML·LastTest, `build/shell-installed-wire/`, `build/shell-linkage.json`, `build/shell-installed-loader.log`, `build/shell-verification.json`에 보관한다.

## 2026-09-14 영속 작업·Todo 및 에이전트/API/MCP 연결 (0.11.0)

C++ `TaskStore`에 일곱 작업·Todo 도구, 양방향 의존 관계의 원자적 갱신·삭제, 순환 검사, 담당자 선점, revision 충돌 검사와 재시작 보존을 구현했다. Engine의 현재 작업 컨텍스트와 게시 전 TaskCreated/TaskCompleted 훅, 인증된 HTTP/native IPC, 얇은 CLI 및 MCP 서버에 연결했다. Qt의 잠금·원자적 파일 저장과 기존 JSON Schema/ToolRunner를 재사용하며 새 생산 의존성은 없다.

| 검증 | 최종 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 빌드 성공, **44/50 통과**, 525.60초. 실패 6건과 후속 대조 결과는 아래에 구분 |
| ASan·UBSan | llama 비활성 Debug 전체 **32/32 통과**, 53.42초. Unicode 보존 및 아래 테스트 격리 수정 포함 |
| 테스트 격리 수정 후 Release | 전송·공식 MCP stdio/HTTP·실제 MCP 추론의 영향 범위 **5/5 통과**, 27.76초. 전체 50개 실행과 별도 기록 |
| 작업 C++ 회귀 | 7개 동작 사례. 스레드 16개 생성·12개 선점 경쟁, 별도 프로세스 8개 생성·선점 경쟁, 의존 관계·취소·손상 파일·훅 차단·Plan 정책·resume·fork·Unicode 담당자 재조회 포함 |
| 새 설치 소비자 | 전체 **13/18 통과**, 271.62초. RAM 보호로 기동이 거절된 8B eager와 MCP 연결 검사의 별도 재실행 **2/2 통과**, 142.08초. 모델 응답/검색 실패와 원래 전체 결과는 유지 |
| 설치된 API·CLI·공식 MCP | 최종 설치본에서 **3/3 통과**. API·CLI 20개 수락 조건, 공식 Python MCP 1.26.0의 stdio/HTTP에서 작업 생성·선점·완료·Todo·revision 충돌·연결 격리·호스트 비활성화 검증 |
| Qwen3 8B 실제 작업 실행 | eager에서 TaskGet→임의 설명 답변, TaskCreate 한 건, TaskUpdate의 담당자·상태와 기존 제목·설명 보존 **통과**, 166.61초. 아래의 명시적 운용 조건 사용 |
| 설치·ABI·로더 | `task-stage`의 0.11.0 라이브러리 실제 로드. 소스/설치 UUID `B2C82223-4256-3693-8594-FAD3616FACD4` 및 SHA-256 일치. 세 실행 파일 모두 0.11.0. iillm은 iiLocalLLM/llama/ggml을 링크하지 않음 |

고정 Qwen2.5 0.5B Q4_K_M은 TaskGet을 호출하지 않고 미리보기 제목을 설명이라고 답했다. 기존 ToolSearch 다음 실제 MCP 도구 호출을 생략하는 실패도 유지한다. 이 모델의 검사 조건이나 관측값을 바꾸지 않았다.

추가 Qwen3 8B Q4_K_M의 기본 추론 조건(temperature 0.6, top_p 0.95, top_k 20, seed 0, 턴당 2,048토큰, 컨텍스트 8,192)에서는 eager 검사가 첫 턴의 출력 한도로 실패했다. deferred 검사는 실제 ToolSearch→TaskGet과 임의 설명 재현, TaskCreate까지 통과했으나 TaskUpdate에서 담당자 대신 제목에 잘못된 문자열을 저장하고 성공했다고 답하여 실패했다. 초기 두 결과는 각각 160.61초·218.73초이며 `build/task-catalog-tests.log`에 보존한다. 도구 호출 성공과 사용자 의도에 맞는 결과는 별도 판정이다.

문서화된 `/no_think` 사용자 입력과 temperature 0.7·top_p 0.8만 적용한 중간 대조에서도 생성 문법이 켜진 eager는 잘못된 수정 인수, deferred는 검색 후 빈 모델 턴으로 실패했다. 이 실행은 `build/task-no-think-schema-tests.log`에 보존한다. 포함된 upstream 변환기의 선택 필드 순서 제한을 코드에서 확인했으며, 이후 모델 로딩의 `tool_grammar` boolean 선택을 추가했다. 기본값 true를 유지하고 false에서도 실행 전 파싱·스키마·권한 검사는 동일하다. 옵션 누락 구현의 실패는 `build/task-grammar-red-tests.log`, 잘못된 타입 거절은 실제 llama runtime 검사에 포함한다.

최종 Qwen3 조건은 `/no_think`, temperature 0.7·top_p 0.8·top_k 20·seed 0, `tool_grammar:false`, 턴당 2,048토큰, 컨텍스트 8,192이다. 요청 값은 따옴표로 구분하고 수정 후 기존 제목·설명도 보존하도록 검사를 강화했다. 일곱 도구가 모두 공개된 eager는 실제 호출과 저장 값 검증에 통과했다. deferred는 ToolSearch 후 빈 모델 턴으로 **실패**, 57.41초이다. 운용 조건과 프롬프트가 함께 달라졌으므로 모든 초기 실패의 원인이 생성 문법 하나라고 결론 내리지 않는다. 모델 답이나 도구 호출을 호스트가 만들어 넣지 않았다. 특정 명시 프롬프트의 수락 검사이며 광범위한 자율 작업 성능을 증명하지 않는다. 모델 해시·원본 revision 및 재현 설정은 [Tasks.md](Tasks.md)에 있다.

최종 전체 실행의 다른 실패는 `mcp_official` 초기화 타임아웃, `configured_mcp_inference`의 peer 연결 실패, Qwen2.5의 `tasks_inference`·`discovery_inference` 및 `chat` 원문 재현이다. 앞의 MCP 두 항목과 chat만 단독 대조한 실행은 **3/3 통과**, 40.44초이다. 이는 전체 44/50 기록을 통과로 바꾸지 않으며, 임의 값이 달라진 재실행의 chat 성공으로 최초 원문 재현 오류가 해결되었다고 주장하지 않는다. 해당 근거는 `build/task-final-focused-tests.log`에 있다.

설치 소비자 전체 실행의 실패 다섯 건은 `installed_tasks_inference`, `installed_tasks_catalog`, `installed_tasks_discovery`, `installed_configured_mcp_inference`, `installed_discovery_inference`이다. 8B eager는 `Insufficient available RAM or no idle model can be evicted`로 기동이 거절되었다. 메모리 보호 조건을 바꾸지 않은 단독 대조에서 같은 설치본의 작업 조회·생성·수정이 통과했다. MCP 연결도 이 대조에서 통과했다. 나머지 세 모델 검사의 실패는 유지한다. 로그는 `build/task-installed-focused-tests.log`이다.

TDD에서 누락 Engine 인터페이스의 빌드 실패를 확인했다. 이후 fork의 빈 현재 작업 상태 누락과, 128개 이모지 담당자가 입력을 통과해도 UTF-16 길이 재검사 때문에 저장 후 읽기를 실패하는 문제를 각각 재현했다. 빈 상태 컨텍스트와 동일 JSON Schema 재검증으로 수정했다. 회귀 로그는 `build/task-fork-regression-red.log`, `build/task-unicode-red-tests.log`, `build/task-unicode-green-tests.log`이다. 초기 테스트 코드의 임시 QJsonValueRef 수명 오류도 수정했으며 마지막 sanitizer 전체 검사에 오류가 없다.

첫 전체 실행의 agent_transport 시간 제한 검사에서는 예상 504 대신 400이 관측되었다. 세션 생성 응답 검증을 추가한 후 단독 재실행은 통과했다. 부하에 의한 초기 세션 생성 지연은 추정이며 원인으로 확정하지 않는다. 이후 sanitizer 실행에서는 큰 응답의 버퍼 초과보다 100ms 요청 제한이 먼저 발생했다. 시간 제한·종료 검사는 기존 100ms를 유지하고, 버퍼 초과 검사는 별도 3,000ms fixture로 분리하여 `resource_limit`을 검증한다. 제품 전송 코드는 변경하지 않았다.

마지막 sanitizer 재빌드 후 기존 앱 발견 테스트의 비동기 완료 가정도 한 차례 실패했다. 자동 갱신은 도구 목록을 먼저 게시하고 잠금 밖에서 이전 연결을 닫으므로 두 조건을 각각 기다리도록 테스트를 수정했다. 실제 제거와 이전 도구 실행 거절 검증은 유지한다. 제품 연결 코드를 변경한 것은 아니다. 실패 원문은 `build/task-app-close-race-red-tests.log`, Release 대조는 `build/task-app-close-release-tests.log`에 보존하며 위 sanitizer 결과는 수정 후 전체 실행이다.

공식 MCP stdio 테스트에서는 실행 중인 Society의 도구 다섯 개가 기대 목록에 추가되는 실패를 확인했다. 공식 Python 클라이언트의 제한된 환경 변수 상속 때문에 테스트의 앱 등록 경로가 자식 서버에 전달되지 않았다. stdio/HTTP 양쪽에 전용 앱 등록·임시 경로를 명시적으로 전달했고, 기대 도구 13개 검증은 유지했다. 실패는 `build/task-stdio-isolation-red-tests.log`, 버퍼 초과와 함께 관측한 실행은 `build/task-fixture-isolation-red-tests.log`에 있다. 수정 후 sanitizer 전체 32개와 Release 영향 범위 5개가 통과했다. 실제 모델을 사용하는 두 MCP 전송 검사도 이 5개에 포함하며 로그는 `build/task-final-fixture-release-tests.log`이다.

이번 변경은 SDK와 Workspace 안의 별도 설치본에 한한다. 기본 SDK 경로 및 Society·Dreamscapes 기기 앱을 재배포한 결과가 아니다. 작업 상태는 실행 증거를 대체하지 않는다. 계획 모드 전환·승인 흐름, 백그라운드 실행/출력/종료, 입력 큐, 알림, 팀 mailbox 등은 여전히 미완료이며 전체 하네스 목표를 완료로 표시하지 않는다.

계약은 [Tasks.md](Tasks.md), 범위는 [HarnessParity.md](HarnessParity.md)이다. 증거는 `build/task-final-{release,sanitizer,consumer}-tests.log`, 해당 JUnit XML, `build/task-installed-wire/`, `build/task-linkage.json`, `build/task-installed-loader.log`, `build/task-verification.json`에 보관한다.

## 2026-09-14 실행 중인 앱 MCP 연결과 실제 모델 입력 검증 (0.10.0)

현재 단계는 데스크톱 POSIX 앱 등록·발견·인증·Qt 주 스레드 도구 호출이다. Society·Dreamscapes의 실제 컨트롤러를 연결한다. 전체 하네스와 모든 앱/플랫폼의 완료를 뜻하지 않는다.

| 검증 | 최종 결과 |
|---|---|
| SDK Release CTest | 45/46, 176.01초. 기존 `iiLocalLLM.discovery_inference` 한 건 실패 유지 |
| ASan + UBSan, llama 비활성 | 31/31, 53.64초 |
| 새 stage의 외부 C++ consumer | 13/14, 33.68초. 동일한 `installed_discovery_inference` 실패 유지. 새 앱 등록·HTTP·QObject 호출·삭제 consumer 통과 |
| Society 현재 작업 트리 | 전체 20/20, 140.98초. 실제 앱 탐색·범위 거절·자동 발견·제거·비활성 및 네이티브 Qwen 호출 포함 |
| Dreamscapes 현재 작업 트리 | 전체 9/9, 135.16초. 구조화 결과 수정 후 실제 MCP 테스트도 3.76초에 통과 |
| 커밋할 코드만 추출한 독립 앱 빌드 | Society MCP 14.00초, Dreamscapes MCP 7.80초에 각각 통과. 기존 미커밋 제품 변경에 의존하지 않음 |
| 모델이 실제 앱 값을 소비하는지 | Qwen2.5 0.5B Q4_K_M이 발견한 Society status 도구를 호출하고 새 컨테이너 ID를 최종 응답에 포함. ID는 프롬프트에 주지 않음. eager 도구 하나와 명시 허용 정책으로 검증 |
| 설치·ABI·로더 | SDK/daemon/CLI 0.10.0. 소스와 stage의 UUID `B1E9F032-0C61-32E7-B9A9-17E8372ADFEA`, SHA-256 일치. 외부 consumer가 실제 `local-app-stage/lib/libiiLocalLLM.0.10.0.dylib`를 로드. 얇은 iillm은 iiLocalLLM/llama/ggml을 링크하지 않음 |

실제 Qwen 검사에서 최초에는 MCP 호출이 성공해도 모델이 `Society state`라는 설명만 읽었다. `structuredContent`가 모델용 텍스트에서 누락된 원인이었다. 서버의 JSON text block과 클라이언트의 구조화 결과 전달을 수정했다. stdio/HTTP 회귀의 실패를 먼저 재현하고 통과시켰으며, 같은 JSON이 이미 있으면 공백 형식과 무관하게 중복하지 않는다. 공식 Python MCP 1.26 클라이언트는 파일 본문과 추가 JSON block을 각각 검증한다.

초기 실행에는 공식 MCP peer 초기화 타임아웃과 Dreamscapes의 첫 GUI 시작 지연이 각각 있었다. 원래 로그를 보존하고 해당 검사와 최종 전체/독립 빌드를 다시 실행했다. 모델 폴더의 텍스트 자동 분류와 이미지 크기의 8픽셀 정렬도 실제 앱 규칙에 맞게 테스트에 반영했다. 최종 SDK의 유일한 실패는 검색 후 도구를 실행하지 않는 기존 소형 Qwen 경로이다.

Dreamscapes의 검사는 기존 생성 프로세스 fixture로 실제 앱 큐·생성 결과 PNG·작업 ID·실행/대기 취소를 확인한다. 실제 확산 모델의 생성 품질·성능 검증으로 확대하지 않는다. 이번 작업은 별도 stage와 앱 build를 사용하며 기본 SDK 설치 및 기존 디바이스 앱 bundle을 교체하지 않는다. iOS·Android·Windows 발견, Congregation·Thinking Space 및 광범위한 자율 앱 작업은 남아 있다.

재현과 계약은 [LocalApplications.md](LocalApplications.md)에 있다. 로컬 증거는 `build/local-app-verification.json`, `build/local-app-verified-*-tests.log`, `build/local-app-final-consumer-tests.log`, 제품별 `build/local-mcp/`에 보관한다.

## 2026-09-14 MCP 설정 연결과 대화별 도구 검색 (0.9.0)

C++ `McpConnections`에 호스트가 지정한 stdio/Streamable HTTP 설정 연결, 환경변수 치환, 서버별 실패 격리, 도구 목록 변경 알림·연결 복구, 명시적 설정 reload를 구현했다. `ToolSearch`는 대화별 선택·원본 JSONL 복구·턴 snapshot·스키마/연결 변경 무효화를 제공한다. 인증된 상태 API, 얇은 CLI, MCP 서버의 설정 기반 도구 중계도 연결했다. 기존 Qt와 MCP/Schema 코드를 재사용하며 새 생산 런타임 의존성은 없다. 환경은 Apple M1 Max / macOS 27 / Qt 6.8.3이다.

| 검증 | 최종 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 전체 타깃 빌드 성공, **44/45 통과**, 150.27초. 실제 추론 14개 항목 포함. 검색 후 연속 호출 1건 실패 |
| ASAN/UBSAN | llama 비활성화 Debug 전체 빌드 및 **30/30 통과**, 44.67초. sanitizer 오류 없음 |
| C++ 검색·연결 회귀 | 10개 동작 사례 통과(QTest 초기화·정리 포함 12 passed). 선택 격리·resume/fork·압축·한 턴 조기 호출·변경 스키마·권한·원자적 교체·설정 검증·HTTP 알림·stdio 재연결·호스트 토큰 보존 검사 |
| 실제 설정 기반 MCP 추론 | Qwen2.5 0.5B Q4_K_M + 공식 Python MCP SDK 1.26.0 서버. eager 모드에서 고정/임의 파일 값 2개 모두 실제 도구 호출·진행 이벤트·최종 답 검증. 소스와 설치본 모두 통과 |
| 별도 설치 소비자 | `build/discovery-stage`, `build/discovery-consumer/build`에서 **12/13 통과**, 29.13초. 설치본에서도 동일한 검색 연속 호출 1건 실패 |
| API·CLI·중계 | 실제 daemon의 HTTP/native IPC/CLI 상태 조회·인증 거부·원격 reload 거부, 공식 SDK의 stdio/HTTP MCP 도구 중계·허가 없는 도구 거부 통과. 설치된 daemon/CLI/HTTP MCP 중계도 별도 통과 |
| 설치·로더·ABI | 실제 `discovery-stage/lib/libiiLocalLLM.0.9.0.dylib` 로딩, 소스/설치 UUID 일치. iillm·iiLocalLLMD·iillm-mcp 0.9.0 확인. 얇은 iillm은 iiLocalLLM/llama/ggml을 링크하지 않음 |

**남은 실패:** `iiLocalLLM.discovery_inference`와 설치본의 대응 검사다. 고정 Qwen2.5 0.5B 모델은 ToolSearch를 호출하고 도구를 선택하지만 두 번째 모델 턴에서 검색 안내를 최종 답으로 끝낸다. 실제 `mcp__fixture__read_secret` 호출을 하지 않으므로 검사 실패를 유지했다. 첫 구현은 긴 검색 스키마 JSON을 답에 복사했다. 전체 스키마를 구조화 데이터와 다음 턴 tools에 보존하고 모델 본문을 짧게 바꾼 뒤에도 연속 호출 능력 검증은 통과하지 못했다. 모델 답을 대신 만들거나 검색 이후 호출을 호스트가 강제로 삽입하지 않았으며, 검사를 성공으로 표시하지 않았다.

같은 모델에서 `deferTools=false` / `--agent-mcp-eager` / `--mcp-eager`로 도구를 처음부터 제공하는 경로는 실제 MCP 호출과 최종 값 검증에 성공했다. 이는 자동 검색 연속 호출의 성공 증거가 아니므로 구분한다. 검색 기능의 모델별 적합성, 앱 manifest/설치 앱 자동 발견, 전체 설정 계층·OAuth·legacy SSE·tasks 및 실제 Society/Dreamscapes endpoint 연동은 남아 있다. 전체 하네스 목표는 진행 중이다.

TDD의 누락 헤더 빌드 실패, Bearer 헤더 연결 실패와 네이티브 검색 실패를 보존했다. 마지막 검토에서는 종료 시 이미 끝난 갱신 요청의 호스트 취소 토큰까지 취소하는 문제를 재현했다. 연결을 직접 종료하고 호스트 토큰은 변경하지 않도록 고친 후 해당 회귀 검사가 통과했다. 그 수정까지 포함하여 위 전체 빌드·검사를 다시 실행했다. 소스/설치 검증에서는 DYLD_LIBRARY_PATH, DYLD_FRAMEWORK_PATH, DYLD_FALLBACK_LIBRARY_PATH, LIBRARY_PATH를 제거했다. 별도 stage 검증은 전역 SDK 설치나 제품 앱 재배포를 의미하지 않는다.

사용 계약은 [ToolDiscovery.md](ToolDiscovery.md)이다. 증거는 `build/discovery-final-{release,sanitizer,consumer}-tests.log`, 대응 JUnit XML·LastTest 기록, `build/discovery-linkage.json`, `build/discovery-installed-loader.log`, `build/discovery-installed-api-result.json`, `build/discovery-installed-mcp-result.json`, `build/discovery-verification.json`에 보관한다. 초기 실패는 `build/discovery-red-build.log`, `build/discovery-tests-first.log`, `build/discovery-focused-tests.log`, `build/discovery-cancellation-red.log`이며 수정 대조는 `build/discovery-cancellation-green.log`에 있다. 전체 검사는 `cmake --build build --parallel`과 `ctest --test-dir build --output-on-failure`로 재현하며 공식 SDK와 고정 모델 경로는 CMake의 선택 검사 옵션으로 지정한다.

## 2026-09-14 인증된 MCP HTTP 서버 (0.8.0)

C++ `mcp::HttpServer`와 `iillm-mcp --http-port`를 추가했다. 인증 principal에 묶인 세션, 요청별 SSE·재개 기록, 독립 알림 GET, 명시적 취소, 역방향 요청, 구형 배열, 용량·수명 제한을 기존 ServerSession에 연결했다. cpp-httplib 0.54.1과 Qt Core를 재사용하며 생산 Python 서버를 추가하지 않았다. 검증 환경은 Apple arm64 / macOS 27 / Qt 6.8.3이다.

| 구분 | 최종 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 빌드 성공, **39/40 통과**, 132.81초. 실패 1건은 아래 모델 원문 재현 검사 |
| 새 HTTP 서버 검사 | 서버·독립 wire·CLI·공식 SDK·실제 HTTP 추론 **5개 CTest 항목 모두 통과** |
| 독립 stdlib HTTP peer | **7개 사례 통과**. 인증·Origin·Host·UTF-8·상한·CORS, POST 재실행 없는 GET 복원, 동일 progressToken 분리, 역방향 취소, 구형 배열 오류 보존, 기록/세션 만료 |
| C++ HTTP 서버 검사 | 앱·대화 분리, 8개 동시 역방향 RPC와 중첩 ping, SSE 용량 2에서도 취소, 바인딩 충돌·8회 즉시 종료/재시작·콜백 합류 통과 |
| CLI 자격 증명 | 비공개 파일·ID/토큰 전체 문자열·중복 토큰·symlink·작업 폴더 경계·state 잠금·다른 앱의 세션 사용 거부 통과. 잘못된 인증 설정은 Service 생성 전에 실패 |
| 공식 Python SDK | **MCP 1.26.0**. 실제 C++ HTTP CLI에서 파일 읽기/쓰기, 정책·스키마·경로 거부, Bash와 자식 프로세스 취소, 이후 연결 사용 통과 |
| ASAN/UBSAN | llama 비활성화 Debug 전체 **27/27 통과**, 42.73초. sanitizer 오류 및 SDK 빌드 경고 없음 |
| 새 설치 소비자 | `build/mcp-http-server-stage`, 독립 `build/mcp-http-server-consumer/build`에서 **10/10 통과**, 35.15초. 공개 HttpServer ABI와 앱별 ToolRegistry factory 포함 |
| 소스 HTTP 실제 추론 | Qwen2.5 0.5B Q4_K_M → Read → 임의 파일 값 `LOCAL_9c8944191291` 포함 답변. 2턴, 45 생성 토큰, 진행 이벤트 11개, 대화 기록 검증 |
| 설치 HTTP 실제 추론 | 새 설치 iillm-mcp → 같은 모델 → Read → 다른 임의 값 `LOCAL_53737beba11c` 포함 답변. 2턴, 41 생성 토큰, 진행 이벤트 11개, 대화 기록 검증 |
| 로딩·CLI | 경로 override 제거 후 stage의 `libiiLocalLLM.0.8.0.dylib` 로딩 확인. 소스/설치 UUID `EDD90334-2406-361B-B1B0-7B95C7946FD5` 일치. 세 실행 파일 모두 0.8.0이며 iillm의 Core/Network 전용 링크 유지 |

처음에는 헤더가 없어 테스트 빌드가 실패했다. 독립 HTTP peer는 이후 취소 직후 스트림을 닫으면 구형 배열에 남은 오류 응답과 역방향 취소 알림이 사라지는 두 실패를 재현했다. JSON-RPC 밖의 channel과 cancelledRequestId를 보존하고, 핸들러 종료 시 앞선 메시지 뒤에 스트림 종료 이벤트를 전달하도록 고쳤다. wire 검사 조건을 완화하지 않았다.

즉시 close 후 재시작 검사에서는 accept 스레드의 준비/종료 상태 정리가 부족함을 확인했다. 이어 최초 전체 실행은 Release 39/40, sanitizer 26/27로 모두 포트 충돌 검사에 실패했다. cpp-httplib의 SO_REUSEPORT 기본값 때문에 두 서버가 동일 포트에 바인딩되었다. 상태를 가지는 MCP 세션이 다른 서버로 배분되지 않도록 포트 공유를 끄고, 바인딩 실패와 종료 후 같은 객체를 재사용하도록 보강했다. 초기 오류와 최종 검증 로그를 각각 보존했다. 중간 C++ 알림 대기 테스트는 QTRY가 조건을 재평가하면서 알림 큐를 두 번 비워 실패하여, 수신 여부를 보존하는 대기로 수정했다.

최종 Release 실패는 `agent_local_inference`다. Read의 실제 결과·Tool 기록은 `LOCAL_fd2e6cbfe577`였지만 모델은 `This is a secret message.`를 포함한 다른 문장을 답했다. 이 실행은 compactions=0, prompt_tokens=730, generated_tokens=40이다. 이번 변경에서 Engine·LlamaRuntime·해당 검사 소스는 바꾸지 않았다. 아래 0.5/0.6 기록에도 소형 모델의 원문 재현 실패가 있지만, 그것을 이번 전체 검사 통과의 대체 근거로 삼지 않는다. 실패 후 동일 검사를 통과할 때까지 반복하지 않았고 모델 답변·기대값·검사 조건도 바꾸지 않았다. 별도 소스/설치 HTTP 추론 통과와 전체 39/40을 구분한다.

계약은 [MCPHTTPServer.md](MCPHTTPServer.md)에 있다. 원격 TLS/proxy·OAuth·legacy SSE·최신 규격·tasks·실제 Society/Dreamscapes 발견/연동은 남아 있으며 전체 하네스 목표는 계속 진행 중이다. 이 stage는 전역 SDK 설치나 제품 앱 재배포를 의미하지 않는다.

증거: `build/mcp-http-server-final-{release,sanitizer,consumer}-tests.log`, 대응 JUnit XML과 LastTest 로그, `build/mcp-http-server-linkage.json`, `build/mcp-http-server-installed-loader.log`, `build/mcp-http-server-{native,installed-native}-result.json`, `build/mcp-http-server-verification.json`. 실패 대조는 `build/mcp-http-server-wire-red.log`, `build/mcp-http-server-lifecycle-tests.log`, `build/mcp-http-server-{release,sanitizer}-tests.log`에 있다. 새 실행 파일을 포함한 전체 빌드 후 검사했으며, 중간 타깃 전용 실행을 최종 전체 결과로 사용하지 않았다.

## 2026-09-14 MCP Streamable HTTP 클라이언트 (0.7.0)

C++ 공통 `mcp::Client`에 stdio와 Streamable HTTP 전송을 연결했다. HTTP JSON/SSE, 호스트 자격 증명 공급자, 진행·취소, SSE GET 복원, 세션 404 재초기화와 연결 세대에 묶인 에이전트 도구를 구현했다. Qt 6.8.3 Network를 재사용하며 생산 패키지에 새 런타임 의존성을 추가하지 않았다. 검증 장비는 Apple M1 Max / macOS 27 / Qt 6.8.3이다.

| 구분 | 최종 관측 결과 |
|---|---|
| Release 빌드·전체 CTest | 전체 타깃 빌드 성공, **35/35 통과**, 158.67초. 실제 GGUF·MLX CPU/Metal 및 에이전트·압축 추론 11개 포함 |
| ASAN/UBSAN | llama 비활성화 Debug 빌드에서 **23/23 통과**, 37.86초. stdio/HTTP 클라이언트, MCP 서버, 공식 SDK, API·CLI·daemon 검사 포함. SDK 빌드 경고·sanitizer 오류 없음 |
| 독립 HTTP peer | 12개 동작 사례, QTest 초기화·정리 포함 **14 passed / 0 skipped**. 8개씩 5회, 총 40개 요청에서 역방향 sampling·roots와 sampling 콜백 안의 중첩 ping 검증 |
| 복원·오류 경계 | 초기화 중 SSE 복원, POST 재실행 없는 Last-Event-ID GET, 긴 retry 지연, 개별 요청 기한, 취소·단절, 404 세션 갱신과 이전 도구 차단 통과 |
| 인증·입력 | 401 challenge, 리다이렉트 미추적, 독립 클라이언트 세션, 잘못된 헤더 이름·값, 큰 응답·잘못된 SSE ID, 신뢰하지 않는 인증서 거부 통과. 호스트의 전역 VerifyNone 설정에서도 인증서를 거부 |
| 공식 SDK | Python MCP SDK **1.26.0**의 실제 stdio/Streamable HTTP 서버와 도구·구조화 결과·진행·리소스·프롬프트·역방향 roots 검증. 소스·설치 소비자 모두 통과 |
| 새 설치 소비자 | `build/mcp-http-stage`와 독립 `build/mcp-http-consumer/build`에서 **9/9 통과**, 18.39초. 공개 ABI, HTTP MCP, 실제 로컬 요약·재개 포함 |
| 로더·실행 파일 | 경로 override 제거 후 `build/mcp-http-stage/lib/libiiLocalLLM.0.7.0.dylib` 로딩 확인. 소스·설치 라이브러리 UUID 일치. iillm·iiLocalLLMD·iillm-mcp 모두 0.7.0. 얇은 iillm은 Qt Core/Network·시스템 라이브러리만 링크 |

초기 구현은 서버가 POST 본문을 처리하고 응답 헤더 전에 연결을 끊을 때 같은 요청을 **두 번** 보냈다. 독립 peer의 `droppedPosts == 1` 검사가 먼저 실패했다. Qt의 버퍼링된 업로드 재전송을 피하도록 sequential `QIODevice`와 `DoNotBufferUploadDataAttribute`를 사용하고, 이미 읽힌 업로드의 되감기를 거부했다. 최종 검사에서는 본문 처리 후 단절·응답 헤더 후 단절 모두 POST가 한 번만 관측된다. 원격 작업의 성공 여부는 단절만으로 확정하지 않는다.

동시성 시험 중에는 별도의 시험 서버 한계도 확인했다. Python 표준 HTTP 서버의 TCP 대기열 기본값 5가 동시 RPC·역방향 응답·중첩 요청의 연결 급증을 감당하지 못했다. 서버 기록과 연결 거부를 확인하고 peer 대기열을 128로 설정했다. 요청 수·결과·POST 횟수 조건은 유지했다. 최종 구현은 제한된 HTTP 관리자 풀을 재사용하며, 임시로 도입했던 매 요청 관리자 재생성과 전송 신호 기반 되감기 판정은 제거했다.

최종 입력 검토에서는 이름 끝에 개행이 있는 헤더가 SDK 검증을 통과하고 Qt에서 경고와 함께 제거되는 문제를 재현했다. 전체 문자열을 검사하는 정규식 경계로 바꿔 `InvalidArgument`를 반환한다. 보완 전 전체 실행도 35/35, sanitizer 23/23, 설치 소비자 9/9였지만, 위 표는 이 보완까지 반영한 마지막 전체 실행이다. 최초 누락·중복 POST·동시 연결 실패·개행 헤더 실패 로그를 보존했다.

이번 실행의 소형 모델 원문 답변 검사는 통과했지만, 아래 0.5/0.6 기록의 임의 문자열 재현 실패를 해결한 변경은 아니다. 모델 답변을 관측값으로 덮어쓰거나 검사 조건을 완화하지 않았다. 전체 하네스, legacy SSE·OAuth·2026 규격, MCP HTTP 서버와 실제 Society/Dreamscapes 발견·연동은 미완료로 유지한다. 이번 stage 설치는 전역 SDK 설치나 제품 앱 재배포를 뜻하지 않는다.

공개 사용 계약은 [MCPHTTP.md](MCPHTTP.md), 남은 범위는 [HarnessParity.md](HarnessParity.md)에 있다. 재현 명령은 `cmake --build build --parallel`과 `ctest --test-dir build --output-on-failure`다. 공식 SDK·실제 모델 검사는 해당 CMake 선택 경로를 설정해야 한다. 증거는 `build/mcp-http-final-release-tests.log`, `build/mcp-http-final-sanitizer-tests.log`, `build/mcp-http-final-consumer-tests.log`, 각 `mcp-http-final-*-junit.xml`·`mcp-http-final-*-LastTest.log`, `build/mcp-http-library-uuids.log`, `build/mcp-http-cli-linkage.log`, `build/mcp-http-verification.json`에 보관한다. 실패 대조는 `build/mcp-http-red-build.log`, `build/mcp-http-second-tests.log`, `build/mcp-http-isolated-manager-tests.log`, `build/mcp-http-peer-backlog-tests.log`, `build/mcp-http-header-red-tests.log`에 있다.

## 2026-09-14 네이티브 예산·대화 압축 (0.6.0)

C++ Service의 실제 템플릿·토크나이저 측정, 자동 도구 결과 축소, 여러 묶음의 로컬 모델 요약, 원본 JSONL 보존 체크포인트, 재개·분기, 원문 조회 및 C++/API/MCP 수동 압축을 연결했다. Apple M1 Max / Qt 6.8.3에서 검증했으며 새 런타임 의존성은 없다. 전체 하네스 및 Society·Dreamscapes 제품 앱 통합 완료를 의미하지 않는다.

| 구분 | 관측 결과 |
|---|---|
| Release 빌드 | 전체 타깃 성공. 최종 전체 Release·대상 sanitizer 빌드 성공·경고 없음 |
| 전체 CTest | **32/33 통과**, 112.13초. `iiLocalLLM.agent_local_inference`의 소형 모델 원문 답변 검사 1개 실패 |
| 추가 변경 검증 | 반환값 경고 정리 후 Service 1/1, 수동 압축 HTTP/native/CLI 검사 2/2 통과. 최종 전체 실행은 중복 요약·즉시 모델 해제 회귀 검사도 포함 |
| ASAN/UBSAN | **9/9 통과**, 18.75초. 압축·프로젝트 지침·에이전트·Service·HTTP·API·native transport·MCP stdio·MCP server |
| 설치 소비자 | fresh CMake 구성에서 **8/8 통과**, 17.05초. 공개 압축/체크포인트 ABI와 실제 로컬 요약·재개 포함 |
| 로딩·CLI | 실제 `build/compaction-stage/lib/libiiLocalLLM.0.6.0.dylib` 로딩 확인. iillm은 Qt Core/Network 링크이며 iiLocalLLM/llama/ggml 링크 없음 |
| API 전송 | 소스 daemon에서 14개 수락 검사 통과. 수동 압축의 인증·빈 세션 보존을 HTTP/native IPC/얇은 CLI에서 확인; 프로젝트 지침·실제 Read·재시작·분기 경로 포함 |
| 실제 Qwen 요약 | 창 4,096토큰, 원본 사전 측정 11,186토큰, 요약 4회·생성 136토큰. 저장된 최종 입력 1,378토큰. 원본 13개 메시지 보존, 요약문과 재개 응답에서 임의 식별자 일치 |
| ABI·저장 | SDK/SOVERSION 0.6.0/0.6. JSONL v2, v1 읽기 및 첫 압축 시 원자적 헤더 이행, 후속 체크포인트 append/flush |

원본 사전 측정은 원문 조회 도구를 추가하기 전의 입력이며, 체크포인트의 최종 측정은 그 도구 스키마를 포함한다. 실제 Qwen 검사는 하나의 임의 식별자 유지와 재개를 검증한다. 모든 사실의 충실성을 증명하지 않는다. 설치 소비자에서도 같은 검사에 통과했다.

최초 실제 요약 검사는 11,186토큰을 1,394토큰으로 줄이고 재개 응답에서 식별자를 반환했지만, 요약문 자체는 식별자를 누락해 실패했다. 각 요약 단계에 최신 사용자 요청 원문을 다시 제공하도록 보강한 뒤 새 임의 식별자로 통과했다. 최초 실패 로그는 `build/compaction-native-initial-result.log`와 `build/compaction-native-initial-runtime.log`에 보존했다. 테스트 기준을 완화하거나 모델 답변을 상수로 대체하지 않았다.

최종 전체 회귀의 실패는 `agent_local_inference`의 임의 파일 값 `LOCAL_fcbfcb5686bc`다. 원문이 Tool 메시지에 정확히 기록됐지만 모델은 `This is a secret message.`라고 답했다. 이 실행의 `compactions`는 0이다. 같은 입력·프롬프트·모델로 이전 `build/context-stage/lib/libiiLocalLLM.0.5.0.dylib`를 실제 로드한 비교에서도 같은 답변과 토큰 수(prompt 730, generated 39)가 재현됐다. 기존 소형 모델 원문 재현 문제이며 새 압축 경로에서 생긴 실패로 표시하지 않는다. 비교 소스·결과·실제 로더 기록은 `build/compaction-baseline/compare.cpp`, `build/compaction-baseline-result.log`, `build/compaction-baseline-loader.log`다.

이전 전체 실행도 32/33(148.49초)이었고 당시에는 `chat`의 HTTP 원문 답변 검사가 실패했다. 이후 재실행은 중복 요약과 모델 즉시 해제 시 임시 상태 수명을 수정했기 때문에 수행했다. 원문 답변 검사 기준은 바꾸지 않았으며 최초 HTTP 실패는 `build/compaction-release-tests.log`에 보존했다. 최종 전체 실행의 `chat`, `agent_mcp_inference`, `mcp_server_inference`, `compaction_inference`는 통과했다.

최종 설치본의 별도 API 추론 검사에서도 원문 `LOCAL_ad9466c9dc39`가 Tool 기록에는 정확히 남았지만, 모델 답변은 `LOCAL_ad94669d939`로 일부 문자를 바꿔 전체 수락 검사가 실패했다(`compactions=0`). 로그는 `build/compaction-final-installed-api.log`다. 설치 소비자 8/8 통과와 이 API 모델 정확성 실패를 구분한다. 실제 모델이 없는 전송 전용 검사 11/11도 통과했다(`build/compaction-final-installed-transport-result.json`). 인증·수동 압축 진입·빈 세션 보존·지침 조회·재시작·분기를 검증했다.

TDD에서 네이티브 측정 1건, 체크포인트 보존·검증·v1 이행 3건, 실행 연결 3건, 수동 API 메서드 미구현 실패를 먼저 확인했다. 최종 압축 단위 검사는 자동 micro/summary, 원문 조회, 여러 요약 묶음, 최신 입력 보존, 취소·차단·잘못된 출력·진전 없는 요약의 롤백, 구형 모델 어댑터, 큰 입력, 읽기 revision, 손상 기록·분기를 검증한다. 마지막 검토에서는 이미 요약한 prefix만 다시 요약하는 불필요한 추론과 keep_alive=0에서 프롬프트 임시 상태보다 모델이 먼저 해제되는 순서를 각각 재현했다. 경계 전진 검사와 소멸 범위 조정 후 두 회귀 검사도 통과했다. 실제 잘못된 메모리 접근을 유발하기 전에 probe로 수명을 검사했으며, 최종 ASAN/UBSAN에서는 오류가 관측되지 않았다.

재현 소스는 `tests/compaction_tests.cpp`, `tests/compaction_runtime_smoke.cpp`, `tests/service_tests.cpp`, `tests/agent_api_tests.cpp`, `tests/agent_api_smoke.py`, `tests/mcp_server_tests.cpp`, `tests/consumer/compaction.cpp`다. 증거는 `build/compaction-final-release-tests.log`, `build/compaction-final-LastTest.log`, `build/compaction-final-sanitizer-tests.log`, `build/compaction-final-consumer-tests.log`, `build/compaction-native-result.log`, `build/compaction-api-source-result.json`, `build/compaction-final-installed-loader.log`, `build/compaction-cli-linkage.log`, `build/compaction-verification.json`에 보관한다. 설치는 Workspace 안의 별도 stage이며 전역 설치나 제품 앱 재배포를 뜻하지 않는다.

## 2026-09-14 프로젝트 지침·경로별 컨텍스트 (0.5.0)

Apple M1 Max / Qt 6.8.3에서 C++ 프로젝트 지침 로더를 실제 Engine 입력에 연결했다. CLAUDE.md·AGENTS.md, Markdown import, YAML 경로 규칙, 중복·순환·루트 경계·상한, 매 모델 호출 전 갱신, 인증된 HTTP/native IPC 조회, MCP 실행 경로를 검증했다. MD4C 0.5.3·LibYAML 0.2.5를 private C object로 포함하며 원본 해시와 라이선스를 고정했다. 전체 하네스 완료나 제품 앱 연동 완료를 뜻하지 않는다.

| 검증 | 최종 관측 결과 |
|---|---|
| Release 빌드 | 성공. 최종 변경 빌드 로그에 경고·오류 없음 |
| 전체 CTest | **29/31 통과**, 96.02초. 실패는 `agent_local_inference`, `mcp_server_inference`의 소형 모델 원문 답변 검사 |
| 새 컨텍스트 검사 | Markdown 코드·주석 구분, import 순서·중복·symlink·깊이, glob·YAML 조건, UTF-8 BOM·CRLF·한국어 파일명, 크기·개수·스캔 상한, 갱신·삭제·resume·fork·편집 선행 읽기 유지 통과 |
| 실제 API·Qwen | 지침 파일에만 적힌 코드 답변, 경로별 조회·해시·앱 격리, Read 관측값, HTTP SSE 이벤트, daemon 재시작·CLI 분기 이어가기 통과. 42 생성 토큰·이벤트 11개는 파일 읽기 실행 기준 |
| ASan + UBSan | **8/8 통과**, 11.94초. C++뿐 아니라 새 C 파서도 sanitizer 플래그로 빌드. agent/context/service/http/API/transport/MCP 서버/daemon 검사 |
| 새 설치 패키지 | `build/context-stage`와 독립 `build/context-consumer/build`에서 **6/6 통과**, 3.99초. 공개 ProjectContext·Engine API 링크·실행 포함 |
| 설치본 실제 추론 | 별도 지침 코드 `CTX_bcbff7b5c2` 답변, Read 관측값·HTTP/native/CLI·재시작·분기 통과. 파일 읽기 44 생성 토큰·이벤트 11개 |
| ABI·배포 | 공개 EngineOptions·RunRequest 변경으로 SOVERSION 0.5. CLI·MCP 구현 버전도 0.5.0. 얇은 iillm은 Core/Network 전용 링크 유지 |

로더 미구현 상태에서 4개 동작 검사 실패, Engine 연결 전 실제 입력 검사 실패를 먼저 확인했다. UTF-8 BOM이 있는 rules frontmatter가 조건 없이 적용되는 문제도 별도의 실패 재현 뒤 수정했다. BOM은 파싱에서 제거하되 원본 SHA-256에는 포함한다. C 파서 추가 후 macOS `.m` 파일이 Objective-C++로 분류되던 빌드 오류는 Objective-C/Objective-C++ 언어를 명시해 수정했으며 llama.cpp 원본을 바꾸지 않았다.

**모델의 원문 답변 정확도 문제는 남아 있다.** Qwen2.5 0.5B Q4_K_M이 도구로 관측한 임의 문자열을 최종 응답에서 `This is a secret message.`로 바꾸거나 경로를 추측하는 사례를 관측했다. 지침 입력에서 감사용 해시를 제외하고 상대 경로·본문을 구분하여 프로젝트 지침 수락 시험은 통과했지만, 임의 파일 원문 답변의 일반적인 신뢰성까지 해결한 것은 아니다. 실패 값 `LOCAL_df5e751e8d69`를 이전 0.4.0 설치본에 넣어 같은 잘못된 답변을 재현했으며 로더 출력으로 해당 0.4.0 dylib를 확인했다. BOM 보완 전 전체 31/31 통과 실행과 개별 재검사 통과 기록도 있지만, 최신 소스의 마지막 전체 실행은 위 **29/31**로 기록한다. 성공한 실행만 선택해 품질 문제를 지우지 않았다. 검증 조건을 완화하거나 모델의 응답을 코드에서 관측값으로 덮어쓰지 않았다.

사용 계약·지원 차이는 [ProjectContext.md](ProjectContext.md)에 있다. managed/user 지침, 외부 import, 일반 첨부, 자동 요약·microcompact·캐시, 전체 하네스 및 실제 제품 연결은 남아 있다. 기본 사용자 SDK 위치와 이미 실행 중인 사용자 daemon은 설치 시험 대상으로 변경하지 않았다.

재현 코드는 `tests/context_tests.cpp`, `tests/agent_api_tests.cpp`, `tests/agent_api_smoke.py`, `tests/mcp_server_tests.cpp`, `tests/consumer/agent.cpp`이다. 로컬 증거는 `build/context-red-tests.log`, `build/context-engine-red-tests.log`, `build/context-bom-red-tests.log`, `build/context-release-ctest-verified.log`, `build/context-sanitizer-tests-verified.log`, `build/context-consumer-tests-verified.log`, `build/context-installed-result.json`, `build/context-compare-old-result.log`, `build/context-compare-old-loader.log`, `build/context-verification.json`에 보관한다.

## 2026-09-14 인증된 에이전트 HTTP·IPC API

Apple M1 Max / Qt 6.8.3에서 `agent::Api`, 전송 공통 `RpcHandler`, daemon 설정과 얇은 CLI의 인증된 RPC 호출을 구현·검증했다. 동일 앱의 HTTP/native IPC 세션·실행을 공유하는 범위이며, 전체 하네스 및 Society/Dreamscapes 제품 연결 완료를 뜻하지 않는다.

| 검증 | 관측 결과 |
|---|---|
| 최종 Release 빌드·전체 CTest | **30/30 통과**, 123.31초. GGUF/MLX CPU·Metal, 기존 CLI/HTTP/MCP 및 새 API 실제 추론 포함 |
| API·전송 회귀 | 잘못된 키, 앱 간 세션·요청 차단, 고정 workspace, state 소유 잠금, 큐·기한·세션 상한, 페이지·재시작·분기, 이벤트 순서, HTTP에서 IPC 실행 취소, 연결 해제·출력 초과·종료 검사 |
| 실제 daemon·CLI | 키 파일 소유·권한 검사, 로그/CLI 출력에서 키 미노출, HTTP/native/CLI의 동일 세션, 다른 앱 차단, daemon 재시작 후 원본·분기 기록 복원 통과 |
| 소스 빌드의 실제 모델 | HTTP SSE → Qwen2.5 0.5B Q4_K_M → Read → 관측값 답변. **41 생성 토큰·이벤트 9개**, 재시작 후 CLI에서 분기 세션 이어가기 통과 |
| ASan + UBSan | Debug·llama 비활성화 빌드에서 기존 agent/service/http 및 새 API/전송/daemon **6/6 통과**, 9.01초 |
| 새 설치 패키지 | `build/agent-api-stage`를 이용한 별도 공개 헤더·CMake 소비자 **6/6 통과**, 52.77초. 새 RpcHandler/Api·두 transport setter 링크·실행 포함 |
| 설치본 실제 모델 | 설치된 daemon·iillm으로 별도 임의 파일 값을 Read하고 답변, 분기·재시작·이어서 실행 통과. **40 생성 토큰·이벤트 9개** |
| 설치본 로더·CLI 경계 | DYLD/QT/QML 경로 override 제거. `build/agent-api-stage/lib/libiiLocalLLM.0.4.0.dylib` 로딩 확인. 설치된 iillm에는 SDK/llama/ggml 링크 없음 |
| 제어 카탈로그 | Unauthorized 오류 추가에 맞춰 Types.h provenance 해시만 재생성. **391그룹·9,183필드**와 설정 내용 유지, catalog 검사 통과 |

새 API 및 transport setter가 없는 상태에서 링크 실패, 이전 daemon에서 새 옵션 미인식을 먼저 확인했다. 취소 시험의 초기 충돌은 임시 QJsonObject에 대한 QJsonValueRef를 테스트가 보관한 원인이었고 값 복사로 고쳤다. Python 수락 시험의 HTTP 모듈 이름 가림도 수정했다. 새 코드의 최종 빌드는 경고 없이 통과했다. 최초 전체 재빌드에는 기존 `tests/service_tests.cpp`의 nodiscard 경고 4개가 있었으며 해당 테스트는 이번 변경 범위가 아니다.

설치본 수락 시험은 초기 두 차례 키 파일 거부를 기다리던 10초 제한에서 실패했다. 별도 하드웨어 실행은 **21.377초** 뒤 정상 완료했고, 그동안의 프로세스 샘플에서 `probeLlamaHardware → ggml_metal_library_init → newLibraryWithSource` 대기를 확인했다. 키 파일의 경로·권한·JSON 검사를 Service 초기화 앞에 배치했다. 수락 시험의 정상 시작 대기는 RPC 기한과 분리해 60초로 두고 시작 시간을 기록한다. 수정된 설치본은 잘못된 키 파일을 **0.421초**에 GPU 초기화 없이 거부했으며, 정상 첫 시작 **21.885초**, 재시작 **0.091초**를 기록했다. 실제 Metal 콜드 스타트 지연 자체를 제거한 변경은 아니다.

소스·설치본 모두 관측 문자열을 정확히 포함했지만 모델은 소개 문장·마침표 또는 코드 블록을 덧붙였다. 이 시험은 도구 관측값 전달과 대화 복원을 증명하며 정확한 최종 출력 형식이나 일반 답변 품질을 보장하지 않는다. 분기는 현재 artifact가 없는 transcript에 한정한다. 영속 작업 핸들·응답 재전송·artifact 복제·파일 rewind·분기 계보·실제 제품 연동은 미완료로 유지한다. 기본 사용자 SDK 위치나 이미 실행 중인 사용자 daemon을 이번 설치 시험 대상으로 바꾸지 않았다.

재현 경로는 `tests/agent_api_tests.cpp`, `tests/agent_transport_tests.cpp`, `tests/agent_api_smoke.py`, `tests/consumer/api.cpp`이다. 로컬 증거는 `build/agent-api-release-ctest-final.log`, `build/agent-api-sanitizer-tests-final.log`, `build/agent-api-consumer-ctest.log`, `build/agent-api-installed-result.json`, `build/agent-api-installed-loader.log`, `build/agent-api-hardware-startup.sample.txt`, `build/agent-api-verification.json`에 보관한다. 최초 실패 로그도 보존한다. 사용 계약은 [AgentAPI.md](AgentAPI.md)에 있다.

## 2026-09-14 C++ MCP 서버·앱 브리지·로컬 에이전트 제공

Apple M1 Max / Qt 6.8.3에서 0.4.0의 `mcp::ServerSession`, POSIX `serveStdio`, `agent::mcpServerOptions`와 `iillm-mcp`를 검증했다. 외부 MCP 클라이언트가 앱 도구와 연결별 로컬 에이전트를 실행하는 범위다. 전체 하네스 및 실제 Society/Dreamscapes 연결 완료를 뜻하지 않는다.

| 검증 | 관측 결과 |
|---|---|
| 최종 Release 빌드·전체 CTest | **26/26 통과**, 113.72초. GGUF Metal/CPU, MLX Metal/CPU, CLI·HTTP와 MCP 실제 추론 포함. 최종 빌드 경고 없음 |
| C++ 프로토콜 시험 | 클라이언트 QTest 20 passed, 서버 QTest 15 passed(각 초기화·정리 포함). 연결 격리, 초기화·기능 협상, 진행·취소·시간 제한, 역방향 요청, 목록 스냅샷, 리소스 구독, 구형 배열 및 용량 오류 검사 |
| 앱 도구 브리지 | 실제 스키마·권한·원본 콘텐츠, 연결 간 파일 읽기 이력 격리, 병렬 읽기/배타 쓰기, 로컬 대화 격리·새 대화 생성 검증 |
| 실제 stdio 실행 파일 | 독립 바이트 단위 peer로 UTF-8 분할, 잘못된 입력 복구, 버전별 배열 처리, 배열 내 취소, EOF·큰 입력·끊어진 stdout 종료 검사 |
| 공식 SDK 교차 검증 | Python MCP SDK **1.26.0**과 양방향 검증. 실제 파일 읽기·쓰기, 기본 거부와 명시 허용, 스키마·경로 제한, Bash와 자식 프로세스 취소 및 이후 연결 사용 통과 |
| 실제 모델을 제공하는 MCP 서버 | 공식 클라이언트 → C++ 서버 → Qwen2.5 0.5B Q4_K_M → Read → 최종 답변. **2턴·41 생성 토큰·진행 알림 9개**, 임의 파일 값 및 도구 호출/결과 ID·영속 transcript 확인 |
| ASan + UBSan | 최종 Debug·llama 비활성화 빌드에서 에이전트·MCP 클라이언트·서버·stdio **4/4 통과**, 12.71초 |
| 새 설치 패키지 | `build/mcp-server-stage`의 공개 패키지만 사용해 별도 소비자를 구성·빌드. **5/5 통과**, 28.61초 |
| 설치본 실행·로딩 | DYLD/QT/QML 경로 override를 제거했다. 로더에서 `build/mcp-server-stage/lib/libiiLocalLLM.0.4.0.dylib` 확인. 설치된 `iillm-mcp`도 공식 클라이언트의 파일/취소 시험 및 별도 임의 값으로 실제 모델 2턴 실행 통과 |
| CLI 링크 경계 | 설치된 iillm은 Qt Core/Network 및 시스템 라이브러리만 링크. 별도 iillm-mcp 실행 파일은 의도대로 SDK를 링크 |

최초 서버·브리지 테스트는 아직 없는 공개 심볼 때문에 링크 실패했다. 구현 후 stdio 시험은 macOS에서 유휴 출력 파이프의 종료를 감지하지 못하는 문제를 드러냈다. 실제 `poll` 실험에서 events=0은 종료를 보고하지 않고 POLLOUT은 POLLHUP을 반환하는 것을 확인했다. 별도 즉시 상태 확인으로 고치고 쓰기 시 SIGPIPE 차단 범위를 제한했다. 구형 배열의 큰 응답이 오류로 바뀔 때 배열에서 이탈하던 결함도 실패 테스트를 먼저 추가한 뒤 수정했다.

검증 중 소형 Qwen이 파일을 읽고도 관측값 대신 예문을 답한 실패를 보존했다. 실패한 C++ 에이전트 대화에서 실제 런타임의 토큰을 복원하자 도구 결과가 그대로 포함되어 있었다. 같은 입력은 **CPU/Metal 각각 새 컨텍스트와 KV 재사용 모두** 같은 잘못된 답을 생성했다. ServiceModel의 지침을 원문 관측값의 문자 단위 복사로 명확히 하고, 실제 실패 문자열과 새 무작위 문자열을 별도 세션에서 읽는 회귀 시험을 추가했다. 수정 전 고정 사례가 실패하고 수정 후 두 사례 및 최종 전체 시험이 통과했다. HTTP 시험에도 같은 지침과 실패 시 입력·응답 보존을 적용했다. 관측값은 초기 프롬프트·도구 스키마에 넣지 않았고, 최종 답변에서 실제 값을 확인하는 조건도 유지했다. 이 결과는 모든 모델의 응답 정확도나 부가 문구 없는 출력 형식을 보증하지 않는다.

첫 전체 실행은 23/26, 두 번째는 25/26이었다. 첫 실행에서 공식 Python 서버 연결의 시간 초과도 한 번 관측했다. 같은 제한으로 개별 및 최종 재검증은 통과했고 오류에 메서드 이름을 추가했지만, 최초 지연 원인은 확정하지 못했다. 제한을 늘리거나 원래 실패 기록을 덮어쓰지 않았다.

증거는 `build/mcp-server-final-build.log`, `build/mcp-server-final-tests.log`, `build/mcp-server-final-LastTest.log`, `build/mcp-server-sanitizer-tests.log`, `build/mcp-server-consumer-tests.log`, `build/mcp-server-installed-load.log`, `build/mcp-server-installed-official-result.json`, `build/mcp-server-installed-native-result.json`과 `build/mcp-server-verification.json`이다. 실패·수정 대조는 `build/mcp-server-full-first-failure*.log`, `build/mcp-server-full-second-failure*.log`, `build/mcp-server-batch-regression-red.log`, `build/model-observation-regression-{red,green}.log`, `build/model-observation-{cpu,metal}.json`, `build/model-observation-exact-metal.json`에 보존했다.

설치는 Workspace 내부 검증용 prefix다. 기본 SDK 경로 갱신·제품 앱 재배포·Windows/Linux 실행은 이 검증에 포함하지 않는다. Streamable HTTP·legacy SSE·OAuth, 전체 sampling/elicitation·tasks, 2026-07-28 규격, 실제 iisacc 앱 발견·인증·연동은 [HarnessParity.md](HarnessParity.md)의 미완료 항목으로 유지한다. 재현 명령과 공개 계약은 [MCPServer.md](MCPServer.md)에 기록한다.

## 2026-09-14 C++ MCP stdio 클라이언트와 외부 도구 실행

아래는 서버 구현 전 클라이언트 단계의 기록이다. 0.4.0의 `mcp::StdioClient`와 `agent::mcpTools`를 Apple M1 Max / Qt 6.8.3에서 검증했다. 외부 서버 프로세스를 실행해 도구·자료·프롬프트를 사용하고 기존 C++ 에이전트에 도구를 연결하는 범위다. 당시 MCP 서버 제공, HTTP 전송·인증, 자동 앱 발견과 실제 iisacc 제품 연동은 미완료였다. 이후 서버 검증은 위 절을 참조한다.

| 검증 | 관측 결과 |
|---|---|
| 당시 전체 Release 빌드·CTest | **22/22 통과**, 81.51초. 기존 GGUF Metal/CPU·MLX Metal/CPU 추론 및 새 MCP 시험 포함 |
| 독립 stdio peer | 17개 동작 사례 통과(QTest 초기화·정리 포함 19 passed). UTF-8 분할, 응답 순서 역전, 협상, 역방향 요청·재진입·취소, 진행 콜백 실패, 잘못된 프레임, 목록 중복·페이지 전체 용량 제한, 큐·종료 및 세션 보존 검사 |
| 공식 SDK 교차 검증 | 별도 Python MCP SDK **1.26.0** 서버와 초기화, 도구·리소스·프롬프트 조회, 구조화 결과, 진행 알림 2개, 역방향 roots 요청 통과. 공식 SDK는 시험용 환경에만 설치 |
| 실제 로컬 모델 → MCP 도구 → 답변 | Qwen2.5 0.5B Q4_K_M이 `mcp__fixture__read_secret`를 호출하고 프롬프트에 없는 임의 파일 값을 최종 답변으로 반환. 실제 토큰 생성, 최소 2턴, 진행 알림 및 도구 호출/결과 정합성 확인 |
| AddressSanitizer + UndefinedBehaviorSanitizer | Debug·llama 비활성화 빌드에서 에이전트와 MCP **2/2 통과**, 7.36초 |
| 새 설치 패키지 소비자 | `build/mcp-stage`로 설치한 공개 패키지에서 독립 소비자를 다시 빌드해 **4/4 통과**, 25.40초. 기존 API·서비스·에이전트와 공식 MCP 서버 교차 검증 포함 |
| 설치본 실제 로딩 | DYLD/QT/QML 경로 환경변수를 제거하고 실행. 동적 로더 기록에서 `build/mcp-stage/lib/libiiLocalLLM.0.4.0.dylib` 확인 |
| CLI 링크 경계 | 설치된 iillm은 Qt Core/Network·시스템 라이브러리에만 연결. iiLocalLLM·llama·ggml 링크 없음 |

새 테스트의 최초 링크 실패를 확인한 뒤 구현했다. 독립 peer는 동시 요청에서 클라이언트가 누락된 `_meta`를 `null`로 삽입하는 오류를 드러냈다. 클라이언트의 JSON 조회 방식을 수정했으며 peer의 입력 허용 범위를 넓히지 않았다. 이후 위 회귀 시험 전체가 통과했다.

공개 `Message`·`ToolResult`에 원본 content·metadata를 추가해 ABI를 **0.4**로 구분했다. 기존 소비자는 새 헤더·라이브러리로 다시 빌드해야 한다. 기존 JSONL 메시지는 계속 읽는다. 네이티브 모델이 지원하지 않는 이미지·음성·blob은 명시적으로 실패하며, 세션 레코드의 4 MiB 한도를 넘는 콘텐츠를 별도 artifact로 옮기는 기능은 미구현이다. 상세 계약은 [MCP.md](MCP.md)에 기록한다.

재현 명령은 `cmake --build build --parallel`과 `ctest --test-dir build --output-on-failure`다. 공식 SDK·실제 모델 시험은 MCP.md의 선택 설정이 필요하다. 증거는 `build/mcp-final-build.log`, `build/mcp-final-tests.log`, `build/mcp-final-LastTest.log`, `build/mcp-sanitizer-tests.log`, `build/mcp-consumer-tests.log`, `build/mcp-installed-load.log`, `build/mcp-final-inference-events.json`, `build/mcp-verification.json`에 보존했다. 설치 검증은 Workspace 내 별도 경로이며 기본 SDK 설치·실제 앱 재배포·커밋·푸시 결과를 포함하지 않는다.

## 2026-09-14 C++ 에이전트 실행 및 네이티브 도구 호출

Apple M1 Max / Qt 6.8.3 / Release 빌드에서 `agent::Engine`, 도구 스키마·권한·훅, JSONL 세션 복구, llama.cpp 구조화 대화와 외부 HTTP 함수 호출을 검증했다. 전체 하네스 완료와 iisacc 제품 실제 연동을 뜻하지 않는다. 미완료 영역은 [HarnessParity.md](HarnessParity.md)에 유지한다.

| 검증 | 관측 결과 |
|---|---|
| 전체 Release CTest | **19/19 통과**, 69.79초. GGUF Metal/CPU 및 MLX Metal/CPU의 기존 추론도 포함 |
| 에이전트 코어 | 13개 동작 테스트 통과. 도구 루프, 정책/스키마, 훅 입력 재검증, 취소, 병렬/배타 순서, 잠금/중단 복원, registry 교체 중 스냅샷 일관성 포함 |
| 실제 C++ 모델/도구 루프 | Qwen2.5 0.5B Q4_K_M이 Read를 스스로 선택하고 프롬프트에 없는 무작위 파일 값을 최종 답변에 반환. 서로 다른 값으로 **3회 연속 통과**, 이후 전체 suite에서도 통과 |
| 실제 daemon HTTP 도구 왕복 | SSE로 Read 호출 수신 → 외부 클라이언트가 fixture 파일을 실제 읽음 → 동일 ID의 tool 결과 입력 → JSON 최종 답변에서 파일 값 확인. CLI/HTTP 전체 과정 model_loads=1, 종료 후 sessions=0 |
| 구조화 API 오류/캐시 | 잘못된 역할·호출 ID, required 도구 누락, 호출 재사용, 잘린 출력 거부. 모델별 contextId 격리 및 재사용, 소비자 실패 시 실행 가능한 호출 제거 |
| HTTP 프로토콜 | tool_choice/parallel_tool_calls 전달, content:null, tool_calls 종료 이유, SSE 호출 index/ID/인자, usage, [DONE] 검증 |
| 외부 설치 소비자 | `build/agent-stage`의 공개 패키지로 새 consumer를 빌드해 **3/3 통과**. DYLD_LIBRARY_PATH를 제거한 상태에서 앱 도구/모델/세션 ABI 실행 |
| CLI 링크 경계 | 설치된 iillm의 의존성은 Qt Core/Network와 시스템 라이브러리. iiLocalLLM/llama/ggml 링크 없음 |
| 파라미터 출처 | Types.h 변경 후 고정된 원본에서 카탈로그 재생성. native 소스 해시와 줄 번호를 제외한 전체 카탈로그의 의미 내용은 이전 설치본과 동일 |

처음 JSON-envelope 방식에서는 소형 모델이 도구를 생략했고, 네이티브 템플릿 연결 후에는 읽은 값을 예문으로 치환한 실패도 관측했다. 실패를 숨기거나 fixture 값을 프롬프트에 넣지 않았다. upstream Jinja/문법/PEG 경로를 연결하고 실제 관측값을 그대로 사용하도록 시스템 지침을 보완한 뒤 위 검증을 통과했다. 이 제한된 수락 테스트는 모든 모델·작업에서의 정확도 보증이 아니다.

검증 로그는 `build/agent-native-build.log`, `build/agent-full-ctest.log`, `build/agent-native-repeat.log`, `build/agent-consumer-ctest.log`이다. 스냅샷 증거는 `build/agent-verification.json`에 기록한다. 설치 경로는 Workspace 안의 stage이며 사용자 기본 SDK 경로 설치, 커밋·푸시, 실제 제품 UI 검증은 이 기록에 포함하지 않는다.

## 2026-09-13 최소 로컬 대화 완성

기존 서비스·런타임을 유지하면서 기본 llama.cpp 빌드, 공식 경량 대화 모델 registry, CLI의 `--temperature`·`/clear`·매 턴 JSON flush를 추가했다. 별도 UI 없이 C++ SDK, 터미널 대화, localhost HTTP JSON/SSE로 사용할 수 있는 범위이다.

Apple M1 Max / RAM 32 GiB / Qt 6.8.3에서 현재 소스를 Release로 빌드하고 다음을 검증했다.

| 검증 | 결과 |
| --- | --- |
| 새 CMake 구성 | `IILOCALLLM_WITH_LLAMA`를 지정하지 않은 `build/default/build`의 값이 ON. 기존 고정 llama.cpp 소스를 재사용한 구성 검사 |
| 변경 전 회귀 재현 | 새 CLI 테스트가 `Unknown option 'temperature'`로 실패하는 것을 확인 후 구현 |
| 변경 후 서비스·CLI | 2/2 통과. 잘못된 온도 입력 거부, greedy 반복성, `/clear`, 신호 취소·세션 정리 포함 |
| 전체 Release CTest | **14/14 통과**, 226.14초. GGUF Metal/CPU, MLX Metal/CPU, 실제 대화 수락 테스트 포함 |
| 공식 모델 pull | 실제 `iillm pull qwen2.5:0.5b`로 HTTPS 다운로드·491,400,032 bytes 및 SHA-256 검증·원자적 설치 완료 |
| 실제 CLI 대화 | 내장 chat template 사용, `2 + 2`에 `4` 응답. 한국어 자기소개 29 tokens 생성 후 stop 종료 |
| 대화 이력·캐시 | 이름을 Mira라고 전달한 다음 턴에 `Mira` 응답, 37 tokens 재사용. `/clear` 후 cached_tokens=0, system prompt 유지 |
| HTTP 및 공유 모델 | JSON/SSE 모두 `4`, 종료 마커·finish_reason 확인. CLI/HTTP 전 과정 model_loads=1, 완료 후 sessions=0 |
| 설치 소비자 | Workspace `build/minimum-stage`만 찾는 새 C++ 소비자 **2/2 통과**. 라이브러리 경로 환경변수 없이 공개 API·새 registry 별칭 사용 |
| 설치 실행 파일 | DYLD_LIBRARY_PATH·DYLD_FRAMEWORK_PATH·LIBRARY_PATH·CMAKE_PREFIX_PATH 없이 설치 daemon/CLI의 동일 실제 대화·이력·초기화·HTTP 검증 통과 |

공식 모델은 revision `9217f5db79a29953eb74d5343926648285ec7e67`, SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`를 사용했다. 모델 품질 전체를 보증하는 평가가 아니라 최소 대화 경로의 실제 동작 검증이다.

설치 실행 파일의 첫 검증은 기동 60초 제한에 걸렸다. 해당 로그에는 Metal 셰이더 초기화가 약 25.8초 걸린 것이 기록되어 있다. 하드웨어 검사 단독 실행은 정상 종료했고, 테스트의 기동 대기만 120초로 조정한 재검증에서 설치 파일의 모든 대화 검사를 통과했다. MLX는 기존 제한 안에서 152.14초로 통과했다. 서비스의 추론 timeout을 늘리거나 검증 항목을 제거하지 않았다.

실행용 모델 저장소는 `build/chat/Models`이다. macOS에서 `build/Models`와 테스트 fixture 경로 `build/models`가 같은 디렉터리가 될 수 있으므로 실행 문서의 경로를 분리했다. 모델 가중치와 로그는 build/ 아래에 두며 Git 및 SDK 설치 패키지에는 포함하지 않는다. 설치 검증 경로는 Workspace staging이며 사용자의 `.local` 설치나 원격 저장소 게시를 수행한 결과는 아니다.

증거 파일:

- `build/minimum-configure.log`, `build/minimum-build.log`, `build/default-configure.log`
- `build/minimum-tdd-red.log`, `build/minimum-tdd-green.log`, `build/minimum-full-tests.log`
- `build/starter-pull.json`, `build/starter-answer.json`, `build/starter-korean.json`, `build/starter-chat-verification.log`
- `build/minimum-install.log`, `build/minimum-consumer.log`, `build/installed-starter-chat-verification.log`
- `build/installed-starter-chat-first-attempt.log`, `build/installed-hardware.json`, `build/installed-hardware.log`

재검증은 README의 `IILOCALLLM_TEST_CHAT_GGUF` 설정과 `ctest --test-dir build --output-on-failure`를 사용한다. Windows/Linux 네이티브 실행, CUDA/Vulkan 실기기, GUI, 도구 호출·멀티모달·영구 대화 저장은 이번 최소 구현의 검증 범위에 포함하지 않는다.

## 2026-09-07 기존 서비스 검증

2026-09-07, Apple M1 Max / 32 GiB 통합 메모리의 macOS, AppleClang 21, C++20, Qt 6.8.3 환경에서 실행했다. 테스트는 실제 저장소 소스와 build/ 아래 산출물을 사용했다.

| 검증 | 결과 |
| --- | --- |
| Release CTest | 13/13 통과: 레거시 API, 서비스/IPC, 상주 정책, CLI, HTTP, 모델 카탈로그, 하드웨어 정책, MLX 정책, llama Metal/CPU 추론, 독립 daemon, MLX Metal/CPU 추론 |
| 서비스 테스트 상세 | 20개 동작 테스트 통과; Qt init/cleanup 포함 22 passed |
| 모델 카탈로그 상세 | 8개 동작 테스트 통과; Qt init/cleanup 포함 10 passed |
| HTTP 상세 | 7개 동작 테스트 통과; Qt init/cleanup 포함 9 passed |
| 하드웨어 정책 상세 | 데이터 행 포함 23개 검증 통과; Qt init/cleanup 포함 25 passed |
| MLX 캐시·장치·CPU 스트리밍 | 10개 Python 테스트 통과; prefix/trim, 장치 명시, Metal 불가, EOS/최대 토큰 종료 |
| AddressSanitizer + UndefinedBehaviorSanitizer | 기본 CTest 8/8 통과; C++/Objective-C++·HTTP 검사, llama 비활성화, leak detection 비활성화 |
| 설치 패키지 소비 | 2/2 통과: 기존 API 및 manifest/catalog/URI/pull/residency/Service/Hardware/Runtime/IPC/HTTP 공개 API, HTTP 리스너 시작·종료 |
| 독립 서비스 프로세스 | 패키지 설치 → URI로 GGUF 로드 → Native IPC와 HTTP 동시 추론 → 재시작 후 같은 URI 사용 → 언로드·제거 및 SIGTERM 정리, HTTP 단독 실행 통과 |
| 설치 후 실행 경로 | DYLD_LIBRARY_PATH, DYLD_FRAMEWORK_PATH, LIBRARY_PATH, CMAKE_PREFIX_PATH 없이 설치 daemon의 실제 추론과 소비자 2/2 통과 |

서비스 테스트는 다중 턴, 캐시 재사용, 실패·취소 rollback, FIFO, 큐 상한, LRU, 예약 토큰 상한, 모델 언로드 제한, 컨텍스트 초과, 청크 경계 stop, 종료 중 진행/대기 작업 취소, UTF-8 경계, IPC 프레임 분할, 기존 endpoint 보호, 포화 상태 cancel, 연결 종료, 출력 버퍼 초과를 검증한다. hardware.get의 실제 응답, 모델 요청에서 runtime/backend/device/gpu_layers 지정 거부, 런타임을 지정하지 않는 모델 로드도 확인했다.

## Native IPC와 HTTP

HTTP 단위 테스트는 실제 loopback TCP 연결과 결정적인 테스트 엔진을 사용했다. GET /health와 /v1/models, 과거 user/assistant 이력을 포함한 JSON 생성, SSE의 역할·Unicode delta·finish_reason·usage·[DONE], stop 문자열 처리를 검증했다. URI/등록 별칭 입력과 모델 없음, JSON·숫자·역할·미지원 필드 오류, Content-Type·body 상한, Host/Origin 검사와 점유된 포트의 실패도 확인했다.

오류가 난 생성의 일반 JSON 500과 SSE error 종료, 클라이언트 disconnect 취소, 요청 deadline, 누적 출력 상한, 리스너 close 중 생성 취소를 검사했다. HTTP의 임시 세션과 캐시가 성공·실패·취소 후 모두 제거되었다. Native Service::chat이 진행 중인 상태에서 같은 FIFO를 포화시켜 HTTP가 queue_full/429를 받는 것과 대기 요청의 timeout/504를 확인했다.

독립 daemon을 --socket과 --http-port 0으로 동시에 시작하여 같은 model://test와 llama.cpp/Metal로 Native IPC 추론, HTTP JSON 추론, 전체 이력을 다시 전달한 HTTP SSE 추론을 수행했다. HTTP 사용 후 기존 IPC 세션 이력은 같았고 세션 개수도 그 세션 하나만 남았다. IPC unload 후 HTTP /v1/models도 빈 배열이 되었다. 서비스 재시작 후 같은 검증을 반복했으며 HTTP 전용 부팅도 확인했다. 종료 후 Unix socket·저장소 lock·TCP 리스너가 모두 해제되었다.

## 모델 관리

ModelCatalog는 엔진 없이 최소 manifest로 GGUF/MLX 구조의 패키지를 설치하고 전체 파일 inventory를 생성한다. 디렉터리 이름과 독립적인 URI, 별도 인스턴스의 재조회, 저장소 소유권 충돌, 중복 id, 불완전 manifest 진단, 다른 정상 모델의 조회를 검증했다. 파일 변조·누락·추가, 원본 해시 불일치, 경로 이탈·symlink 거부, 취소/실패 시 게시되지 않는 설치, 원본과 외부 symlink 대상이 보존되는 제거도 확인했다.

서비스 테스트는 ModelManager의 7개 연산, 설치와 엔진 로드의 분리, URI 전용 요청, 기본 컨텍스트 제한, manifest 상한, chat capability, 로드 중 제거 및 세션이 있는 모델의 언로드 거부를 검사했다. 설치 파일을 변조한 뒤 load를 호출했을 때 integrity_failure가 발생하고 테스트 엔진의 load 횟수가 증가하지 않았다.

실제 GGUF·MLX smoke는 기존 로컬 가중치를 임시 패키지로 복사하여 install → verify → URI load → 추론 → unload → remove를 수행했다. 독립 daemon 테스트는 CLI 설치 후 원본 패키지를 다른 경로로 옮기고 두 번 부팅하여 model://test로 실제 추론했다. 마지막에는 IPC install/load/unload/remove로 같은 패키지를 다시 관리했다. 두 종료에서 소켓과 카탈로그 소유권 잠금이 모두 해제되었다. 로컬 패키지 테스트는 배포자 서명 검증을 포함하지 않는다. 다운로드·상주 기능의 검증은 아래와 같다.

## CLI, pull과 모델 상주

`tests/cli_smoke.py`는 실행 파일 iiLocalLLMD와 iillm을 별도 프로세스로 띄운다. otool로 iillm이 Qt Core/Network만 사용하고 libiiLocalLLM/llama/ggml을 링크하지 않는 것을 확인했다. daemon이 없을 때 run은 연결 오류로 종료했다. models/pull/ps, 위치 독립적인 alias→URI 해석, daemon 재시작 후 설치 목록 복원, 기존 정상 모델 pull 시 추가 HTTP 전송이 없는 것을 검증했다.

loopback HTTP 원본에서 실제 약 19MB GGUF를 다운로드·설치했다. 잘못된 SHA-256, 잘못된 파일 크기(부족/초과), 허용하지 않은 리다이렉트를 실패 처리하고 staging/설치 잔여 파일이 없는지 확인했다. 느린 다운로드 중 SIGINT로 CLI를 종료하자 서비스가 pull을 취소하고 임시 파일을 정리했다. PTY의 대기 중 run도 SIGINT로 130 종료하고 세션을 정리했다. sanitizer의 같은 테스트는 엔진 없는 소형 파일을 사용했다.

실제 TinyStories GGUF에는 템플릿이 없으므로 서비스 호스트의 models.load options.chat_template=chatml을 명시한 뒤 unload했다. 이후 CLI run이 기억한 서비스 설정으로 자동 재로드하여 실제 텍스트를 생성했다. CLI → HTTP → stdin을 받는 CLI 순서의 생성에서 stats.model_loads가 증가하지 않았고, 정상 run 후 세션이 남지 않았다. ps의 메모리 추정치는 weights/context/overhead를 포함했다. keep_alive=0 생성 뒤 ps가 비고 cached_contexts=0임을 확인했다.

정책 테스트는 2개 모델이 공존하는 예산에서 세 번째 모델을 요청하고, 최근 사용이 가장 오래된 모델의 KV/가중치만 제거한 뒤 이전 세션 이력으로 다시 생성하는 것을 확인했다. 단일 모델이 예산보다 크면 runtime load 횟수가 0인 채 resource_limit이다. 추가 API 호출 없이 TTL이 만료되어 실제 모델 파괴가 실행되었고, keep_alive=0인 활성 생성은 보호되다가 취소 후 해제되었다. 활성 생성 중 models() future가 즉시 준비되고 active_requests=1, expires_in_ms=-1을 반환했다. HTTP의 자동 로드/재사용, keep_alive=0, 잘못된 duration도 별도 테스트했다.

실제 8 GiB 시스템의 동작은 측정하지 않았다. 8 GiB/32 GiB 하드웨어 입력에 따른 0ms/5분 기본 정책, 명시 설정 우선순위, LRU/활성 보호, 메모리 산술과 duration 범위를 결정적인 테스트로 검사했다. 실제 모델별 RSS/VRAM 피크의 정확도나 모든 특수 KV 아키텍처를 보증하는 검증은 아니다.

설치된 daemon/CLI로 고정 llama.cpp revision의 공개 LICENSE 1,078 bytes를 실제 HTTPS로 받아 SHA-256과 원자적 설치 결과를 비교했다. 기본 테스트는 계속 외부 인터넷에 의존하지 않는다.

기본 Qwen registry의 고정 revision/크기/LFS SHA-256은 공식 API에서 확인했다. 약 5GB Qwen 가중치를 실제로 다운로드하거나 추론한 검증은 수행하지 않았다. loopback TinyStories 전송 및 GGUF/MLX 실제 추론과 이 검증 범위를 구분한다.

## 하드웨어 자동 선택

설치된 daemon의 --hardware에서 cpu_architecture=arm64, apple_silicon=true, ram_bytes=34,359,738,368을 관측했다. GPU vendor는 apple, 이름은 Apple M1 Max, unified_memory=true이다. 전용 VRAM은 null로 보고하며 Metal 권장 작업 메모리 26,800,603,136 bytes와 구분한다. metal_available=true, cuda_available=false, vulkan_available=false이다. 시스템 Metal과 ggml의 어댑터 항목은 같은 물리 GPU를 가리킨다.

GGUF는 llama.cpp/metal, MLX 패키지는 mlx/metal을 자동 선택했다. 실제 IPC 모델 설정·요청은 URI를 사용하고 파일 경로·런타임·장치 필드가 없다. GPU 초기화 실패 시 CPU 재시도, vendor별 CUDA/Vulkan 분기, 잘못된 입력·취소의 비재시도는 주입한 하드웨어·실패 조건으로 검증했다. 어댑터의 실제 CPU 실행은 별도 로컬 모델 테스트로 검증했다.

## 실제 런타임 검증

llama.cpp 소스는 커밋 `5202104b59ada9005db079eea43882a2b7bf5802`를 고정했다. 테스트 모델은 ggml-org/models-moved의 `tinyllamas/stories15M-q4_0.gguf`이다. 첫 prompt 35 tokens, 생성 24 tokens, 다음 턴 캐시 재사용 58 tokens를 관측했다.

MLX는 Python 3.12.14, mlx 0.32.2, mlx-lm 0.31.3을 사용했다. 테스트 모델은 `mlx-community/SmolLM-135M-Instruct-4bit`, revision `642e06afe3fab57fd6cc518637c471af0a569e1e`이다. 첫 prompt 17 tokens, 생성 24 tokens, 다음 턴 캐시 재사용 41 tokens를 관측했다.

두 런타임에서 Metal 자동 선택 → 실제 생성 → 다음 턴 KV 재사용 → 세션 초기화 → 생성 도중 취소 → 이전 이력 유지/캐시 삭제 → 새 생성까지 통과했다. 모델 출력과 delta 결합 결과가 같은지도 확인했다. CPU에서도 두 턴 실제 생성과 KV 재사용을 확인했으며 llama.cpp 40 tokens, MLX 22 tokens의 재사용을 관측했다. llama.cpp CPU 실행 로그는 GPU offload 0/7 layers이다. 경량 fixture이므로 답변 품질이나 모든 모델 아키텍처의 지원을 입증하지는 않는다.

## 산출물과 범위

- `build/iiLocalLLMD`, `build/iilocal-llm-service`: llama.cpp 활성화 Release daemon.
- `build/iillm`: 추론 엔진을 링크하지 않는 Native IPC 클라이언트.
- `build/stage/`: Workspace 내부 설치 검증 패키지. 시스템 SDK 설치와는 별개의 staging이다.
- `build/consumer/build/`: 설치 패키지만 링크하는 소비자.
- `build/sanitizer/build/`: 기본 기능의 sanitizer 빌드.
- `build/Testing/Temporary/LastTest.log`: Release 테스트 상세와 실제 추론 출력.
- `build/install-verification.log`: 구성·빌드·테스트·설치·소비자 검증 로그.
- `build/hardware.json`: 설치 daemon의 실제 하드웨어 스냅샷.
- `build/cli-full-tests.log`: CLI/상주 기능의 전체 Release CTest 13/13 결과.
- `build/cli-final-tests.log`: HTTP keep_alive 및 CLI의 PTY 중단 검증 추가 후 관련 테스트 2/2 결과.
- `build/cli-sanitizer.log`: CLI/상주를 포함한 ASan/UBSan 기본 CTest 8/8 결과.
- `build/cli-sanitizer-final.log`: 최종 HTTP/CLI 관련 재검증 2/2 결과.
- `build/cli-install-verification.log`: 설치 패키지와 확장된 소비자 2/2 결과.
- `build/installed-daemon-verification.log`: 이전 IPC/HTTP 설치 daemon 검증.
- `build/installed-cli-verification.log`: 라이브러리 경로 환경변수 없이 설치 iiLocalLLMD/iillm의 다운로드·취소·재시작·동일 인스턴스 추론 검증.
- `build/https-pull-verification.log`: 설치 실행 파일의 실제 HTTPS 다운로드 및 SHA-256 검증.
- `build/models/`: 로컬 추론 fixture; 배포 패키지에는 포함하지 않는다.

Windows/Linux 네이티브 빌드와 Windows Named Pipe 실행, 실제 NVIDIA/CUDA 및 AMD·Intel/Vulkan 드라이버 실행, ONNX, 다른 모델의 template/가중치, 장시간 서비스 부하 및 모델 품질은 이번 실행에서 검증하지 않았다. HTTP는 문서화한 텍스트 Chat Completions 범위이며 전체 OpenAI API/SDK 호환성 인증은 아니다. CUDA/Vulkan 선택 분기는 정책 단위 테스트 범위이다. ONNX는 사용자 정의 Runtime 구현을 등록할 수 있는 확장 지점만 제공한다. 코드는 로컬 작업 트리에 반영했고 공개 배포나 원격 저장소 변경은 검증 범위에 포함하지 않았다.

## 2026-09-13 상세 파라미터 객체 (0.3.0)

- 공식 프로젝트 13개, 잠금 파일 214개. 391개 그룹, 상속 포함 9,183개 필드, 고유 선언 4,610개. `fetch_parameter_sources.py`로 모든 캐시 해시 확인 후 재생성 결과가 카탈로그·coverage와 바이트 단위로 일치했다.
- TDD 실패 증거: `build/parameters-tdd-red.log`(공개 헤더 부재), `build/parameter-bindings-red.log`(아직 없는 생성 변환 심볼). 이후 C++ 타입·범위·튜플·상속·null/unset/default·교차 조건·중첩 민감 필드·미지원 바인딩 테스트 통과.
- 최종 Release 전체 CTest: **17/17 통과, 99.48초**. 로그 `build/parameters-full-tests.log`. 마지막 추가 중첩 redaction 검사는 `build/parameters-final-unit-test.log`에서 통과했다. 최종 빌드 경고 없음.
- HTTP 확장 옵션이 런타임에 도착하는지 검증했다. 취소된 HTTP 작업의 큐 슬롯이 반환되기 전 정리 요청을 넣던 기존 테스트는 실제 admission을 기다리도록 보완했다.
- GGUF/MLX 실제 모델에서 토큰 42/43에 서로 다른 강한 logit_bias를 적용해 출력이 바뀌는 것을 검사했다. min-p·최소 후보 수·XTC·패널티·llama typical sampling도 실제 생성에 적용했다. CPU 경로, KV 재사용, 취소·복구, CLI·HTTP JSON/SSE 회귀를 함께 통과했다.
- mlx-lm 0.31.3의 min-p에서 MLX 0.32.2가 scalar bool을 거부하는 오류를 재현했다. 공식 수정 샘플러를 해시 고정하여 포함했고 수치 회귀 3개와 MLX 실제 생성이 통과했다. source cache의 공식 파일과 vendored sampler의 해시 일치도 자동 검사한다.
- 검증용 설치 prefix: `build/parameter-stage`. 별도 소비자: `build/parameter-consumer/build`. DYLD/QML/QT_PLUGIN_PATH override를 지운 CTest **2/2 통과, 23.75초**. `build/parameters-consumer-runtime.log`에서 실제 로딩한 파일이 `build/parameter-stage/lib/libiiLocalLLM.0.3.0.dylib`임을 확인했다.
- 설치된 sampler 경로에서도 min_keep=2 수치 검사를 통과했다. 설치된 CLI에서 391개 그룹 조회, TrainingArguments/LoRA JSON 검증, 새 옵션 파일을 적용한 Qwen 계산 응답 `4`를 확인했다. 증거 `build/parameters-installed-acceptance.json`.
- 설치된 0.3.0 데몬의 HTTP에 min_p·repetition/presence/frequency penalty를 보냈고 `안녕하세요! 무엇을 도와드릴까요?`를 HTTP 200으로 받았다. 증거 `build/parameters-installed-http.json`.

현재 이 작업이 시작한 데몬은 `build/parameter-stage/bin/iiLocalLLMD`, IPC `build/chat.sock`, HTTP `http://127.0.0.1:50890`이다. 기존 모델 저장소 `build/chat/Models`를 그대로 사용하고 모델은 요청 시 로드한다. 로그는 `build/chat-daemon-parameters.log`이다. 프로세스 생존은 이 검증 시점의 상태이며 영구 등록한 시스템 서비스는 아니다.

소스 구현, 빌드·테스트, 검증용 설치, 로컬 실행을 확인했다. 학습 프레임워크 실행·실제 학습 작업·CUDA/Vulkan/Windows/Linux 장치 검증·사용자 기본 설치 경로 갱신·커밋·푸시는 이 검증의 완료 범위에 포함하지 않는다. 학습 객체의 범위와 선언 검증 한계는 [Parameters.md](Parameters.md)에 명시했다.
