# 구현 검증 기록

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
