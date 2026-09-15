# 구현 검증 기록

## 2026-09-15 MCP 도구 결과 변경 훅 (0.32.0)

PostToolUse의 updatedMCPToolOutput을 C++ 실행기에 연결했다. 명령·HTTP·C++ 훅이 성공한 가져오기 MCP 도구의 관측을 문자열 또는 MCP 콘텐츠 배열로 교체한다. 원래 출력 스키마를 먼저 검증하고, 교체 뒤 원래 구조화 데이터가 모델·transcript·API·MCP 재전달에 남지 않도록 처리한다. 이름·공개 metadata로 일반 도구를 MCP 도구로 가장할 수 없으며, 원래 _meta와 이미 수행된 외부 효과는 보존한다. [McpOutputHooks.md](McpOutputHooks.md)에 참조 대조와 형식 제한을 기록한다.

| 검증 경계 | 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 68/68, 139.59초 |
| ASan·UBSan, llama 비활성 Debug | 65/65, 170.08초 |
| 새 설치 소비자, 공식 MCP 교차 검사 포함 | 38/38, 50.92초 |
| 원본 보존·실패 경계 | 일반 도구 metadata 위장, 원격 출력 스키마 오류, 오류 응답, 권한 거부에 교체 미적용 |
| 출력 형식·병합 | 문자열·한글·미디어/리소스 블록·빈 배열, false 값 무시, 잘못된 후속 응답 뒤 유효한 이전 결과 유지, 완료 순서와 피드백 보존 |
| 중단 | 잘못된 이벤트·명령 종료 코드 1/2의 교체 미적용, continue:false와 실제 실행 중 취소 전파, 이미 수행된 MCP 호출 유지 |
| 모델·API·MCP | 실제 HTTP MCP 생산자와 HTTP 훅, 교체된 Model 입력·영구 transcript·API 결과·ToolFinished, 재전달 outputSchema 계약 |
| 실제 로컬 추론 | 소스 2건·설치 2건, 공식 Python MCP 1.26.0 stdio 서버와 Qwen2.5 0.5B, 문자열·배열 관측 교체 |

실제 추론 검사는 tests/agent_runtime_smoke.cpp의 --mcp-output-hook 모드이다. 원래 서버 값과 교체 값은 다르며, 두 번째 교체 값은 실행 때 새로 만든다. 두 형식 모두 모델이 실제 MCP 도구를 호출하고 변경된 값을 최종 답변에 포함했다. 각 호출은 2회 이상의 모델 턴, 생성 토큰과 MCP 진행 알림을 남기며 Tool 메시지의 text/content와 빈 data를 검사한다. API 회귀의 Model은 결정적인 검사 대역이며 실제 모델 검사는 별도 C++ Engine 경로이다. 전체 모델의 도구 선택·판단 정확도를 보장하지 않는다.

모델은 491,400,032바이트, SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`이다. 라이브러리 SHA-256은 `0e80cc126ce11ba52bd918418437da00ab505770360a422f6a6cc65c8e0659b3`이다. 설치 경로는 build/mcp-output-hooks-stage, 소비자는 build/mcp-output-hooks-consumer/build이다. 실제 설치 소비자의 로더·공개 헤더·문서·카탈로그·CLI 버전과 얇은 클라이언트 링크, 소스/설치 라이브러리와 실행 파일의 CMake RPATH 변환 후 동일성은 mcp-output-hooks-linkage.json에 기록한다. 공개 Tool/HookResult 구조체가 변경되어 0.32 헤더와 라이브러리로 소비자를 다시 빌드해야 한다.

TDD의 기존 구현에서 결과 교체 실패 5건을 먼저 관찰했다(mcp-output-hooks-red.log). 첫 구현 뒤 명령 인자에 넣은 한글이 분해형으로 전달되는 별도 현상 때문에 3건이 실패했다(mcp-output-hooks-green.log). 명령 테스트는 ASCII JSON Unicode escape로 정확한 stdout 디코딩을 검사하도록 수정했고, HTTP/C++ 검사는 원래 한글 문자열을 직접 비교한다. QProcess 인자 인코딩을 수정한 것으로 보고하지 않는다. 최종 통과 기록은 mcp-output-hooks-verification.json, 소스/설치 실제 추론은 mcp-output-hooks-{source,installed}-native.json, 검사한 입력 해시는 mcp-output-hooks-tested-source.json이다.

참조의 임의 unknown 값·공급자 전용 콘텐츠 블록까지 동일하게 수락하는 구현은 아니다. 전체 생명주기·설정/스킬/플러그인 병합·전체 하네스 및 실제 앱/플랫폼 검증은 계속 partial이다. Society와 Dreamscapes를 이번 SDK 변경으로 다시 패키징하지 않았으며, iPhone은 사용자 요청대로 제외한다. 기존 사용자 데몬 PID 14909는 중단하거나 교체하지 않는다.

## 2026-09-15 C++ 에이전트 훅 (0.31.0)

실제 도구를 실행하는 별도 검증 대화를 C++ Engine·ToolRunner·API·MCP에 연결했다. dontAsk 권한, 예약 StructuredOutput, 50번째 assistant 메시지 실행 전 중단, 부모 transcript의 정확한 Read, 취소·임시 상태 및 background Bash 정리를 구현했다. Task 게시 전 콜백은 잠금 밖에서 실행하고 원래 보드와 비교해 충돌을 거부한다. MCP 검증기의 TaskStore·세션 권한은 외부 연결 ID가 아닌 실제 Engine 대화에 연결한다. 계약과 참조 차이는 [AgentHooks.md](AgentHooks.md)에 기록한다.

| 검증 경계 | 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 67/67, 149.66초 |
| ASan·UBSan, llama 비활성 Debug | 64/64, 153.52초 |
| 새 설치 소비자, 공식 MCP 교차 검사 포함 | 37/37, 40.91초 |
| 종료 경계 | 결과 도구 뒤의 호출 미실행·결과 짝 보존, 잘못된 결과 수정, 49번째 수락/50번째 실행 전 중단 |
| 권한·파일 | bypass/acceptEdits 암묵적 쓰기 차단, 명시적 허용/거부/Ask, 세션 허용 유지, 정확한 transcript Read와 인접 상태/링크/쓰기 거부 |
| 실행 통합 | MCP 소유 대화의 TaskList, 게시 전 보드 읽기와 동시 변경 충돌, 스킬·지연 도구 검색·Plan 프로필과 추가 호스트 도구 |
| 정리 | 부모 취소·자체 기한·입출력 한도 뒤 임시 대화 제거, 검증기가 시작한 background Bash 종료 |
| Qwen3 8B API | 소스·설치 각각 파일 허용, 파일 차단 후 대화 지속, Stop 차단 후 FIXED 응답, UserPromptSubmit 차단 |
| Qwen3 8B CLI·Task | 양쪽에서 IPC 입력 차단과 Task 생성 허용/게시 전 거부 |
| Qwen3 8B MCP | 양쪽에서 HTTP Write 3건(허용·훅 거부·호스트 거부), 공식 Python SDK stdio Write 2건 |

모델은 model://qwen3-8b-q4, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 파일을 다시 해시했다. 진단을 수집한 실제 훅은 소스 15개, 설치본 15개이다. 모든 수집된 훅에서 실제 Read 또는 TaskList와 StructuredOutput, 두 번 이상의 assistant 응답, 생성 토큰, 새 agent ID와 부모 이력 0개를 확인했다. CLI·stdio 호출은 별도 검사했으며 이 진단 개수에는 포함하지 않는다.

최종 라이브러리 SHA-256은 `14dbfaae191b1d1add2cdaa186fb45edc91955ba12c69b0c47065873a669ae40`, Mach-O UUID는 `171A67DA-AD30-3AB3-A85E-4AEE8A04A079`이다. 소스·설치 라이브러리 바이트, 세 실행 파일의 UUID와 CMake RPATH 변환 후 바이트, 공개 헤더 41개·문서·카탈로그·라이선스를 비교한다. 실제 설치 소비자가 `build/agent-hooks-stage`의 라이브러리를 로드하며 iillm은 모델 런타임에 직접 링크하지 않는다. 0.31 C++ 공개 구조체 변경으로 consumer 재빌드가 필요하다.

첫 메모리 검사 전체는 64/64로 통과했다. 최종 native fixture 변경 후 재검사에서는 기존 셸 권한 분석 테스트가 149 ms 시점에 안전 거부로 돌아와 63/64였다. 당시 실제 모델 검사도 실행 중이었으며 이를 원인으로 확정하지 않는다. 해당 실패는 agent-hooks-sanitizer-parser-timeout.*에 보존했다. 추론 작업 종료 후 단독 검사와 순차 전체 64/64를 통과했으며, 간헐적인 시간 초과의 원인이 해결되었다고 주장하지 않는다.

게시 전 Task 검증의 잠금 재진입과 변경 덮어쓰기, MCP 검증기의 잘못된 작업 목록 소유권을 먼저 실패하는 테스트로 확인했다. 첫 실제 Qwen3 검사에서는 복합적인 gate 조건에서 차단 대상 경로를 잘못 허용했다. 두 번째에서는 중첩된 원래 사용자 요청을 검증 작업으로 해석했고 실제 쓰기는 dontAsk가 막았다. 검증 표를 파일 basename별 명시적 boolean으로 바꾸고, 시스템 지시에 중첩된 입력과 수행할 조건의 경계를 명시했다. 두 실패는 `agent-hooks-source-native-initial.*`, `agent-hooks-source-native-second.*`에 보존하며 성공한 후속 검증으로 덮어쓰지 않는다. 두 번째 JSON은 첫 성공 시도까지의 이전 체크포인트이므로 두 번째 실패 자체는 해당 로그를 근거로 삼는다. 세 번째 검사에서 API·CLI는 통과했으나 Task 훅이 지연된 TaskList 조회를 생략했다. 조건을 이미 게시된 보드에 의존하도록 바꾸고 ToolSearch로 TaskList를 선택하도록 명시했다. 해당 실패는 agent-hooks-source-native-third.*에 보존한다. 이후 검증기는 실패 응답도 last_attempt에 즉시 기록한다. 네 번째 검사에서는 stdio의 차단 경로를 모델이 잘못 허용했다(agent-hooks-source-native-fourth.*). 최종 도구 훅 검증은 호출마다 파일에 기록한 JSON 판단을 읽는 조건으로 바꿔 실행 계층을 분리해 검증한다. 의미 기반 정책 판단의 정확성을 통과했다고 주장하지 않으며 호스트 정책은 계속 별도로 검사한다.

증거는 `build/agent-hooks-verification.json`, `agent-hooks-linkage.json`, `agent-hooks-tested-source.json`, 소스/설치 native 보고서, 전체 로그/XML, `agent-hooks-publication.json`이다. 커밋과 실제 원격 HEAD 일치는 게시 기록에서 따로 확인한다. 전체 훅 설정·플러그인/스킬별 병합·남은 생명주기·OS 샌드박스·다른 하네스 영역·제품 앱과 다른 플랫폼 검증은 미완료이다. 이번 설치본은 SDK 검증용이며 Society·Dreamscapes를 다시 패키징하지 않는다. iPhone은 사용자 지시로 제외한다. 전체 목표는 계속 진행 중이다.

## 2026-09-15 C++ 프롬프트 훅 (0.30.0)

단일 모델 판단 훅을 C++ Engine·ToolRunner와 인증 API·native IPC CLI·MCP HTTP/stdio에 연결했다. 대화 스냅샷, 미완결 도구 호출의 임시 짝, 입력/출력 상한, 별도 취소 기한, JSON 스키마·요청별 추론 모드와 모델 사용량 진단을 추가했다. 원본 대화에서 도구를 재실행하지 않는다. 실제 검증 중 발견한 HTTP SSE의 작업 취소 후 잘림도 수정했다. 계약과 원본 차이는 [PromptHooks.md](PromptHooks.md)에 기록한다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 65/65, 143.01초 |
| ASan·UBSan, llama 비활성 Debug | 62/62, 147.88초 |
| 새 설치 소비자(공식 MCP 교차 검사 2건 포함) | 36/36, 41.54초 |
| 호스트 모델 문맥 | 원본 이력 보존, pending/실제 도구 결과 구분, Task 트랜잭션 거부, 자식 시작/종료 문맥 |
| 실행 제약 | 잘못된 JSON/타입/도구 호출 거부, 입력/출력 상한, 부모와 구분한 시간 초과, once·중복·공유 슬롯 |
| 권한 | true가 호스트 Deny를 우회하지 않음; PermissionRequest false의 중단을 직접/앱 요청 경로에서 확인 |
| 네이티브 문법 | 실제 Qwen2.5 0.5B에 비-JSON 출력 지시를 주어도 tool_grammar=false 상태에서 고정 ok:true JSON 생성; 소스·설치 소비자 |
| 네이티브 추론 모드 | 실제 llama 템플릿의 요청별 true/false와 원래 로딩 기본값 복원; ServiceModel 측정/생성 전달 일치 |
| Qwen3 8B API | 소스·설치본 각각 파일 허용/차단, Stop 중단, UserPromptSubmit 차단의 4개 실행 |
| Qwen3 8B CLI·Task | 양쪽에서 native IPC 입력 차단 및 Task 생성 허용·게시 전 차단 |
| Qwen3 8B MCP | 양쪽에서 HTTP 직접 Write 3건(허용·모델 차단·호스트 거부), 공식 Python SDK stdio Write 2건 |
| SSE 오류 | 연결된 훅 취소/백엔드 종료 오류에 done·[DONE]·정상 chunk 종료; 실제 Task 차단 교차 검사 |

큰 모델은 model://qwen3-8b-q4, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 파일을 이번 검증에서 다시 해시했다. 실제 모델 질의에서 기록한 훅 진단은 소스 13개, 설치본 13개이며 UserPromptSubmit·PreToolUse·Stop·TaskCreated를 포함한다. CLI/stdio의 판단도 별도 교차 검사했지만 이 개수에는 진단을 수집한 SSE 호출만 센다. 훅 토큰은 메인 RunResult 토큰과 별도이다.

최종 라이브러리 SHA-256은 `3e2dd7510bcef61dc063fa03784da72cea50d44a555c9b4aabb5eff00b96e5b7`, Mach-O UUID는 `4BBD5D5F-8634-3A04-A7E5-398BF385188E`이다. 소스와 설치 라이브러리, 세 실행 파일의 UUID와 설치 RPATH 변환 후 바이트, 41개 공개 헤더·문서·카탈로그를 대조한다. 실제 소비자의 로더가 `build/model-hooks-stage` 라이브러리를 선택하며 얇은 iillm은 모델 런타임에 링크하지 않는다. 새 ABI는 0.30 헤더/라이브러리로 소비자를 다시 빌드해야 한다.

최초 컴파일 실패와 SSE 재현 실패를 보존한다. 실제 모델 첫 검사는 인증 fixture의 필수 id 누락으로, 두 번째 검사는 실제 SSE 취소 잘림으로 중단됐다. 첫 전체 검사는 64/65로 로컬 파라미터 출처 해시만 실패했다. 기존 생성기를 통해 Types.h의 선언 위치와 해시만 갱신하고 16개 생성 제어 내용이 동일함을 대조한 뒤 최종 전체 검사를 통과했다. 첫 sanitizer 전체 검사에서는 공식 MCP stdio 초기화 1건이 10초 기한을 넘겼다. 변경 없이 단독 검사와 후속 전체 검사를 통과했으며 원인 해결로 주장하지 않는다. 후속 실제 모델 검사는 MCP의 빈 커서용 SSE 이벤트를 검증 코드가 JSON으로 읽어 중단됐다. 검증 코드만 수정하고 빈 이벤트·종료 마커·잘못된 JSON 회귀 검사, Release·sanitizer의 해당 CLI 검사, 소스·설치본 전체 실제 모델 검증을 다시 통과했다. 앞선 전체 CTest 65/62/36 기록 이후 C++ 구현과 바이너리는 변경하지 않았다. 이 실패들을 성공 기록으로 대체하지 않는다.

증거는 `build/model-hooks-verification.json`, `model-hooks-linkage.json`, `model-hooks-tested-source.json`, `model-hooks-*-native.json`, 각 최종 로그/XML과 `model-hooks-publication.json`에 분리한다. 커밋/원격 HEAD와 최종 전체 소스 해시는 게시 기록에서 확인한다.

도구를 실행하는 type:agent 훅, 전체 훅 설정/스킬/플러그인 병합·생명주기, OS 샌드박스, 남은 하네스와 전체 앱/플랫폼 검증은 미완료이다. 이번 패키지는 별도 SDK 검증용 설치본이며 Society·Dreamscapes 앱을 재패키징하지 않았다. iPhone은 사용자 지시로 제외한다. 전체 목표는 계속 진행 중이다.

## 2026-09-15 C++ HTTP 훅 (0.29.0)

HTTP/HTTPS POST와 JSON 응답을 기존 명령·C++ 훅의 생명주기·권한 결정 경로에 연결했다. URL/환경 허용 목록, 직접 연결 DNS 주소 고정, 원래 Host·SNI·인증서 검증, 프록시, 응답/기한/취소 제한을 제공한다. 기존 Qt 6.8.3 Network를 사용하며 새 생산 의존성은 없다. Python은 독립 검증용 HTTP/HTTPS 피어와 클라이언트이다. 설정과 참조 차이는 [HTTPHooks.md](HTTPHooks.md)를 따른다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 62/62, 125.72초 |
| ASan·UBSan, llama 비활성 Debug | 60/60, 149.23초 |
| 별도 설치 소비자 | 33/33, 35.58초; HTTP 훅과 독립 TLS 포함 |
| 입력/응답 | UTF-8 JSON POST, 허용 환경만 헤더 치환, 2xx 객체 결정, 빈/비정상 응답, 리다이렉트 거부 |
| 전송 제약 | URL 차단, private/mapped IP 차단, localhost DNS·Host, HTTP 프록시·NO_PROXY, 응답 크기·기한·취소 |
| 독립 TLS 피어 | DNS localhost 인증서의 SNI/Host 유지, IP 호스트 불일치·비신뢰 인증서 거부 |
| 공유 실행기 | once 동시 호출, 취소 후 용량 회수, HTTP/명령 PermissionRequest 첫 결정, URL/조건 중복 처리 |
| API·CLI·MCP | 실제 입력 변경·거부·Task 이벤트·세션 시작/입력/clear/종료, 권한 저장·재시작·클라이언트 격리 |
| 실제 Qwen3 8B | 소스/설치본 각각 9회: Write 허용/차단, Stop 취소, 입력/clear 문맥 소비, 승인 입력 변경/중단, 권한 저장/재시작 |
| 공식 MCP stdio | SDK 1.26.0으로 실제 Write/Read·입력 변경·거부·세션 제어·권한 재사용 |
| 설치 산출물 | 공개 헤더 41개·문서·카탈로그·라이선스, stage 실제 로딩, 얇은 CLI 연결 검증 |

수정 전 HTTP 설정 거부는 http-hooks-red.log, 중복 URL을 두 번 호출한 실패는 http-hooks-dedup-red.log, 줄바꿈으로 끝난 헤더명을 받아들인 설정 검사의 실패는 http-hooks-header-red.log에 보존한다. 서로 다른 조건 검사는 네이티브 권한 규칙에 계층형 설정의 경로 표기를 사용한 테스트 구성 오류였으며 상대 경로 규칙으로 수정했다. 이 최초 결과는 http-hooks-focused.log에 있다. 첫 Release 실행은 헤더명 경계 검사를 추가하기 위해 중단했다. 종료 전 부분 로그는 http-hooks-release-before-header.log에 보존하며 위 표는 수정 후 전체 재검증이다.

잘못된 호스트 설정 20건은 모델 초기화 전에 거부했다. HTTP 피어는 실제 POST의 인증 헤더·JSON 본문을 검사하며 허용되지 않은 환경 값이 전달되지 않음을 확인했다. 외부 테스트 피어는 기존 결정 fixture를 실행하므로 API·MCP의 도구 결과와 세션 상태를 동일한 계약으로 대조한다. 모델 추론은 API HTTP이며 MCP 결과는 직접 도구/세션 제어 검증이다. 모든 MCP 도구를 모델이 자율 선택했다는 의미가 아니다.

모델은 model://qwen3-8b-q4, 5027783488바이트, SHA-256 d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785이다. 이번 검증에서 파일 해시를 다시 계산했다. context 8192, temperature 0, thinking=false/tool_grammar=false이며 일반 출력 상한 1024, Stop 사례 상한 64이다. 임의 문자열을 실제 Write 결과와 비교했고 차단·중단 대상 파일은 생성되지 않았음을 확인했다.

설치 prefix는 build/http-hooks-stage, 소비자는 build/http-hooks-consumer/build이다. 라이브러리 SHA-256은 ad4ab648b4a48937f1f9dca178a67a2ca88edfa8e1524339c6e0fe6494872ff6, Mach-O UUID는 D19346AD-0FCE-3FD3-B2D6-F7ED5CA9C410이다. 소스/설치 라이브러리 해시와 실행 파일 UUID·설치 RPATH 변환 후 해시가 일치한다. 경로 환경 변수를 비운 소비자가 stage 라이브러리를 로드했으며 iillm은 libiiLocalLLM/llama/ggml에 직접 링크하지 않는다. 공개 CommandHookOptions 변경으로 C++ 소비자는 0.29 헤더·라이브러리와 함께 재빌드해야 한다.

증거는 build/http-hooks-verification.json, http-hooks-linkage.json, 각 final.log/XML, source/installed-native.json이다. HTTPS 프록시 전송, 샌드박스 프록시 통합, CLI 추가 CA/mTLS, 로컬 async 훅 실행기, 전체 생명주기·설정/스킬/플러그인 병합, 다른 OS/실제 앱 전체 검증은 남아 있다. 이번 단계에서 Society/Dreamscapes를 재패키징하지 않았으며 iPhone은 사용자 지시로 제외한다. 기존 stage와 사용자 daemon은 보존했다. 전체 하네스 목표는 partial이다.

## 2026-09-15 HTTP·MCP 제어 용량 예약 (0.28.0)

일반 HTTP 응답과 제어 응답의 동시 한도를 분리했다. 응답을 처리기에 접수할 때 예약하고 JSON/SSE 전송 종료·실패 때 회수한다. MCP도 일반/제어 활성·보관 스트림을 분리하고 재접속·혼합 배치에서 원래 분류를 유지한다. C++과 CLI 설정은 [ControlCapacity.md](ControlCapacity.md)를 따른다. 기존 cpp-httplib Response.user_data와 원자적 카운터를 재사용했으며 새 생산 의존성은 없다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 59/59, 95.38초 |
| ASan·UBSan, llama 비활성 Debug | 57/57, 134.46초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 31/31, 56.17초; HTTP/MCP HTTP 회귀 검사 포함 |
| HTTP 응답 수명 | 일반/제어 독립 포화, health, 느린 JSON·SSE 네 조합, 소켓 종료 후 용량 회수 통과 |
| MCP 활성·보관 용량 | 일반 활성·보관 포화 중 제어, 제어 전역 한도, 연결 종료 뒤 보관 한도, 취소 후 회수 통과 |
| 독립 MCP HTTP 클라이언트 | 혼합 배치·위조 분류 거부, 일반/제어 재접속 한도 유지, 다른 세션의 cursor 거부, 재실행 없음 통과 |
| 실제 daemon·MCP HTTP | 일반 용량 1에서 추가 일반 요청 429와 승인 조회·응답 성공을 함께 확인 |
| 실제 Qwen3 8B, 소스/설치본 | API HTTP 용량 1에서 Write 승인 입력 변경 후 정확한 임의 바이트 생성, HTTP interrupt 후 cancelled 통과 |
| 공식 Python MCP stdio | MCP SDK 1.26.0의 확장 제어 응답으로 실제 Write 입력 변경·파일 생성 통과 |
| 설치 산출물 | 공개 헤더 41개·문서·카탈로그·라이선스 일치, 실제 stage 라이브러리 로딩 |

수정 전 HTTP 검사는 일반 요청의 1,800 ms 기한 때문에 제어 조회가 지연되어 실패했다. MCP 검사는 일반 활성·보관 스트림 포화에서 제어 조회가 HTTP 429로 실패했다. control-capacity-red.log와 control-capacity-mcp-red2.log에 보존한다. 초기 신규 MCP 테스트에서 비동기 세션 초기화 전 빈 목록을 접근한 오류, QTRY 조건이 알림을 중복 소비한 오류는 테스트 구성 문제로 구분해 수정했다. 해당 오류를 생산 서버 충돌 수정으로 보고하지 않는다. 첫 전체 검사에서 기존 8개 동시 역방향 요청 테스트가 기본 제어 한도 4개를 초과해 429로 실패했다. 라우팅 검사는 제어 용량 8개를 명시하도록 수정했고 기본 한도 초과의 429 검사는 유지한다. 첫 실행 로그는 control-capacity-release-first.log에 남겼다.

CLI 용량 설정의 잘못된 숫자·범위·HTTP 옵션 조합 16건을 모델 초기화 전에 거부했다. 기존 권한 설정 오류 16건도 통과했다. 기본 용량의 permission_requests_wire와 제한 용량의 control_capacity_wire를 모두 실행했다. API의 native IPC TaskCreate 승인 경로도 유지한다. MCP 실제 파일 도구 결과는 HTTP와 공식 stdio에서 확인했으며 MCP Qwen 모델 실행 결과를 의미하지 않는다.

모델은 model://qwen3-8b-q4, 5027783488바이트, SHA-256 d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785이다. 이번 검증에서 해시를 다시 계산했다. context 8192, temperature 0, 출력 상한 1024, thinking=false/tool_grammar=false이다. 소스와 설치본 각각 같은 HTTP 승인 흐름의 허용·중단 두 경우를 실행했다.

설치 위치는 build/control-capacity-stage, 소비자는 build/control-capacity-consumer/build이다. 라이브러리 SHA-256은 e388d7ce176f2e4bb4b67ed3b977e12d8e0aff5c759454c1439eb83deabed966, Mach-O UUID는 98D0A13D-0090-31FD-BE22-437519AF8F6C이다. 실행 파일 UUID 및 CMake 설치 RPATH 변환 후 해시가 일치한다. iillm은 libiiLocalLLM/llama/ggml에 직접 링크하지 않는다. 경로 환경 변수를 비운 소비자 로딩을 확인했다. 공개 옵션 구조체와 RpcHandler 인터페이스가 바뀌므로 C++ 소비자는 0.28 헤더·라이브러리와 함께 재빌드해야 한다.

증거는 build/control-capacity-verification.json, control-capacity-linkage.json, 각 final.log/XML, source/installed-native.json에 보존한다. 연결 큐·인증 콜백·메시지/이벤트 예산과 제어 용량 자체는 여전히 제한된다. 임의의 비협조 C++ 콜백 중단, 영속 승인 복구, 실제 앱 승인 UI, 전체 하네스는 완료 범위가 아니다. 이번 SDK 단계에서 Society/Dreamscapes를 다시 패키징하지 않았으며 iPhone은 사용자 지시로 제외한다. 기존 stage와 사용자 daemon은 보존했다. 전체 목표는 partial이다.

## 2026-09-15 앱 권한 요청·응답 중개 (0.27.0)

C++ PermissionRequests와 ToolRunner의 훅/앱 경쟁을 구현했다. 첫 결정만 입력·정책 변경에 적용하고 늦은 응답, 같은 결정 재전송, 취소·기한·종료를 처리한다. API는 인증 클라이언트별, MCP는 연결별 채널을 사용하며 parent/child 실행으로 소유권을 전달한다. API/native IPC와 MCP 확장 제어 경로, 호스트 CLI 설정, 독립 MCP 제어 작업 풀을 제공한다. 응답 메서드는 모델 도구 목록에 없다. 계약과 참조 차이는 [PermissionRequests.md](PermissionRequests.md)에 있다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 58/58, 109.97초 |
| ASan·UBSan, llama 비활성 Debug | 56/56, 121.84초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 29/29, 26.20초 |
| C++ 실제 파일·동시성 | 앱/훅 승자, 늦은 입력 무시, 8개 동시 응답의 단일 승자, 중복·채널 격리, 기한·취소·이력 제한 통과 |
| API/MCP dispatcher | 일반 큐 포화 중 조회·응답, 다른 클라이언트/연결 거부, 모델/자식 실행의 채널 전달 통과 |
| API·native IPC CLI | HTTP TaskCreate를 IPC 응답으로 변경, 거부 시 미게시, 잘못된 응답과 재전송 통과 |
| MCP HTTP·공식 Python stdio | 앱 입력 변경으로 실제 파일 생성, 변경 대상의 호스트 deny 재검사, 연결 격리와 비도구 제어 경로 통과 |
| 실제 Qwen3 8B, 소스/설치본 | API 모델 Write의 승인 입력을 native IPC로 변경해 정확한 임의 바이트 생성, 앱 interrupt로 run cancelled 통과 |
| 설치/ABI | 공개 헤더 41개·문서·카탈로그·라이선스 일치, 실제 stage 라이브러리 로딩 |

새 공개 API가 없는 컴파일 실패는 permission-requests-red.log와 permission-requests-race-red.log에 보존한다. 새 테스트의 임시 JSON 객체 참조 수명 오류는 Release에서 재현되었고 CrashReporter 프레임이 테스트의 QJsonValueConstRef 접근을 가리켰다. value()로 값을 소유하도록 고쳤다. ASan의 Qt 바이너리 접근에서 재현되지 않았다는 이유로 해결로 간주하지 않고 Release도 다시 검사했다. SDK 생산 코드의 충돌 수정으로 보고하지 않는다. MCP 테스트의 event/kind 필드, sanitizer의 긴 Unix 소켓 경로, 공식 Python RequestParams의 확장 필드 누락도 각각 수정했다. 원문은 permission-requests-transport-tests.log, permission-requests-retest.log, permission-requests-san-repair-tests.log, permission-requests-official-focused.log에 있다.

호스트 설정의 잘못된 타입·필드·범위·권한·위치 16건을 모델 초기화 전에 거부한다. 공식 Python MCP SDK 1.26.0은 확장 필드를 허용하는 RequestParams 하위 모델과 표준 send_request로 실제 stdio 응답을 보냈다. MCP 진행 알림은 중첩 모델·도구·훅에 단조 증가 step을 부여하고 원래 진행값을 별도 메타데이터로 보존한다.

모델은 model://qwen3-8b-q4, 5027783488바이트, SHA-256 d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785이다. 파일 해시를 이번 검증에서 다시 계산했다. context 8192, temperature 0, 출력 상한 1024, thinking=false/tool_grammar=false로 실행했다. 실제 모델 검증은 API 실행과 native IPC 승인이다. MCP에서는 실제 파일 도구의 HTTP/공식 stdio 실행과 C++ 모델/자식 채널 전달을 검사했으며 이를 MCP Qwen 생성 증거로 대체하지 않는다.

설치는 build/permission-requests-stage, 소비자는 build/permission-requests-consumer/build이다. 라이브러리 SHA-256은 b2954560ae85f729155d787c9f74c8c709b0af53ed074c5abfc4f1527310b404, Mach-O UUID는 52EB33BD-947A-391A-B3D1-BCBC5499EFA0이다. 실행 파일 UUID 및 CMake 설치 RPATH 변환 후 해시가 일치하고 iillm은 libiiLocalLLM/llama/ggml에 직접 링크되지 않는다. 라이브러리 경로 환경 변수를 비운 설치 로딩을 확인했다. build/permission-requests-verification.json, permission-requests-linkage.json, 각 final.log/XML과 source/installed-native.json에 증거를 보존한다.

accepted는 결정 접수이며 도구·정책 저장 완료가 아니다. 완료 이력은 영속적이지 않다. 협조하지 않는 C++ 콜백을 강제로 중단하지 않는다. 독립 제어 처리는 dispatcher 큐의 보장이고 HTTP 작업자/SSE 총량 포화의 제어 예약은 남아 있다. 자동 제안·분류기, 전체 OS/설정/플러그인/앱 기능 역시 남아 있다. Society/Dreamscapes를 이번 단계에서 다시 패키징하지 않았고 iPhone은 사용자 지시로 제외했다. 기존 stage·증거와 실행 중인 사용자 daemon은 보존한다. 전체 목표는 partial이다.

## 2026-09-15 승인 권한 갱신·저장·세션 상속 (0.26.0)

C++ SettingsPermissionPolicy에 규칙 추가/교체/삭제, 모드 변경, 디렉터리 추가/삭제를 구현했다. user/project/local 파일 저장과 session/cliArg 메모리를 구분하고 Engine fork/clear 및 자식 접수/재개에 런타임 상태를 전달한다. ToolRunner는 승인 입력의 스키마를 먼저 검사하고 정책 갱신 후 작업 경계·준비 대상·최종 거부를 확인한다. CLI·API·native IPC·MCP가 기본 정책 갱신을 사용한다. 계약과 참조 차이는 [PermissionUpdates.md](PermissionUpdates.md)에 기록한다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 56/56, 89.25초 |
| ASan·UBSan, llama 비활성 Debug | 54/54, 107.78초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 28/28, 26.28초 |
| C++ 실제 파일과 세션 | 영속 규칙/모드/디렉터리, 비권한 JSON 보존, session/cli 격리, 관리 정책, fork/clear/자식 상속과 재개 통과 |
| 저장 실패·동시 갱신 | 사전 검증 실패 시 JSON/메모리 미게시, 잠금 취소/시간 초과, 링크 거부, 8개 작성자 30회 반복 통과 |
| API·native IPC CLI | 기존 TaskCreate 승인 입력 변경·거부 시 미게시·인증 검사 통과 |
| MCP HTTP·공식 Python stdio | 다음 호출 승인 생략, clear 후 세션 grant 유지, 다른 클라이언트 격리, 새 프로세스의 파일 grant 복원과 세션 grant 미복원 통과 |
| 실제 Qwen3 8B, 소스/설치본 | 승인으로 규칙 저장 후 재시작한 daemon의 다른 클라이언트에서 재질문 없이 새 파일 생성 통과 |
| 설치/ABI | 0.26 공개 헤더 40개·문서·카탈로그·라이선스 일치, 실제 stage 라이브러리 로딩 |

새 정책 API가 없을 때의 컴파일 실패는 permission-updates-red.log에 보존했다. 초기 검사에서 추가 디렉터리가 없는 세션을 조회할 때 비상수 JSON 접근이 null 필드를 생성하는 오류를 발견하여 value() 읽기로 수정했다. 새 자식 재개 fixture가 호출 ID를 재사용하여 프로토콜 검증에 실패한 사례와 MCP clear의 응답 진단을 진행 알림에서 찾던 fixture 오류도 보존했다. 프로토콜 검증과 훅 검증을 제거하지 않았다. 경로 제거 시험의 초안은 호출자가 준 ToolContext 경로를 독립 권한으로 간주했으나 실제 구현은 이미 정책 값으로 교체했다. 이 기대값을 바로잡고 제거된 경로가 동일 호출에서도 실행되지 않음을 검사했다. 생산 코드의 경로 잔존 오류를 수정했다는 주장은 하지 않는다. 초안은 permission-updates-revocation-red.log에 보존한다. 관련 기록은 permission-updates-focused.log, permission-updates-integration-focused.log와 permission-updates-subagent-diagnostic.log이다.

동시 작성자 검사는 잠금 파일의 첫 O_CREAT 열기에서 ENOENT를 재현했다. Qt/SDK와 독립된 Python os.open 경합에서도 이 macOS 27/APFS 작업 경로의 320회 중 50회가 실패했다. 기존 파일 열기와 O_EXCL 생성을 분리하고 동일 기한·취소 안에서 다시 열도록 수정했다. O_NOFOLLOW·정규 파일·단일 링크 검사는 유지한다. 수정 후 8개 동시 작성자의 30회 반복과 전체 검사가 통과했다. 원문은 permission-updates-open-race.json, permission-updates-lock-path.log, permission-updates-lock-repair.log에 있다. 다른 OS 전체에 같은 파일 시스템 현상이 있다고 일반화하지 않는다.

실제 모델 검사는 기존 허용/거부 Write, Stop 취소, UserPromptSubmit 문맥, SessionStart(clear) 문맥, PermissionRequest 입력 변경/interrupt에 영속 승인과 재시작 검사를 추가했다. 첫 호출은 훅이 localSettings에 제한된 Write 규칙을 저장한 뒤 임의 내용을 정확히 파일로 쓴다. daemon 재시작 후 다른 인증 클라이언트의 새 세션도 별도 파일을 만들며 PermissionRequest 이벤트는 없다. 소스와 설치본 각각에서 수행했다. MCP의 세션 grant는 clear에는 이어지고 다른 클라이언트나 stdio 새 프로세스에는 남지 않음을 별도로 확인했다.

모델은 model://qwen3-8b-q4, 5027783488바이트, SHA-256 d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785이다. 이번 검증에서 파일 해시를 다시 계산했다. context 8192, temperature 0, 출력 상한 1024, thinking=false/tool_grammar=false를 사용했다. 공식 stdio 클라이언트는 Python MCP SDK 1.26.0이며 permission-updates-source-native.json과 permission-updates-installed-native.json에 전송 결과와 훅 이벤트를 보존한다. 전체 테스트는 빌드 종료 후 직렬로 실행했다.

설치는 build/permission-updates-stage, 소비자는 build/permission-updates-consumer/build이다. 라이브러리 SHA-256은 6afa60a62b673318ad160e9e4c615af25fd9ce2094d00f9bd802bf2a0d7e4717, Mach-O UUID는 6C7CE84C-41AD-3583-AF81-8D4F573841D4이다. 실행 파일 UUID와 CMake 설치 RPATH 변환 후 해시도 일치하며 iillm은 libiiLocalLLM/llama/ggml에 직접 링크되지 않는다. 라이브러리 경로 환경 변수를 비우고 설치 라이브러리 로딩을 확인했다. 전체 증거는 build/permission-updates-verification.json, permission-updates-linkage.json 및 각 final.log/XML이다.

파일 하나의 교체는 원자적이지만 여러 파일과 도구 실행 전체는 트랜잭션이 아니다. 파일 설정은 그 출처를 함께 읽는 다른 세션에도 반영되고 메모리 상태는 정책 객체/세션 수명에 제한된다. 자동 권한 제안, 원격 승인 중개, 분류기/PermissionDenied, 전체 보안·설정·앱/플랫폼 검증은 남아 있다. Society/Dreamscapes를 이번 SDK 단계에서 다시 패키징하지 않았고 iPhone은 사용자 지시로 제외했다. 이전 stage와 증거는 보존하며 전체 목표는 [HarnessParity.md](HarnessParity.md)의 partial 상태이다.

## 2026-09-15 권한 요청과 구조화 호스트 응답 (0.25.0)

C++ ToolRunner/Engine에 Ask 전용 PermissionRequest, 구조화 응답, 입력 수정 후 스키마·준비 대상·최종 호스트 거부 재검사, 일반 거부와 실행 취소, 명시적 권한 갱신 처리기를 연결했다. API·native IPC·MCP와 임베디드 호스트가 같은 경로를 사용한다. 병렬 명령의 첫 완료 결정은 behavior·입력·권한 목록을 함께 보존하며, 구조화 응답 콜백이 있으면 도구를 직렬 분류한다. 계약과 참조 차이는 [PermissionRequest.md](PermissionRequest.md)에 있다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 55/55, 108.50초 |
| ASan·UBSan, llama 비활성 Debug | 53/53, 114.97초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 27/27, 48.13초 |
| API·native IPC CLI | TaskCreate 승인 입력 변경, 거부 시 미게시, 인증과 임베디드 구조화 응답 통과 |
| MCP HTTP·공식 stdio | 실제 파일 변경, 거부, 수정 입력의 스키마/명시적 거부 재검사, 갱신 처리기 없는 요청 실패 통과 |
| 실제 Qwen3 8B, 소스/설치본 | 권한 훅에서 입력을 변경하여 정확한 파일 생성, interrupt로 cancelled 종료 통과 |
| 설치/ABI | 0.25 공개 헤더 40개·문서·카탈로그·라이선스 일치, 실제 설치 라이브러리 로딩 |

새 응답 API가 없을 때 컴파일 실패한 permission-request-red.log를 보존했다. 초기 전송 검사에서는 공식 stdio 연결이 앞선 HTTP 연결에서 만든 파일을 읽지 않고 덮어쓰려다 기존 read-before-edit 검사에 실패했다. fixture에 해당 파일을 먼저 Read하는 단계를 추가했다. 이 실패는 보호 규칙이 승인 입력 변경에도 유지되는 증거이며 생산 코드 결함을 고쳤다는 주장이 아니다. 원문은 permission-request-wire.log이다. 최종 전체 검사는 모든 빌드를 마친 뒤 직렬로 실행했다.

네이티브 모델은 request-native.txt와 MODEL_CONTENT를 요청하고, 권한 훅은 프롬프트에 없는 임의 값을 permission-native-applied.txt로 쓰도록 입력을 교체했다. 원래 경로는 생성되지 않았으며 파일 내용과 PostToolUse 입력이 훅 응답과 일치했다. transcript의 모델 호출은 원래 입력을 유지한다. 별도 request-interrupt.txt 호출은 파일을 만들지 않고 cancelled로 끝났다. 기존 허용/거부 Write, Stop 중단, UserPromptSubmit 문맥, SessionStart(clear) 문맥 검사도 통과했다.

모델은 model://qwen3-8b-q4, 5027783488바이트, SHA-256 d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785이다. 해시를 이번 검증에서 다시 계산했다. context 8192, temperature 0, 응답 상한 1024, thinking=false/tool_grammar=false를 사용했다. 소스/설치 증거는 permission-request-source-native.json과 permission-request-installed-native.json이다. 공식 stdio 클라이언트는 Python MCP SDK 1.26.0이다.

설치는 build/permission-request-stage, 소비자는 build/permission-request-consumer/build이다. 라이브러리 SHA-256은 2b14af3e42a0f3075bcafd0e1800a54a3412a5d67f4762d717d57820ba22d538, Mach-O UUID는 84B6D858-B8EB-387D-B7C1-9F9DF1A2EFA1이다. 실행 파일 UUID와 CMake 설치 RPATH 변환 후 해시도 일치하며 iillm은 libiiLocalLLM/llama/ggml에 직접 링크되지 않는다. 런타임 경로 환경 변수를 비우고 설치 라이브러리 로딩을 확인했다. 전체 기록은 build/permission-request-verification.json, permission-request-linkage.json과 각 final.log/XML에 있다.

기본 설정의 지속 권한 갱신, 자동 제안 생성, 원격 request_id 응답 중개, PermissionDenied 분류기 재시도와 전체 하네스/앱 검증은 남아 있다. 이번 단계는 명시적 C++ 갱신 처리기를 연결하며 CLI가 갱신을 저장한 것으로 보고하지 않는다. Society/Dreamscapes 앱을 다시 패키징하지 않았고 iPhone은 사용자 지시로 제외했다. 이전 stage와 증거는 보존했으며 전체 목표는 [HarnessParity.md](HarnessParity.md)의 partial 상태이다.

## 2026-09-15 대화 초기화와 백그라운드 보존 (0.24.0)

C++ Engine::clearSession, 인증 API agent.sessions.clear, MCP iiLocalLLM.agent.clear 및 new_session을 구현했다. 이전 foreground 실행과 접수 호출을 취소·정리하고, 새 ID에 SessionStart(clear)를 즉시 실행한다. 백그라운드 셸의 실제 프로세스·출력과 자식 실행은 유지하며 완료 알림을 새 소유자에게 옮긴다. 연속 clear, 같은 자식의 여러 실행, 큐 상한 실패/재시도, 저장소 재개, 호스트 종료 경합을 검사했다. 정확한 범위와 부분 실패 계약은 [SessionClear.md](SessionClear.md)에 있다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 54/54, 107.40초 |
| ASan·UBSan, llama 비활성 Debug | 52/52, 108.63초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 26/26, 23.17초 |
| API·native IPC CLI | 소유권·새 ID·부모 기록·원본 보존·즉시 clear 시작 문맥 통과 |
| MCP HTTP·공식 stdio | 명시적 clear와 new_session 교체, 진행 호출 취소, 연결 종료/신호 정리 통과 |
| 실제 Qwen3 8B, 소스/설치본 | 허용 Write·거부·Stop 중단·제출 훅 문맥·clear 시작 훅 문맥의 파일 쓰기 통과 |
| 설치/ABI | 0.24 공개 헤더 40개·문서·카탈로그·라이선스 일치, 실제 설치 라이브러리 로딩 |

새 API가 없는 상태의 컴파일 실패(session-clear-red.log, session-clear-child-red.log, session-clear-engine-red.log)와 알림 순번 충돌의 실행 실패(session-clear-queue-red.log)를 보존했다. 통합 초기 검사에는 fixture 오류도 있었다. 임시 QJsonObject의 참조를 보관한 테스트를 소유 QJsonValue로 바꾸었고, task.output 중첩 필드를 읽고 AUTOMOC를 켰다. 기존 MCP 테스트의 종료 기대값은 새 보존 계약으로 갱신했다. 취소된 MCP 호출은 정상 RunResult가 아닌 JSON-RPC cancelled 오류이므로 검사도 그 전송 계약에 맞췄다. 이 fixture 수정을 생산 코드 결함 해결로 주장하지 않는다. 관련 6개 검사/전송 검사는 session-clear-preinstall.log에 있으며 최종 전체 검사는 모든 빌드가 끝난 뒤 직렬 실행했다.

실제 모델 검사는 사용자 프롬프트에 없는 임의 값을 SessionStart(clear)의 문맥으로만 전달하고 새 세션이 정확한 파일 내용을 Write로 생성하는지 확인했다. 새 기록은 시작 문맥 1개에서 출발하고, 이전 모델 세션의 SessionEnd(clear)와 신규 세션의 SessionStart(clear)/종료 other가 각각 한 번임을 대조했다. 이전 사용자 입력과 압축을 복사하지 않으며 새 모델 문맥 ID를 쓴다. 이 결과는 가중치/KV의 즉시 메모리 해제나 다른 모델·모바일·장기 부하의 검증을 뜻하지 않는다.

모델은 model://qwen3-8b-q4, 5027783488바이트, SHA-256 d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785이다. context 8192, temperature 0, 응답 상한 1024, thinking=false/tool_grammar=false의 기존 검증 프로필을 사용했다. 가중치 해시를 이번 검증에서 다시 계산했다. 소스/설치본 네이티브 증거는 session-clear-source-native.json 및 session-clear-installed-native.json에 보존했다. 공식 stdio 클라이언트는 작업 공간의 Python MCP SDK 1.26.0이다.

설치 경로는 build/session-clear-stage, 별도 소비자는 build/session-clear-consumer/build이다. 런타임 경로 환경 변수를 비운 채 검사했고 iillm이 libiiLocalLLM/llama/ggml에 직접 링크되지 않음을 확인했다. 라이브러리 SHA-256은 3ed3ebe3d7f8bf0d133531c10c386fd8bc59638202248db7a1f6818c0e2b8920, Mach-O UUID는 A73B165F-6942-33B9-B3A7-A8E01CE1191B이다. 소스와 설치 실행 파일의 UUID 및 CMake RPATH 변환 후 해시도 일치한다. 검증 원문은 build/session-clear-verification.json, session-clear-linkage.json, 각 final.log/XML 및 최초 실패 로그에 있다.

이번 단계에서는 제품 앱을 다시 패키징하지 않았다. iPhone은 사용자 지시로 제외했다. 사용자 데몬 PID 14909를 유지했으며 별도 검증 데몬만 실행·종료했다. 참조의 UI/팀/git/LSP/worktree/플러그인 캐시 초기화, 전체 훅과 실제 제품 consumer 검증 등은 여전히 남아 있다. SDK clear는 여러 저장소를 하나의 트랜잭션으로 묶거나 공유 MCP 연결을 초기화하지 않는다. 전체 목표는 partial이며 [HarnessParity.md](HarnessParity.md)의 범위를 유지한다.

## 2026-09-15 실제 세션 종료와 호스트 정리 (0.23.0)

C++ SessionEnd와 Engine::endSession/close를 실제 API·MCP 종료 지점에 연결했다. Stop의 턴 종료와 구분하며, 활성화별 중복 방지·취소·기록 보존·resume을 제공한다. 기본 1.5초의 공통 훅 예산을 적용하고 종료 거부·추가 문맥·새 초기 입력은 적용하지 않는다. [SessionEnd.md](SessionEnd.md)에 동시성, 프로세스 정리, 진단과 참조 차이를 기록한다. 새 생산 의존성 없이 Qt/C++ 실행기를 재사용한다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 53/53, 136.80초 |
| ASan·UBSan, llama 비활성 Debug | 51/51, 105.31초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 21/21, 15.73초 |
| C++ 종료 | 진행·대기 실행과 직접 네이티브 호출 취소, 활성화별 중복 방지·조회 재진입·resume·예외·공통 예산 통과 |
| 인증 API·IPC CLI | 소스/설치본의 소유권·사유 검증, logout·중복 종료·기록 보존 및 daemon 종료 훅 확인 |
| MCP | 소스/설치본의 교체 clear, HTTP DELETE other, 공식 SDK stdio 종료 및 SIGINT/SIGTERM 정상 정리 확인 |
| 실제 Qwen3 8B | 소스/설치본 각각 허용 Write·도구 거부·Stop 중단·훅 문맥의 임의 값 Write 통과 |
| 설치/ABI | 공개 헤더 40개·문서·카탈로그·라이선스 일치, 0.23 실제 설치 라이브러리 로딩 |

TDD 초기 실행은 SessionEnd 설정 거부와 agent.sessions.end 미존재의 두 실패를 재현했다(session-end-red.log). 첫 관련 검사 5개는 10.95초에 통과했다. 이후 추가한 fixture 두 개는 필수 description 누락과 MCP agent.run의 기본 Ask 정책 때문에 의도한 경로에 진입하지 못했다(session-end-target-3.log). fixture에 올바른 입력과 명시적 호스트 허용 정책을 넣고 4.82초에 통과했다(session-end-target-4.log). 해당 실패를 생산 코드 결함 수정으로 주장하지 않는다. 모든 빌드가 끝난 뒤 최종 전체 검사를 직렬 실행했다.

API 검사는 단일 일반 worker가 실행 중인 상황에서도 별도 제어 worker가 같은 세션의 실행·대기 요청을 취소하고 SessionEnd 이전에 경계를 정리하는지 확인한다. 취소된 대기 요청이 자동으로 resume하지 않으며 다음 명시적 요청만 resume한다. Engine 검사에서는 다른 세션의 worker를 기다리지 않고 아직 시작하지 않은 작업을 제거한다. 직접 TaskCreate 취소는 저장을 롤백하고 호출자의 원래 취소 토큰을 변경하지 않는다. 훅에서 기록 조회가 가능하고 동일 세션 새 실행은 거부된다. 실제 명령의 reason 매처와 공유 시간 예산도 검사했다.

wire 검사는 --agent-hooks/--hooks의 실제 명령 프로세스를 사용한다. 종료 훅의 continue:false와 block을 받아도 CLI는 ended=true를 반환하며, 기존 원문을 그대로 조회한다. API 프로세스 종료 후 owner의 other가 한 번, 명시적으로 닫은 세션의 logout이 한 번임을 검사했다. MCP 교체 전 ID의 clear, HTTP DELETE 수락 후 비동기 정리 완료, 공식 Python MCP SDK 1.26.0의 stdio 연결 종료, 별도 stdio 프로세스의 SIGINT/SIGTERM 후 정상 종료 코드와 other 1회를 확인했다. 종료 진단은 영속 감사 로그가 아니다.

Qwen3의 허용/거부 Write 호출 수는 소스 1/1, 설치본 1/1이다. 허용 파일의 정확한 바이트, 거부 파일 부재, 도구 호출/결과 정합성과 HOST_STOP을 확인했다. 사용자 프롬프트에 없는 임의 값을 제출 훅의 문맥으로만 전달하여 실제 Write 결과가 일치하는지도 확인했다. 각 실제 모델 세션은 호스트 정리 경로의 SessionEnd에도 참여한다. 다른 모델·장기 부하·모바일 검증을 대신하지 않는다.

모델은 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`의 Qwen3 8B Q4_K_M이며 가중치 해시를 다시 검사했다. 컨텍스트 8192, temperature 0, 최대 출력 1024(Stop은 64), enable_thinking=false, tool_grammar=false 조건이다. 무결성 검증을 포함한 API 시작은 소스 55.927초, 설치본 53.890초였다.

설치 prefix는 build/session-end-stage, 소비자는 build/session-end-consumer/build이다. 라이브러리 SHA-256 `aaeb2f02c690b71e4e31681da61135367387efd4495738b76a06adc8dc267ad9`, Mach-O UUID `FA32C427-5E62-312D-81D6-C70C66D31EA9`가 소스/설치본에서 일치한다. CLI·daemon·MCP 모두 0.23.0이며 환경의 DYLD_LIBRARY_PATH·DYLD_FRAMEWORK_PATH·DYLD_FALLBACK_LIBRARY_PATH·LIBRARY_PATH를 제거하고 검증했다. 실제 설치 로딩, 추론 라이브러리에 연결하지 않는 CLI, CMake의 설치 RPATH 변경 정규화 후 실행 파일 바이트, 공개 헤더·문서·라이선스·내부 C 심볼 비공개를 확인했다. ASan은 malloc_context_size=0, UBSan은 halt_on_error=1:print_stacktrace=1이다.

최종 증거는 build/session-end-verification.json, session-end-linkage.json, 소스/설치 native JSON과 각 log/JUnit이며, 커밋·원격 게시 증거는 session-end-publication.json에 별도 기록한다. 주 실행과 네이티브 셸의 정리, 자식 에이전트에 대한 취소 요청을 구분한다. C++ 콜백의 비협조적 대기, 전체 wall-clock 상한, 크래시에서의 훅 보장, 영속 종료 진단은 제공하지 않는다. SessionStart(clear)·전체 clear 정책·watchPaths·권한 훅과 나머지 실행기, 앱/플랫폼 검증은 남아 있어 전체 하네스 목표는 partial이다. 이번 SDK 단계에서 Society·Dreamscapes를 다시 패키징하지 않았다.

## 2026-09-15 C++ 사용자 입력·세션 시작 (0.22.0)

UserPromptSubmit과 SessionStart를 직접 입력·사용자 스킬·입력 큐·활성화·재개·압축에 연결했다. 차단·중단 판정을 원본에 저장하고 일반 모델 문맥에서의 적용을 구분한다. 큐의 준비와 저장/확인을 분리하여 콜백의 큐 조회·추가·철회, 긴급 입력 취소와 저장 후 확인 실패 복구를 지원한다. 기존 Qt/C++ 실행기를 사용하며 새 생산 의존성은 없다. [InputLifecycle.md](InputLifecycle.md)에 상세 계약과 남은 범위를 기록한다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 52/52, 75.23초 |
| ASan·UBSan, llama 비활성 Debug | 50/50, 100.66초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 20/20, 16.22초 |
| 실제 Qwen3 8B | 소스·설치본 각각 허용 Write·도구 차단·Stop 중단·훅 문맥의 임의 값으로 Write 통과 |
| API·IPC CLI·MCP HTTP·공식 SDK stdio | 소스·설치본에서 사용자 입력 차단/중단과 명령 진단, 기존 도구 권한·인증 경계 통과 |
| API 입력/세션 생명주기 | 큐 확인·원본 판정·초기 입력의 큐/제출 훅 적용·최초 활성화 1회·호스트 재시작 resume 확인 |
| 잘못된 호스트 설정 | 소스·설치본 각각 16개 모델 초기화 전 거부 |
| 설치/ABI | 공개 헤더 40개·문서·카탈로그·라이선스 일치, 0.22 설치 경로 로딩 |

TDD 최초 실행은 새 생명주기 검사 4개가 실패했다(`input-lifecycle-red.log`). 준비/저장 분리 뒤 Release 큐 경로에서 임시 QJsonObject의 QJsonValueRef 수명으로 충돌하는 문제를 재현했고 value()로 값을 소유하도록 수정했다(`input-lifecycle-target.log`). 해당 충돌은 초기 ASan 단독 실행에서는 재현되지 않았다. 최종 Release·ASan 전체 결과로 수정 후 동작을 따로 확인했다. 압축 fixture의 짧은 대화는 요약 안내문보다 작아 정상적으로 축소를 거부했으므로 충분한 기존 이력을 제공하도록 고쳤다.

관련 검사 8개 묶음은 15.72초에 통과했다. 그 뒤 최초 Release 전체는 52/52, 103.30초에 통과했으나 최종 검토에서 SessionStart 초기 입력이 더 작은 Engine 상한을 우회하는 문제를 찾았다. 32자 상한에 33자 초기 입력을 넣는 실패를 먼저 재현하고, 게시 전에 maxInputCharacters를 검사하도록 수정했다(`input-lifecycle-limit-red.log`). 이전 전체 로그는 `input-lifecycle-release-before-limit.log/xml`에 보존했다. 모든 빌드 종료 뒤 현재 최종본의 전체 검사를 직렬로 다시 통과했다. C·C++ ASan/UBSan은 malloc_context_size=0 및 halt_on_error=1:print_stacktrace=1 조건이다.

상한 수정 뒤 최초 전체 실행은 51/52, 77.39초였으며 discovery_mcp_stdio의 자식 프로세스가 dyld Allocator.cpp allocated() assertion으로 종료됐다. 같은 바이너리의 단독 재실행은 통과했다. 실패 로그/JUnit은 input-lifecycle-release-dyld-failure에 보존했고 이후 최종 전체 실행을 별도로 확인했다. 로더 오류의 원인을 수정했다고 주장하지 않는다.

C++ 검사는 원문 차단/중단의 차이, 차단된 경로의 지침 제외, 저장한 입력 판정의 재사용, 8개 혼합 소비자의 중복 없는 전달, 준비 중 게시/철회/취소, 긴급 입력 후 재준비, 입력 확인 뒤 배치 중단, 사용자 스킬의 원래 slash 인자, 자식의 SubagentStart와 주 대화 시작 구분, 활성화/재개/압축과 분기를 검사한다. 자식·압축 검사는 결정적인 Model 대역이며 실제 모델 전체 자식 워크플로 검증과 구분한다.

실제 명령 wire 검사는 API에서 차단/중단의 generated_tokens=0, 원본 disposition, 큐 확인, 원격 userPrompt 우회 거부, CLI 차단 결과를 확인했다. SessionStart의 continue:false와 block reason은 거부권/모델 문맥으로 적용하지 않고 additionalContext만 저장했다. initialUserMessage는 큐로 전달하여 다시 UserPromptSubmit에서 차단·확인했으며, 호스트 재시작 후 동일 세션은 startup→resume 각 1회를 기록했다. MCP HTTP와 공식 Python SDK 1.26.0 stdio도 입력 차단/중단을 구조화 오류로 받았다. 이 경로는 실제 모델 생성 이전에 끝나며 아래 추론 실행과 구분한다.

Qwen3의 허용/거부 Write 호출 수는 소스 1/1, 설치본 1/1이다. call ID와 도구 결과, 허용된 파일 바이트와 차단된 파일 부재를 대조했다. 추가로 사용자 프롬프트에 없는 임의 문자열을 UserPromptSubmit의 추가 문맥으로만 제공하고 실제 Write의 파일 바이트가 일치하는지 확인했다. 모델 응답 뒤 HOST_STOP 중단도 각각 통과했다.

모델은 Qwen3 8B Q4_K_M, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 가중치 해시를 다시 확인했다. 컨텍스트 8192·temperature 0·최대 출력 1024(Stop은 64), enable_thinking=false, tool_grammar=false 조건이다. 무결성 검증을 포함한 API 시작은 소스 53.317초·설치본 53.611초이다. 이 결과로 다른 모델·플랫폼이나 장기 부하를 검증했다고 간주하지 않는다.

설치 prefix는 `build/input-lifecycle-stage`, 소비자는 `build/input-lifecycle-consumer/build`이다. 소스/설치 라이브러리 SHA-256 `079f1b245b9b5594861325c8643d6cde5d1284648243c4f46b5d65d71addb0fe`, Mach-O UUID `223D1222-A030-3DA9-A017-95167581F64E`가 일치한다. CLI·daemon·MCP 버전은 0.22.0이며 실제 설치 라이브러리 로딩과 얇은 CLI의 추론 라이브러리 비연결을 확인했다. daemon/MCP의 CMake 설치 RPATH 변경을 복사본에 적용하여 바이트를 대조하고 공개 헤더·문서·카탈로그·라이선스 및 내부 C 심볼 비공개를 검사했다.

최종 증거는 `build/input-lifecycle-verification.json`, `input-lifecycle-linkage.json`, 소스·설치 native JSON과 각 log/JUnit이다. 원격 커밋/푸시는 `input-lifecycle-publication.json`에 별도 기록한다. SessionEnd·clear·watchPaths·PermissionRequest/Denied, 나머지 훅 실행기·전체 설정 병합·앱/플랫폼 검증은 남아 있어 전체 목표는 partial이다. iPhone 제외 지시와 기존 사용자 daemon을 유지하며 이 SDK 단계에서 Society·Dreamscapes를 다시 패키징하지 않았다.

## 2026-09-15 C++ 외부 명령 훅 (0.21.0)

명시적 호스트 설정의 command 훅을 도구·모델·Stop·압축·Task·Subagent 콜백에 연결했다. JSON stdin, 입력 재검증, 호출 한 번의 권한 후보, 차단·중단, 병렬 실행·세션별 once, 취소·프로세스 정리·진단을 C++로 구현했다. daemon의 `--agent-hooks`, MCP의 `--hooks` 및 API·IPC CLI 조회/이벤트를 지원한다. 새 생산 의존성 없이 Qt Core와 기존 실행기를 재사용하며 Python은 검증에만 사용한다. [CommandHooks.md](CommandHooks.md)에 지원 범위와 참조 차이를 기록한다.

| 검증 경계 | 최종 관측 |
|---|---|
| Release 전체, inference 라벨 제외 | 51/51, 67.72초 |
| ASan·UBSan, llama 비활성 Debug | 49/49, 91.12초; 계측 오류 보고 없음 |
| 별도 설치 소비자 | 19/19, 14.62초 |
| 실제 Qwen3 8B | 소스·설치본 각각 허용 Write·차단 Write·Stop 중단 3개 시나리오 통과 |
| API·IPC CLI·MCP HTTP·공식 SDK stdio | 소스·설치본 통과; 입력 변경, 실제 파일 바이트/부재, 호스트 Deny/Ask 우선, 진단 전달, 인증·세션 격리 |
| 잘못된 호스트 설정 | 소스·설치본 각각 16개 모델 초기화 전 거부 |
| 설치 및 ABI | 공개 헤더 40개·문서·카탈로그·의존성 라이선스 일치; 실제 0.21 라이브러리 로딩 |

최종 검사는 직렬로 실행했다. 첫 Release 전체 실행은 50/51이며 기존 `limitsQueueAndPrivateState`의 150ms 요청이 model.waiting 상태까지 도달하지 못했다. 당시 sanitizer 빌드도 수행 중이었다. 두 작업의 인과를 분리 측정하지 않았으며 제품/테스트 기한은 바꾸지 않았다. 최초 로그 `command-hooks-release-tests.log`를 보존하고, 모든 빌드 종료 뒤 전체 Release·sanitizer·설치 검사를 통과했다. ASAN_OPTIONS=malloc_context_size=0, UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1로 C·C++ 계측을 유지한다.

C++ 명령 검사는 실제 셸 프로세스로 Unicode/셸 문자 stdin 전달, 입력 변경 뒤 실제 Write, 호스트 규칙 및 Plan 우선, 다음 호출의 권한 비복원, exit 2 차단·다른 오류 비차단, JSON 우선 처리, 실제 병렬 장벽, 세션별 once, 취소·시간/출력 제한과 자손 정리를 검증한다. Stop 차단의 두 번째 턴과 continue:false의 독립 중단 이유, 자식의 ID·transcript·Start/Stop 진단도 확인했다. 자식/반복 턴 단위 검사는 결정적인 C++ Model 대역이며 실제 모델 전체 위임 검증과 구분한다.

TDD에서 미매칭 훅도 큰 결과를 직렬화하던 오류와 UTF-8 변환 완료 전에 오류를 확인하던 결함을 재현했다. 매칭 항목을 먼저 고르고 QString 변환을 끝낸 뒤 판정하도록 수정했다. 끝이 잘린 UTF-8도 거부한다. `command-hooks-unmatched-red.log/xml`, `command-hooks-utf8-red.txt` 및 수정 후 로그를 보존했다. 초기 wire 테스트가 이벤트 키를 kind로 잘못 기대한 실패는 기존 event 키로 수정했다. 해당 로그도 남아 있다.

API는 훅 활성 여부와 CLI 조회 일치, 다른 앱 세션 404·잘못된 토큰 401·원격 hooks 주입 400을 확인했다. TaskCreated의 외부 명령이 게시를 거부하면 작업이 저장되지 않으며 TaskCompleted 입력도 확인한다. MCP는 실제 파일 경로 변경·Deny/Ask·exit 2 차단, progressToken 진단, 시작 후 설정 파일 변경의 비적용을 검사했다. 공식 Python MCP SDK 1.26.0 stdio에서도 파일 쓰기와 차단을 확인한다. MCP 직접 호출은 모델을 사용하지 않으며 실제 추론 결과는 다음과 같다.

소스의 Write 호출 수는 허용 1회·거부 1회이고, 설치본은 허용 1회·거부 1회이다. 각 도구 결과를 transcript의 call ID와 짝지었다. 허용은 파일 바이트 일치, 차단은 파일 부재와 HOOK_BLOCK 오류를 요구한다. 두 호스트에서 별도 실제 응답의 Stop 명령이 HOST_STOP을 반환하여 cancelled 상태로 끝나는 것도 확인했다.

모델은 Qwen3 8B Q4_K_M, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이며 이번에도 가중치 해시를 별도로 검증했다. 컨텍스트 8192·temperature 0·최대 출력 1024(Stop 시나리오 64), enable_thinking=false, tool_grammar=false이다. 무결성 검증을 유지한 API 시작은 소스 54.321초·설치본 53.510초다. 이 결과는 모든 모델의 지시 이행이나 초기화 성능을 입증하지 않는다.

설치 prefix는 `build/command-hooks-stage`, 소비자는 `build/command-hooks-consumer/build`이다. 라이브러리 SHA-256 `bbf550720d89e6f355e01987ef5f67b22acb067a68651f9664760bea7682dbf6`, Mach-O UUID `BB1C7703-3905-37AA-B3E2-EB8528C6EEAB`가 소스·설치본에서 일치한다. CLI·daemon·MCP 버전은 0.21.0이다. CLI는 바이트 일치, daemon/MCP는 CMake RPATH 설치 변경을 소스 복사본에 적용한 뒤 SHA 일치와 UUID를 확인했다. CLI의 추론 라이브러리 비연결, 의존성 C 심볼 비공개와 실제 설치 경로 로딩도 확인했다. 문서 최종본도 같은 prefix에 재설치하여 대조한다.

검증 기록은 `build/command-hooks-verification.json`, `command-hooks-linkage.json`, 소스·설치 native JSON과 개별 log/JUnit에 있고 원격 게시 결과는 `command-hooks-publication.json`에 기록한다. 공개 ABI는 0.21이므로 소비자는 헤더·라이브러리를 함께 갱신해 다시 빌드한다.

전체 생명주기, HTTP/prompt/agent/async 훅, updatedMCPToolOutput, 영속 once, 스킬·에이전트·플러그인 설정 병합, Windows/모바일 명령 및 Linux 실기기 검증은 남아 있다. hooks와 전체 목표는 partial이다. 이 단계는 SDK 구현/설치 소비자 검증이며 Society·Dreamscapes 재패키징이나 실기기 생성 완료를 뜻하지 않는다. iPhone 제외 지시와 기존 사용자 daemon을 유지한다.

## 2026-09-15 C++ 추가 작업 디렉터리 (0.20.0)

`permissions.additionalDirectories`와 daemon의 `--agent-add-dir`·MCP의 `--add-dir`을 C++ 파일 도구·권한 판정·자식 에이전트에 연결했다. 현재 경로와 출처·상태는 인증 API·IPC CLI·MCP에서 조회하고 모델의 각 턴에도 전달한다. 설정 삭제는 다음 호출에 반영한다. 도구 권한·호스트 비공개 경로·자식 도구 범위는 추가 디렉터리 안에도 적용한다. 새 생산 의존성은 없으며 Qt와 C++ 실행기를 재사용한다. Python은 검증 클라이언트다. 정확한 계약은 [WorkingDirectories.md](WorkingDirectories.md)를 따른다.

| 검증 경계 | 관측 결과 |
|---|---|
| Release 전체 검사, inference 라벨 제외 | 49/49, 87.22초 |
| ASan·UBSan, llama 비활성 Debug | 47/47, 86.45초; 런타임 오류 보고 없음 |
| 별도 설치 소비자 | 18/18, 12.04초 |
| 실제 Qwen3 8B의 추가 디렉터리 Write | 소스 2/2, 설치본 2/2; 각각 허용·철회 후 거부, 실제 Write 각 1회 |
| 인증 HTTP·IPC CLI·MCP HTTP·공식 SDK stdio | 소스·설치본 통과; 설정/CLI 경로·읽기·쓰기·검색·철회·세션 격리 확인 |
| 잘못된 호스트 설정 | 소스·설치본 각각 15개 시작 전 거부 |
| 대소문자 변형 비공개 경로 | Release·sanitizer·설치 소비자·MCP에서 거부 확인; 조건부 검사의 실행 여부 기록 |
| 설치·ABI·로더 | 공개 헤더 39개·문서·catalog·의존성 소스/라이선스 일치; 실제 0.20 라이브러리 로딩 |

전체 검사는 직렬로 실행했다. inference 라벨을 제외한 Release 결과를 모든 모델의 동작 정확도로 해석하지 않는다. ASan/UBSan은 C·C++ 계측을 유지하고 `ASAN_OPTIONS=malloc_context_size=0`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`로 실행한다. 이전 단계의 최초 Bash 파서 호출 warm-up은 그대로이며 100ms 제품 한도를 바꾸지 않았다. 최초 호출 성능이 해결됐다는 증거는 아니다.

C++ 검사는 출처별 디렉터리 합침·상대 경로 기준·명시적 홈·중복·미존재/일반 파일·한도·취소, 실제 Read/Write/Edit·Bash 리다이렉션·Glob/Grep, 현재 정책 철회와 호출자 경로 주입 거부를 포함한다. 비공개 저장소와 현재 세션 artifact의 읽기 전용 경계를 검사한다. 자식 에이전트는 현재 추가 경로를 모델 문맥에서 확인하고 실제 Write를 수행하며, 읽기 전용 프로파일 및 경로 철회 후에는 파일을 만들지 않는다. 이 자식 검사는 결정적인 C++ Model 대역을 사용한다. 추가 디렉터리에서 실제 모델을 사용하는 부모·자식 위임 전체를 검증한 결과는 아니다.

처음의 세 회귀는 기존 workspace 범위, 미지원 additionalDirectories, Glob 스키마 때문에 실패했다. 구현 후 별도 링크 교체 검사는 추가 디렉터리의 심볼릭 링크를 바꾸면 새 대상이 읽히는 문제를 재현했다. 같은 출처·설정 SHA·입력은 최초 canonical 대상에 묶고, 설정 바이트가 바뀔 때 다시 바인딩하도록 수정했다. 명시적 CLI 경로는 정책 객체의 수명 동안 바인딩한다. 이전 대상의 직접 경로 권한은 유지하고 새로운 링크 대상은 거부한다. 실행 callback에서 대상을 교체하는 경우도 거부한다. 바인딩 표는 현재 입력만 보관하고 성공한 snapshot의 검증이 끝나야 교체한다. 수정 전 `working-directories-red.log/xml`, `working-directories-binding-red.log/xml`과 수정 후·최종 검사를 보존한다.

같은 경로를 파일 설정과 CLI에서 지정한 뒤 파일 설정을 삭제하는 회귀도 처음에는 실패했다. 중복 입력을 일찍 생략하면서 CLI의 최초 바인딩까지 누락한 원인이었다. 조회 항목은 중복 제거하되 출처별 바인딩은 모두 보관하도록 수정했다. 파일 설정의 새 SHA가 다른 대상을 명시적으로 허용해도 CLI의 원래 대상은 유지하며, 파일 설정을 삭제하면 새 대상의 접근은 다시 거부한다. `working-directories-duplicate-binding-red.log/xml`과 수정 후 회귀·전체 검사를 보존한다. 이 수정 전에 통과한 검사·모델 결과·라이브러리는 `working-directories-initial-qualification/`에 따로 남기고, 수정된 생산 라이브러리로 전체 검증을 다시 수행했다.

현재 macOS 볼륨은 파일명의 대소문자를 구분하지 않는다. MCP의 인증 파일을 대문자 경로로 요청하는 회귀와 C++의 비공개 디렉터리 대문자 읽기·새 파일 쓰기가 모두 거부됐다. 해당 우회는 재현되지 않았고 이를 이유로 생산 코드를 변경하지 않았다. 최종 전체 검사에는 보강한 C++ 검사와 wire 결과의 실행 여부 기록이 포함된다. wire 보고서의 `private_case_variant_checked`가 true인지도 확인하여 조건부 검사가 생략되지 않았음을 기록한다.

실제 모델 검사는 원래 workspace 밖의 `shared/out/allowed.txt`에 예측 불가능한 요청 값을 정확히 쓰도록 한다. 그 다음 설정에서 shared의 권한 범위를 삭제하고 별도의 세션에서 `shared/out/blocked.txt` 쓰기를 요청한다. 경로용 Allow 규칙은 유지하지만 두 번째 실행은 `Path is outside the configured working directories`를 반환하고 파일이 없어야 통과한다. 소스·설치본 모두 두 실행이 completed이고 실제 Write 각 1회를 관측했다. CLI가 추가한 별도 경로는 파일 설정 철회 후에도 유지한다.

MCP HTTP·공식 Python MCP SDK 1.26.0 stdio는 실제 추가 경로 쓰기와 철회 후 거부를 검사한다. HTTP는 Read/Glob/Grep, 호스트 인증·설정·state 비공개, 추가 디렉터리의 `.claude/settings.json` 보호도 검사한다. CLI 경로만 사용한 새 MCP 호스트에서는 디스크 설정 출처가 비어 있고 ambient 프로젝트 Write 규칙이 활성화되지 않는다. API는 다른 앱의 대화 404·잘못된 토큰 401·원격 mode/경로 주입 400을 확인한다. MCP 파일 검사와 위 실제 Qwen3 추론은 별도 경계이다.

모델은 Qwen3 8B Q4_K_M, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 가중치 해시를 별도로 다시 확인했다. 컨텍스트 8192·최대 출력 1024토큰·temperature 0, `enable_thinking=false`, `tool_grammar=false`이다. 해시 검증을 유지한 daemon 시작 시간은 소스 53.292초, 설치본 53.454초이며 전체 모델/wire 검사는 각각 67.772초, 69.586초였다. 모델 초기화 성능은 계속 별도 개선 대상이다.

설치 prefix는 `build/working-directories-stage`, 소비자는 `build/working-directories-consumer/build`이다. 라이브러리 SHA-256 `9e3f833c362ab3cb23cb613a3d0fc5210216a37182b9cf3600e33d1660973954`, Mach-O UUID `5BF8F868-9909-3D01-8E37-F379496FC3DE`가 소스와 설치본에서 일치한다. CLI·daemon·MCP의 버전은 0.20.0이다. CLI 바이너리는 바이트 단위로 같고, daemon/MCP는 CMake 설치의 RPATH 변경을 별도 소스 복사본에 적용한 뒤 설치본 SHA와 대조한다. UUID도 같다. CLI가 추론 라이브러리를 링크하지 않고 의존성 C 심볼이 비공개임을 확인한다. 문서 최종 갱신도 같은 prefix에 설치하고 원본과 비교한다.

라이브러리 경로 환경변수를 제거하고 최종 검사 임시 데이터·앱 발견 경로를 build 아래로 격리했다. 명령·JUnit·모델 결과·설치 대조·변경 파일 해시는 `build/working-directories-verification.json`, 원격 게시 확인은 `working-directories-publication.json`에 남긴다. 공개 ABI는 0.20이므로 소비자는 헤더·라이브러리를 함께 갱신해 다시 빌드한다.

slash `/add-dir` UI, 원격 경로 변경·설정 쓰기, 추가 루트 instruction 자동 로딩, 전체 검색 의미·파일별 검색 권한, Bash cwd 변경·OS 샌드박스와 나머지 하네스는 남아 있다. 모든 OS 경로 별칭·Unicode·파일 열기 경쟁 조건을 차단하거나 일반 프로그램의 부작용을 격리하는 기능은 아니다. settings/permissions/files/subagents는 계속 **partial**이다. 이 단계는 SDK 검증이며 Society·Dreamscapes와 물리 디바이스 앱을 변경하지 않았다. iPhone 제외 지시를 유지하고 전체 목표는 진행 중이다.

## 2026-09-15 C++ 계층형 권한 설정 (0.19.0)

사용자·프로젝트·로컬·명시적 호스트·관리자 파일의 권한을 C++ `SettingsPermissionPolicy`에 연결했다. 출처별 경로 기준, 배열 합침·scalar 우선순위, managed-only 필터, bypass 금지와 파일 변경·삭제의 다음 호출 반영을 구현했다. 인증된 API·IPC·CLI와 MCP에서 현재 정책·출처·SHA를 조회한다. 설정 파일 자체의 쓰기 보호, 지원하지 않는 권한 필드의 실행 거부, 잘못된 호스트 설정의 모델 초기화 전 거부를 포함한다. 자세한 계약·참조와 차이는 [PermissionSettings.md](PermissionSettings.md)에 기록한다.

| 검증 경계 | 최종 결과 |
|---|---|
| Release 전체 검사, inference 라벨 제외 | 47/47, 62.61초 |
| AddressSanitizer·UndefinedBehaviorSanitizer, llama 비활성 | 45/45, 72.03초; 런타임 오류 보고 없음 |
| 별도 설치 소비자 | 17/17, 6.54초 |
| 독립 node-ignore 7.0.5 기대값과 파일 규칙 대조 | 50개 사례, C++ 검사 통과 |
| Qwen3 8B의 실제 허용·거부 Write | 소스 2/2, 설치본 2/2; 각 실행에서 Write 1회 |
| 인증 HTTP·IPC CLI·MCP HTTP·공식 SDK stdio | 소스·설치본 모두 통과; 실제 쓰기·변경 후 거부·세션 격리 확인 |
| 잘못된 호스트 설정 | 소스·설치본 각각 15개 시작 전 거부 |
| 설치·ABI·로더 | 공개 헤더 39개·문서·catalog·의존성 소스/라이선스 일치, 실제 0.19 라이브러리 로딩 |

Release는 inference 라벨을 제외한 검사 범위이며 모든 모델의 지시 이행 정확도를 검증한 결과가 아니다. Sanitizer는 C·C++에 address/undefined 계측을 적용하고 `ASAN_OPTIONS=malloc_context_size=0`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`로 실행했다. 할당·해제 스택 이력은 껐으며 계측은 유지한다. 마지막 전체 검사는 직렬로 실행했다.

C++ 회귀 검사는 출처 필터·관리 조각의 정렬·managed-only의 첫 판단·모드 복원·bypass 금지, 경로 기준·basename·부정·부모 제외·중복·문자 범위·Unicode를 포함한다. 링크·비정규 파일·읽기 실패·크기 한도·과도한 패턴·취소는 실행 실패로 검사하고 Deny를 버리지 않는다. API의 다른 앱 세션은 404, 잘못된 토큰은 401, 원격 mode 주입은 400으로 거부한다. 일반 env 설정은 적용하거나 값을 노출하지 않는다. 독립 기대값은 고정한 node-ignore 파일의 URL·SHA와 함께 `tests/permission_settings_patterns.json`에 있다. 이 버전이 참조 미러의 실제 배포 의존성과 같다는 뜻은 아니다.

실제 Qwen3 8B 검사는 새 세션 두 개에서 모델이 생성한 Write와 결과를 확인한다. 허용 실행은 실제 파일 바이트가 요청과 일치하고, 거부 실행은 permission denied 결과와 파일 부재를 확인해야 통과한다. 소스·설치본 각각 허용 Write 1회·거부 Write 1회를 관측했고 두 실행 모두 완료했다. MCP HTTP와 공식 Python MCP SDK 1.26.0 stdio에서도 실제 파일을 쓴 뒤 관리 설정을 바꿔 다음 쓰기의 거부를 확인했다. MCP HTTP에서는 범위 이탈·민감 파일·설정 쓰기 거부와 다른 연결의 세션 접근 거부도 검사했다. MCP 검사 자체는 모델을 필요로 하지 않으며 위 실제 모델 검사와 별도 경계다.

모델은 Qwen3 8B Q4_K_M, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 컨텍스트 8192·최대 출력 1024토큰·temperature 0, `enable_thinking=false`, `tool_grammar=false`로 실행했다. 이번에도 가중치 해시를 별도로 재확인했다. 첫 소스 호스트는 검사의 90초 시작 한도를 넘겼다. 프로세스 샘플에서는 가중치 파일 읽기와 SHA-256 검증이 관측되었다. 제품의 무결성 검증은 유지하고 모델을 사용하는 검사의 시작 한도를 300초로 지정했다. 최종 API 시작 시간은 소스 100.904초, 설치본 53.248초이며 빠른 시작을 검증한 결과로 해석하지 않는다. 최초 로그와 `permission-settings-model-startup.sample.txt`를 보존한다.

초기 테스트 작성 중 새 API 검사가 임시 JSON 객체의 참조를 보관해 충돌했고 `.value()` 복사로 수정했다. MCP SSE 프레임 처리와 기존 도구 개수·첫 항목 가정도 새 조회 도구에 맞게 수정했다. 문자 집합의 선행 `!`를 처리하는 C 매처와 node-ignore의 차이는 독립 사례로 재현해 어댑터를 고쳤다. 과도한 패턴 검사도 실제 재귀 깊이 한도에 도달하는 입력으로 바로잡았다. 원래 실패와 최종 성공은 별도로 보존한다.

중간 sanitizer 검사에서는 기존 150ms API 기한 사례가 대기 상태까지 도달하지 못했다. sanitizer 오류 보고는 없었고 최종 직렬 전체 검사에서는 통과했다. 이후 Release 전체 검사 한 번에서는 기존 Bash 허용 명령 검사가 Allow 대신 Ask를 반환했고 단독 대조에서는 통과했다. 진단을 추가한 다음 sanitizer 검사에서는 첫 `git status` 판정이 121ms 뒤 Ask를 반환했다. 제품 파서의 전체 구문 분석 기한은 100ms이다. 초기 페이지 로딩·계측 등 각각의 기여는 분리 측정하지 않았다. 파서 한도와 판정 동작은 유지하고 테스트에 최초 파서 호출을 한 번 추가하여 시간·결과를 기록한다. 최초 호출은 Allow 또는 보수적인 Ask를 허용하며 이후의 기존 문법 검사는 계속 Allow를 요구한다. 마지막 통과는 이 초기화 뒤의 문법 검사 결과이며 최초 호출의 성능 문제가 해결됐다는 뜻은 아니다. 로그는 `permission-settings-sanitizer-tests-first.log`, `permission-settings-release-tests-bash-intermittent.log`, `permission-settings-sanitizer-tests-bash-cold.log`에 남긴다.

설치 prefix는 `build/permission-settings-stage`, 소비자는 `build/permission-settings-consumer/build`이다. 라이브러리 SHA-256 `6ccd13980e38d11a4ac3a244afd9d3d43807c4888f7adb19884d34efaa411aa3`, Mach-O UUID `B7A53712-3595-3D3A-BB69-A5E4EE39AF2F`가 소스와 설치본에서 일치한다. CLI·데몬·MCP의 버전은 0.19.0이다. CLI 바이너리는 바이트 단위로 같고, 데몬·MCP는 CMake 설치 시 build RPATH가 `@loader_path/../lib`로 바뀌므로 원본 해시는 설치본과 다르다. 소스 실행 파일의 별도 복사본에 같은 설치 경로 변환을 적용한 뒤 설치본 SHA와 일치함을 확인했고 UUID도 같다. CLI는 추론 라이브러리를 링크하지 않는다. tree-sitter와 wildmatch C 심볼은 비공개이고 수정한 wildmatch 소스·원본 라이선스·출처·수정 설명을 설치 패키지에 포함했다. 정확한 의존성 조건은 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)를 따른다.

라이브러리 경로 환경변수 네 개를 제거하고 임시 데이터·앱 발견 경로를 build 아래로 격리했다. 명령·JUnit·모델 결과·설치 대조·초기 실패와 변경 파일 해시는 `build/permission-settings-verification.json` 및 `permission-settings-*`에 있다. ABI는 0.19이므로 헤더와 라이브러리를 함께 갱신하여 소비자를 다시 빌드해야 한다. 이번 단계는 SDK 검증이며 Society·Dreamscapes나 물리 디바이스 앱을 변경하지 않았다. iPhone 제외 지시를 유지한다.

생산 권한 실행기는 C++이고 패턴 매칭은 제한을 추가한 C 의존성을 사용한다. Python은 검증 클라이언트다. 추가 디렉터리·원격 관리/MDM·Windows 레지스트리·자동 분류·원격 승인·전체 BashSecurity·OS 샌드박스와 나머지 하네스 기능은 남아 있다. settings와 permissions는 **partial**이며 전체 목표는 계속 진행 중이다.

## 2026-09-15 호출 범위 스킬 권한과 실행 스냅샷 (0.18.0)

스킬의 `allowed-tools`를 현재 호출의 권한에만 합치고, 호스트의 Deny·Ask·Plan 및 자식 도구 범위를 유지한다. 모델의 Skill 호출은 권한 판정 전에 본문·출처·SHA·요청 권한을 한 번 고정하고, 성공한 도구 결과가 저장된 뒤 권한을 활성화한다. 파일 도구도 승인한 canonical 대상을 실행 직전에 재검증한다. 정확한 문법·우선순위·미지원 범위는 [Permissions.md](Permissions.md)에 기록한다.

| 검증 경계 | 최종 결과 |
|---|---|
| Release 전체 검사, inference 라벨 제외 | 45/45, 79.73초 |
| AddressSanitizer·UndefinedBehaviorSanitizer, llama 비활성 | 43/43, 80.24초; 런타임 오류 보고 없음 |
| 별도 설치 소비자의 공개 헤더·라이브러리 사용 | 16/16, 8.91초 |
| Qwen3 8B: HTTP 직접·IPC CLI 직접·모델 Skill, inline/fork | 소스 6/6, 설치본 6/6; 실제 파일 바이트와 다음 호출의 거부 확인 |
| Qwen3 8B: 공식 MCP SDK 1.26.0, stdio·HTTP, inline/fork | 소스 4/4, 설치본 4/4; 각 전송에서 실제 파일 두 개 확인 |
| 설치·ABI·로더 | 공개 헤더 38개·문서·catalog·두 MIT 라이선스 일치; 실제 0.18 라이브러리 로딩 |
| 배포 실행 파일·파서 경계 | CLI·데몬·MCP 모두 0.18.0; 얇은 CLI 링크 유지, tree-sitter C 심볼 비공개 |

추론을 제외한 Release 전체 검사는 모든 모델의 정확도 검사가 아니다. sanitizer는 C·C++에 address/undefined 계측을 적용하고 `ASAN_OPTIONS=malloc_context_size=0`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`로 실행했다. 초기 실패, 중간 수정 및 최종 전체 직렬 실행은 별도 로그로 보존한다.

C++ 회귀 검사는 규칙 파싱·상한·취소, 호스트 Deny/Ask 우선순위, 파일·Skill·Agent·MCP 서버 규칙, 복합 Bash의 모든 명령, 중첩 거부·리다이렉션·심볼릭 링크, 입력 변경 훅 뒤 스냅샷, 권한 응답 또는 ToolStarted 중 파일 경로 교체를 포함한다. 스킬 권한은 같은 도구 묶음의 후속 호출과 현재 실행의 자동 압축 이후에 유지되고, 새 실행·세션 분기·재시작·기록 복구에서 복원되지 않는다. 큐의 새 사용자 prompt는 스킬 추가 권한을 지우고 notification은 유지한다. 자식 fork의 전용 권한, 일반 위임·백그라운드 수락 스냅샷, 재개 시 현재 호출자의 권한 및 사전 로딩 스킬의 권한 비활성도 검사했다.

추가 검토에서 `'git status'`라는 단일 실행 파일을 git/status 두 단어처럼 판정하는 오류, `<>` 오류 노드와 후행 인자가 포함된 리다이렉션에서 파일 거부 규칙을 놓치는 오류를 재현했다. 전자는 공백 실행 파일명의 자동 접두사 허용을 막고, 후자는 파서가 분리하지 못한 리다이렉션에 보수적인 Deny/Ask를 적용해 수정했다. 고정한 tree-sitter-bash 문법이 `<>`를 지원한다고 주장하지 않는다. 수정 전 `permission-shell-boundaries-red.log`·`permission-redirect-target-red.log`와 수정 후 검사, 최종 전체 검사가 남아 있다.

API 검사는 호스트가 Skill(writer)를 허용한 상태에서 스킬이 요청한 `Write(grant-*.txt)`만으로 임의 값을 실제 파일에 쓰도록 한다. 각 inline/fork 및 HTTP/CLI/모델 경로의 Write는 1회였으며, 같은 부모의 다음 일반 호출은 Write를 거부하고 파일을 만들지 않았다. 외부 allowed_tools 입력은 400, 다른 앱 토큰 접근은 404로 거부했다. 소스·설치 데몬 모두 종료 코드 0이며, 기존 프로파일 읽기·백그라운드 재개 검사도 함께 통과했다.

MCP는 stdio·HTTP 각각 인라인·fork 쓰기, 직접 MCP Write의 호출 밖 거부, 원격 권한 필드 거부를 검사했다. 기존 임의 값 Read, 자식 프로파일의 읽기·재개, 스킬 fork의 부모 이력 격리도 함께 검사했다. 모든 Write의 경로·내용과 성공 결과를 대조하고 실제 파일 바이트도 일치해야 통과한다. 관측한 Write 수는 source stdio: 2회, source http: 2회, installed stdio: 2회, installed http: 2회이다. 한 번만 실행을 보장하는 기능을 추가한 것은 아니다.

Qwen2.5 0.5B에서는 중복 Write, 쓰기 없이 완료 응답, 요청과 다른 파일 바이트가 관측되어 정확한 쓰기 검증을 통과한 모델로 처리하지 않는다. 최초의 단일 호출 가정은 중복 Write에서 실패했고, 모든 호출·결과를 검사하면서 실행 횟수를 기록하도록 fixture를 고쳤다. MCP가 반환한 절대·상대 경로도 canonical 대상 기준으로 대조한다. 원래 실패는 `skill-permissions-source-mcp-stdio-native-first-pass.log`, `*-no-write.log`, `*-05b.log` 등에 보존한다.

8B의 기본 MCP 모델 로딩 설정에서도 구조화 응답이 출력 한도에 도달한 실패가 있었다. `iillm-mcp --model-options FILE`을 추가하여 API에서 검증한 `enable_thinking=false`, `tool_grammar=false`를 같은 호스트 설정으로 전달했다. 비공개 JSON을 모델 초기화 전에 검증하고 모델 로딩 완료 후 요청을 처리한다. 옵션 부재·잘못된 JSON/권한/위치/심볼릭 링크는 CLI 회귀 검사에 포함한다. 최종 8B 모델은 Q4_K_M, 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이며 컨텍스트 8192·temperature 0을 사용한다. API 스킬 쓰기 한도는 1024토큰, MCP는 2048토큰이며 일반 프로파일의 자식 생성 설정은 2048토큰이다. 모든 스킬·모델·입력에 대한 일반적인 지시 이행 정확도를 보장하지 않는다.

설치 prefix는 `build/skill-permissions-stage`, 소비자는 `build/skill-permissions-consumer/build`이다. 최종 라이브러리 SHA-256 `9e824127ededcdb3544671a5a977212953a768115f0d6ad23a80e424e2a9620c`, Mach-O UUID `3EE8390D-B4B3-34E3-8968-0D1A8C452322`가 소스 빌드와 설치본에서 일치한다. 실행 파일별 해시, 실제 loader 경로, 버전·링크·설치 문서 대조는 `build/skill-permissions-linkage.json`에 있다. 라이브러리 경로 환경변수 네 개를 제거하고 임시 데이터·앱 발견 경로를 build 아래로 격리했다. 명령·JUnit·모델 결과·실패 기록과 소스 파일 해시는 `build/skill-permissions-verification.json` 및 `skill-permissions-*`에 있다. `skill-permissions-prefinal/`의 중간 성공은 최종 결과와 구분한다.

새 권한 실행기는 C++이고 Bash AST는 버전·SHA를 고정한 MIT C 의존성을 사용한다. Python은 이번 검증의 외부 클라이언트 역할이다. ABI는 0.18이므로 새 공개 헤더와 라이브러리로 소비자를 다시 빌드해야 한다. 관리/사용자/프로젝트 권한 계층, 전체 BashSecurity·별칭 정규화·자동 분류·OS 샌드박스·원격 권한 중개와 나머지 하네스 기능은 계속 미완료이다. skills/subagents/permissions는 **partial**이며 전체 목표는 진행 중이다. 이번 단계는 SDK 검증이며 Society/Dreamscapes 재설치나 물리 디바이스 UI 검증을 포함하지 않는다. iPhone 제외 지시를 유지한다.

## 2026-09-15 C++ 스킬의 별도 자식 실행 (0.17.0)

`context: fork` 스킬을 사용자 직접 호출과 모델의 `Skill` 도구 호출에 연결했다. 선택한 에이전트 프로파일·호스트 모델 별칭을 사용하고, 부모 세션에서 한 번 치환한 본문을 부모 이력 없이 자식 대화에 저장한다. 직접 호출은 자식의 결과·상태·사용량을 부모 호출 ID로 반환한다. 모델 호출은 자식 결과를 도구 관측으로 받고 후속 턴을 진행한다. 부모 권한, 자식 도구 범위, 취소·기한·턴 한도와 자식 훅을 유지한다. 세부 계약과 참조 차이는 [Skills.md](Skills.md)에 기록한다.

| 검증 경계 | 결과 |
|---|---|
| Release 전체 검사, inference 라벨 제외 | 직렬 42/42, 80.94초 |
| AddressSanitizer·UndefinedBehaviorSanitizer, llama 비활성 | 40/40, 81.10초; 런타임 오류 보고 없음 |
| 새 설치 소비자, 별도 build 및 설치 헤더·라이브러리 | 14/14, 7.30초 |
| Qwen3 8B: HTTP 직접·IPC CLI 직접·모델 Skill 도구 | 소스 3/3, 설치본 3/3 |
| Qwen2.5 0.5B: 공식 MCP SDK 1.26.0 stdio·HTTP | 소스 2/2, 설치본 2/2 |
| 설치 파일과 실제 로딩 경로 | 공개 헤더 37개·문서·catalog 일치, 0.17 라이브러리 로딩 확인 |
| CLI 의존성·버전 | iillm에 iiLocalLLM/llama/ggml 링크 없음; CLI·데몬·MCP 실행기 모두 0.17.0 |

C++ 회귀 검사는 부모 모델 호출 생략·이력 격리, 부모 ID로 인자 치환, 본문 출처 저장, 프로파일의 background 지정에도 동기 반환, 결과·사용량 전달, 도구 범위·부모 Deny 유지, 에이전트 fallback·허가되지 않은 모델 거부, 직접 호출 중 큐 입력 보존, SubagentStop의 추가 턴 요구, 취소·기한·실패·턴 한도, 관찰자 실패와 재시작 후 자동 재실행 방지, fork 실행기 없는 호스트의 명시적 미지원 처리를 검증한다. API는 앱 인증 경계, MCP는 연결별 대화 경계와 같은 스킬 실행 경로를 검사한다.

8B 검증은 매 경로마다 새로운 예측 불가능한 파일 값을 생성하고 자식의 실제 `Read` 호출·도구 결과·최종 답변을 대조했다. 각 경로에서 자식 Read 1회가 관측되었다. 부모 기록에 스킬 본문을 자동 주입하지 않았고, 직접 호출에는 부모 도구 호출이 없으며 모델 경로에는 Skill 1회가 있었다. 자식 파일 SHA-256·별칭 모델·부모 ID 치환을 확인하고 다른 앱 토큰의 접근은 404로 거부했다. 같은 실행에서 기존 파일 프로파일의 직접 실행·백그라운드 재개·프롬프트 고정도 통과했다. 두 데몬은 종료 코드 0이다.

0.5B MCP 검증도 임의 파일 값을 자식에서 실제로 읽고 최종 응답과 대조한다. 부모에는 호출·응답 두 메시지만 추가되며, 자식에는 이전 부모·다른 자식의 관측 값이 포함되지 않는 것을 검사했다. 각 전송에서 기존 인라인 스킬·일반 위임·백그라운드 재개 검사도 함께 통과했다. 이 기록은 로컬 테스트 호스트의 API/MCP 호환성 증거이며 실제 Society·Dreamscapes UI나 물리 디바이스 검증을 포함하지 않는다.

테스트를 먼저 추가했을 때 `SkillInfo`의 executionContext/agent/model 필드 부재로 컴파일이 실패한 기록은 `build/skill-fork-red.log`에 있다. 첫 8B HTTP 검사는 테스트 코드가 세션 생성에 `system_prompt`를 전송하여 `invalid_argument`로 거부되었다. API의 필드 `system`으로 수정한 후 최종 검사를 통과했다. 최초 로그·JSON·데몬 로그는 `build/skill-fork-*-initial.*`에 보존한다.

설치 경로는 `build/skill-fork-stage`, 소비자는 `build/skill-fork-consumer/build`이다. 라이브러리 SHA-256은 `b49862f794440b0998ee4f7774f840e80682b6ed83672af16be5a749e42bfc20`, Mach-O UUID는 `F3F44868-B10A-32BE-B603-3D8F7F017A51`로 소스 빌드와 설치본이 일치한다. 검사는 `DYLD_LIBRARY_PATH`·`DYLD_FRAMEWORK_PATH`·`DYLD_FALLBACK_LIBRARY_PATH`·`LIBRARY_PATH`를 제거한 환경에서 수행했다. 전체 실행 명령·결과와 아티팩트 경로는 `build/skill-fork-verification.json`, 개별 JUnit·모델 보고서는 `build/skill-fork-*`에 있다. 이 파일들은 로컬 검증 산출물이다.

생산 실행기는 C++이며 신규 Python 런타임 의존성을 추가하지 않았다. ABI는 0.17이므로 소비자는 공개 헤더와 라이브러리를 함께 갱신하고 다시 빌드해야 한다. 스킬의 allowed-tools 권한 추가, effort, KAIROS 예약 fork, 외부 훅, 플러그인·원격 스킬, 전체 하네스의 나머지 기능은 계속 미완료이며 skills/subagents 상태는 **partial**이다.

## 2026-09-15 파일 기반 에이전트 프로파일과 자식 훅 (0.16.0)

관리·실행 시 JSON/C++·프로젝트·사용자·플러그인 디렉터리·내장 정의의 프로파일 계층을 C++ 실행기에 연결했다. `Subagents::attach`는 현재 프로파일을 각 모델 턴에 반영하며, 자식 기록에는 선택한 프롬프트와 출처·파일 SHA-256을 고정한다. 호스트 모델 별칭/허용 목록, 재개 시 범위 교집합, 초기 프롬프트, 백그라운드 지정, 스킬 사전 로딩과 SubagentStart/SubagentStop 콜백을 제공한다. `agent.agents.profiles`, MCP `iiLocalLLM.agent.agents.profiles`, CLI `agent agents profiles SESSION`이 같은 구현을 사용한다. 세부 계약과 참조 차이는 [AgentProfiles.md](AgentProfiles.md)에 있다. 전체 하네스·서브에이전트·훅 영역은 **partial**이다.

| 검증 경계 | 결과 |
|---|---|
| Release 전체 검사, inference 라벨 제외 | 최종 직렬 41/41, 57.61초 |
| AddressSanitizer, llama 비활성 | 39/39, 76.97초 |
| 새 설치 소비자, inference 이름 제외 | 17/17, 25.00초 |
| C++ Qwen3 8B 위임·부모 분기·백그라운드·재개 | 소스 4/4, 설치본 4/4 |
| 실제 파일 프로파일의 Start/Stop 훅 | 소스·설치본 각각 Start 4회, Stop 4회 |
| 8B 실제 데몬 프로파일 실행·CLI 재개 | 소스 최종 CTest 1/1, 62.32초; 설치본 1/1, 59.01초 |
| 공식 Python MCP의 0.5B 프로파일 실행·재개 | 소스 stdio/HTTP 2/2, 설치본 stdio/HTTP 2/2 |
| 설치 데몬 HTTP·native IPC·CLI, 추론 없는 제어 검사 | 통과 |

단위 검사는 우선순위·프로젝트 상위 탐색·현재 파일 갱신·심볼릭 링크 범위·취소/용량 제한·미지원 필드·카탈로그 비공개 필드 제외·호스트 설정 검증을 포함한다. 실제 C++ 실행에서는 새 프로파일의 다음 턴 인식, 원래 프롬프트 유지, 현재 도구 제한과의 교집합, 파일이 지정한 임의 모델의 거부, 별칭 선택, 스킬의 실제 자식 세션 ID 치환, 사전 로딩 실패 시 저장소 정리와 background 지정도 확인했다. 추가 스킬 권한이나 미지원 설정을 조용히 허용하지 않는다.

Start 훅 안에서 자식 목록을 조회해 소유자 잠금과의 교착이 없음을 확인했다. Start 차단은 기록된 실패가 되고 모델을 실행하지 않는다. 자식 Stop은 부모 Stop과 구분되며 차단 피드백으로 다음 턴을 실행한다. `stop_hook_active`, 부모의 Deny/Ask 유지, Plan 제한을 검사했다. 마지막 Plan fixture는 부모 Bypass에 명시적 Deny가 없는 조건으로 보강하여 Release와 ASAN에서 각각 다시 빌드·검사했다. 이 변경과 공개 헤더 설명 보완은 라이브러리 해시를 바꾸지 않았다.

프로파일 디렉터리가 실행 중 범위를 벗어나는 링크로 교체되면 `AgentProfiles`는 오류를 반환한다. 이 오류 때문에 기존 자식의 MCP 조회·중단·연결 종료까지 실패하던 경계를 회귀 검사 후 수정했다. 초기 Release 전체 검사에서는 기존 전송 시험의 100ms HTTP 제한 안에 세션 생성이 끝나지 않아 40/41이었다. 같은 전송 검사의 단독 재검사와 이후 전체 직렬 검사는 통과했다. 최초 실패를 `agent-profiles-release-tests.log/xml`에 보존했으며 성공 결과로 덮어쓰지 않았다.

초기 TDD 빌드는 미구현 헤더/attach/훅 enum으로 실패했다. 스킬 세션 ID 치환 실패, MCP 설정 오류로 인한 제어 경로 실패도 수정 전 로그를 유지한다. Qt moc가 테스트의 URL을 포함한 raw 문자열을 잘못 읽는 문제는 일반 이스케이프 문자열로 고쳤다. MCP fixture에 두 번째 자식을 추가한 뒤 UUID 정렬의 첫 항목을 가정하던 검사는 반환된 실제 ID를 선택하도록 수정했다. 이들은 제품 동작 실패와 테스트 fixture 결함을 구분해 `build/agent-profiles-*red*`, `*green*`에 기록했다.

8B C++ 검사는 파일 프로파일과 사전 로딩한 스킬을 사용한다. 부모의 실제 Agent 호출, 자식의 실제 Read, 임의 생성 파일 값, 짝이 맞는 도구 호출/결과, 분기된 부모 문맥, 백그라운드 알림과 재개를 확인했다. Qwen3 8B Q4_K_M의 가중치는 5,027,783,488바이트, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. C++ 구성은 context 8192, 캐시 문맥 1개, temperature 0, 최대 생성 2048토큰, thinking/tool grammar false이다.

새 데몬 검사는 동일한 8B를 호스트 모델 별칭으로 선택하고 HTTP로 첫 실행, 얇은 IPC CLI로 백그라운드 재개, HTTP로 최종 결과를 조회한다. 각 실행의 실제 Read 두 번과 서로 다른 파일 값, 앱별 인증 격리, 프로파일 갱신 뒤 기존 시스템 프롬프트·SHA 유지, 한 번만 로딩된 스킬과 자식 ID를 저장된 JSONL/작업 기록에서 대조했다. 소스·설치본 모두 정상 종료 코드 0을 확인했다. 첫 소스 시도는 새 Python probe가 필수 RPC id를 빠뜨려 세션 생성 전 실패했으며 `agent-profiles-source-api-native.*`에 남겨 두었다. 수정된 fixture의 최종 등록 CTest 결과는 `agent-profiles-api-wire-final.*`, `agent-profiles-api-inference.*`이다. 8B의 이 성공은 이전 0.15의 0.5B API 모델 실패를 재검증하거나 지우는 증거가 아니다.

공식 MCP 1.26.0의 stdio·HTTP 검사는 서버 기동 후 reader 파일을 생성하고 카탈로그의 갱신·본문 제외를 확인한다. 해당 reader로 실제 Read와 백그라운드 재개, 변경된 파일 값, 두 개 이상의 Read 결과, 알림과 대화 정합성을 검사한다. 가중치는 Qwen2.5 0.5B Q4_K_M, 491,400,032바이트, SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`이다. 검증 프로세스는 Python을 외부 클라이언트로 사용하며 제품의 프로파일·훅 실행기는 C++이다.

설치 prefix는 `build/agent-profiles-stage`, 새 소비자는 `build/agent-profiles-consumer/build`이다. 라이브러리 SHA-256은 `2bc772c145572a147ff3e16874d69c83605f165cdfd5d9258c01375d6435857e`, UUID는 `40F442C1-66E2-36B1-B78C-AA4FAD61857E`이다. 소스 실제 8B 검증 이후와 최종 설치본의 라이브러리가 동일하며, 공개 헤더 37개·설치 문서·카탈로그를 원본과 대조한다. 실제 loader 경로가 이 설치본을 가리키고 `iillm`이 libiiLocalLLM/llama/ggml을 링크하지 않음을 확인한다. ABI는 0.16이며 소비자를 다시 빌드했다.

명령·JUnit·모델 흐름·실패·설치 동일성은 `build/agent-profiles-verification.json`, `agent-profiles-linkage.json`, `agent-profiles-steps.json`과 개별 로그에 있다. 스킬 `context: fork`, 전체 전문 역할·조건부 선택, 플러그인 생명주기, 에이전트별 MCP/메모리·외부 훅, 팀·mailbox·worktree·remote 격리와 실제 Society/Dreamscapes 제품 UI 검증은 남아 있다. 이 단계에서 기본 SDK나 제품 앱을 재설치하지 않았으며 iPhone 제외 지시를 유지한다. 전체 목표는 진행 중이다.

## 2026-09-15 C++ 서브에이전트와 호출 경계 (0.15.0)

기존 Engine 위에 별도 자식 대화 실행, 도구·모델 범위 제한, 동기·백그라운드 실행, 명시적 부모 컨텍스트 분기, 자식 재개, 취소·기한, 결과 조회와 완료 알림을 추가했다. 인증 API·native IPC·HTTP·CLI·MCP에서 같은 실행기를 사용한다. 추가 생산 의존성과 Python 실행기는 없으며 기존 Qt·C++ Model/Service·도구·세션·입력 큐를 재사용한다. EngineOptions·ToolContext·ApiOptions 확장에 따라 ABI는 0.15이고 소비자를 다시 빌드한다. 전체 하네스와 subagents의 상태는 **partial**이다.

| 검증 | 관측 결과 |
|---|---|
| Release 전체 빌드·inference 라벨 제외 CTest | **40/40 통과**, 73.85초 |
| ASan·UBSan, llama 비활성 Debug | **38/38 통과**, 81.08초. `ASAN_OPTIONS=malloc_context_size=0` |
| 새 설치 소비자, 이름 끝의 inference 제외 | **17/17 통과**, 33.07초 |
| 소스 Qwen3 8B 실제 추론 | **1/1 통과**, 102.31초. 부모 위임·분기·백그라운드·재개 네 흐름 |
| 설치본 Qwen3 8B 실제 추론 | 독립 소비자 **종료 코드 0**, 87.21초. 같은 네 흐름 |
| 공식 MCP stdio·HTTP, Qwen2.5 0.5B | 소스 **2/2**, 설치본 **2/2 통과**. 자식의 실제 Read·변경된 파일 재조회·완료 알림 확인 |
| 설치 API·CLI·공식 MCP, 추론 없는 세 경로 | **3/3 통과** |
| 설치 API·CLI, Qwen2.5 0.5B 자식 추론 | **0/1 통과**. 자식 구조화 응답이 512토큰 한도에서 미완료, `protocol_error`, 실제 도구 호출 0회 |
| 설치 파일·ABI·로더 | 공개 헤더 36개, 문서·카탈로그, 소스/설치 라이브러리 SHA-256·UUID 일치. 실제 stage 라이브러리 로딩과 얇은 CLI 링크 확인 |

설치 통신 검사는 합계 **5/6**이다. Release 표는 전체 추론 검사의 통과를 뜻하지 않는다. Sanitizer는 할당·해제 스택 이력을 끈 조건이며 ASan·UBSan 계측은 유지한다. 마지막에 실제 Bash 정리 검사에 데스크톱 POSIX 조건을 명시한 뒤 Release·Sanitizer의 해당 검사도 각각 1/1 통과했다. macOS에서 실행하는 내용은 같고 생산 코드와 라이브러리는 바뀌지 않았다. Windows·모바일 실행을 검증한 것은 아니다.

서브에이전트 단위 검사는 13개 동작 사례이며 Qt 초기화·정리를 포함해 15 passed이다. 부모/자식 Engine 경로, 별도 대화와 권한 정책, 지연 MCP 도구 검색 범위, 생성되는 Task 도구 차단, 부모 Bypass에서도 금지 도구 실행 거부, 재개 시 도구 범위 확장 금지, 부모 미완료 호출을 짝지은 컨텍스트 분기, 기한·취소·동시 실행 한도·완료 알림·저장소 소유권을 검사했다. 자식이 시작한 실제 background Bash의 종료도 확인했다. 비정상 종료 복구는 종료된 저장소의 running 레코드를 구성해 재시작한 시험이며 실제 프로세스 강제 종료 실험과 구분한다.

회귀 네 개는 수정 전에 실패했다. 잘못된 max_turns에서 남던 자식 대화, 분기 지침을 합친 입력 길이 검증 누락, 재개 수락 저장 실패 뒤 메모리 결과 변경, 셸 조회와 중단 사이의 자연 종료 처리이다. 입력 검증을 대화 저장보다 앞에 두고, 저장 성공 뒤 수락 상태를 교체하며, 셸 종료를 다시 확인하도록 수정했다. 첫 셸 경합 fixture는 모델 컨텍스트 구성에서 상태를 소진하므로 모델 응답 뒤 경합을 시작하도록 바로잡았고, 수정 전 실패를 다시 확인했다. 결과는 `build/subagents-boundaries-red*.log/xml`, `subagents-boundaries-green.*`에 보존한다. 초기에 공개 헤더가 없어 실패한 TDD 빌드도 유지한다.

API 검사는 인증된 두 클라이언트의 자식 ID·부모 대화 격리, 실행 중 조회·중단, 알림, 재개, 부모별 생성 설정과 자식 호스트 생성 설정의 분리를 확인한다. 데몬의 `--agent-subagent-options FILE`은 기존 GenerationOptions 검증기를 재사용한다. 잘못된 JSON·범위·알 수 없는 필드는 드라이버 초기화 전에 실패한다. API의 기존 150ms 기한 시험에서 이미 완료된 요청의 status를 조회하던 경합은 NotFound일 때 future 완료를 확인하도록 보완했으며 Timeout·대기 요청 취소 검사는 유지했다. MCP의 연결별 대화 격리와 연결 종료 시 진행 중 자식 중단은 결정적인 서버 세션 검사로 확인한다. 이 검사들의 성공을 실제 Society/Dreamscapes 제품 UI 검증으로 대체하지 않는다.

8B 검사는 Qwen3 Q4_K_M의 SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`, 5,027,783,488바이트 모델을 Service에서 검증해 사용한다. 컨텍스트 8,192, 캐시 컨텍스트 1개, `enable_thinking=false`, `tool_grammar=false`, temperature 0, 생성 한도 2,048, 부모/자식 턴 한도 6이다. 부모가 실제 Agent를 선택하고 자식이 예측 불가능한 파일 값을 Read한 뒤 부모가 그 값을 최종 답변에 포함해야 통과한다. 분기에서는 부모의 파일 힌트를 자식 기록에서 확인한다. 백그라운드 재개는 같은 자식이 변경된 파일 값을 다시 읽어야 하며 알림 수 1→2와 도구 호출/결과 짝도 검사한다. 네 흐름의 개별 결과를 합쳐 종료 코드를 결정한다.

첫 8B 시험에서는 자식의 파일 읽기·분기·백그라운드·재개가 성공했으나 부모가 완료 결과를 계속 조회해 6턴 제한에 도달했다. 원래 요청·프로파일 본문을 도구 결과에 반복하지 않도록 하고 완료 응답과 retrieval_status 설명을 명확히 했다. 수정 전 실패(118.80초), 설명 변경 후 통과(81.55초), 경계 수정 후 최종 소스 통과와 설치 소비자 통과는 각각 별도 실행이다. 실패 기록을 제거하거나 판정 조건을 완화하지 않았다.

0.5B API는 **실제 자식 작업 수락 검증을 통과하지 못했다**. 기본 자식 설정(temperature 0.7, 256토큰)에서는 Read 없이 파일 내용을 지어냈다. 호스트 생성 설정을 명시한 소스 대조(temperature 0, 512토큰)는 기존 부모가 실제 Read 결과의 `LOCAL_` 접두사를 다른 형식으로 바꾸어 자식 검사 전에 실패했다. 같은 자식 설정의 설치 대조에서는 자식의 첫 구조화 응답이 출력 한도에 도달했다. 토큰 상한을 자동 상향하거나 성공할 때까지 반복하지 않았다. 이 세 실패는 `subagents-final-source-native.log`, `subagents-generation-options-green.log`, `subagents-current-installed-api-native.log`에 남아 있다. 마지막 설치 실패에서는 백그라운드 재개·재시작 후 자식 결과 조회까지 도달하지 못했다. 이 API 경로의 실제 8B daemon 호출이나 일반적인 0.5B 신뢰성은 입증하지 않는다.

공식 MCP 설치 검사는 mcp 1.26.0과 고정 Qwen2.5 0.5B Q4_K_M(491,400,032바이트, SHA-256 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`)을 사용한다. 두 전송에서 foreground 자식, background 재개, 실제 두 번의 Read, 두 파일 값 관측, 짝지어진 JSONL, 완료 알림 1개, 종료 상태 중단 요청을 확인했다. MCP의 성공은 실패한 API fixture와 별개이며 모든 모델·프로파일·앱 작업에서의 품질을 보장하지 않는다.

최종 설치 prefix는 `build/subagents-current-stage`, 새 소비자는 `build/subagents-current-consumer/build`이다. 라이브러리 SHA-256은 `7cb02862a0522346d66b055f504ab3d7cb0e874be0b144f61d14b69120621b03`, UUID는 `2914932E-BD2D-3C44-8E77-03EA0E99265D`이다. 소스 8B 검증 뒤 서브에이전트 실행기와 라이브러리 해시는 유지됐다. 이후 추가한 데몬 생성 설정 옵션은 별도 빌드·파라미터 거부·API 검사로 검증했다. 라이브러리 경로 환경변수를 지우고 설치된 0.15.0을 실제 로딩했으며 iillm이 libiiLocalLLM·llama·ggml을 링크하지 않는 것도 확인했다. 마지막 문서 변경은 같은 바이너리로 다시 설치해 문서·카탈로그 일치를 확인한다.

명령·JUnit·개별 모델 흐름·실패·파일 동일성은 `build/subagents-current-verification.json`, `subagents-current-*`, `subagents-final-source-native*`, `subagents-platform-guard*`, `subagents-current-installed-wire/`에 있다. 계약은 [Subagents.md](Subagents.md), 남은 범위는 [HarnessParity.md](HarnessParity.md)에 기록한다. 파일 기반 전문 프로파일, 스킬 fork, 전용 생명주기 훅, 팀·mailbox·worktree·remote 격리와 전체 앱 검증은 남아 있다. 완료 알림과 최종 레코드는 하나의 원자적 트랜잭션이 아니며 강제 종료 시 정확히 한 번 전달을 보장하지 않는다. 전체 목표는 계속 진행 중이다. 이번 단계에서 사용자 기본 SDK나 제품 앱을 재설치하지 않았으며 iPhone 제외 지시를 유지한다.

## 2026-09-14 C++ 로컬 스킬과 인증 호출 경로 (0.14.0)

`SKILL.md`의 제한된 탐색·YAML 메타데이터·리터럴 인자 치환·인라인 대화 주입을 C++로 구현했다. 직접 사용자 호출과 모델의 `Skill` 도구 호출을 구분하며, 호출 당시 본문·경로·SHA-256을 세션에 보존한다. 같은 턴의 도구 결과를 모두 짝지은 후 본문을 주입하고, 저장 직후 중단되면 이미 커밋된 결과에서 누락된 본문을 한 번 복원한다. 인증 API·native IPC·HTTP·CLI·MCP에 목록과 직접 호출을 연결했다. 기존 Qt와 libyaml 0.2.5를 재사용하며 새 생산 의존성이나 Python 실행기를 추가하지 않았다. 공개 `EngineOptions`·`RunRequest` 레이아웃 변경에 따라 ABI는 0.14이며 소비자를 다시 빌드한다.

| 검증 | 관측 결과 |
|---|---|
| Release 전체 빌드·inference 라벨 제외 CTest | **39/39 통과**, 79.00초 |
| ASan·UBSan, llama 비활성 Debug | **37/37 통과**, 69.86초. `ASAN_OPTIONS=malloc_context_size=0` |
| 새 설치 소비자, 이름 끝의 inference 제외 | **16/16 통과**, 33.95초 |
| 소스 Qwen3 8B 스킬 실제 추론 | **1/1 통과**, 63.49초. 직접 호출·모델 호출 두 흐름 |
| 설치본 Qwen3 8B 스킬 실제 추론 | 독립 소비자 **종료 코드 0**, 68.01초. 직접 호출·모델 호출 두 흐름 |
| 소스·설치본 Qwen2.5 0.5B 스킬 실제 추론 | 각각 **0/1 통과**. 직접 호출 성공, 모델 호출 반복으로 실패 |
| 설치 API·CLI·공식 MCP stdio/HTTP | **6/6 통과**. 모델 없는 세 경로와 실제 모델을 쓰는 세 경로 |
| 설치 파일·ABI·로더 | 공개 헤더 35개, 문서·카탈로그, 바이너리 SHA-256·UUID 일치. 실제 stage 라이브러리 로딩 및 얇은 CLI 링크 확인 |

이 Release 범위는 전체 추론 검사의 통과를 뜻하지 않는다. 이번 변경과 직접 관련된 실제 모델 검사는 위 표에 따로 기록했다. Sanitizer는 할당·해제 스택 이력을 끈 조건이며 ASan·UBSan 계측은 유지한다. 기존 Task·입력 큐·다른 모델의 수락 검사 결과는 이번 실행으로 갱신하지 않는다.

스킬 단위 검사는 10개 동작 사례이며 Qt 초기화·정리를 포함해 12 passed이다. 중복 이름·canonical 경로 우선순위, 다음 탐색의 파일 변경 반영, 호출 주체별 제한, 미지원 실행 속성, 잘못된 YAML·UTF-8·자원 한도·취소, 경로 이탈, 리터럴 인자 치환을 검사했다. 엔진 검사는 같은 턴 결과의 순서, 재시작·분기의 원본 보존, 관찰자 오류 뒤 한 번 복원, 압축 뒤 중복 주입 방지, 다른 도구가 스킬 메타데이터를 흉내 내는 경우를 포함한다. API·MCP 검사는 앱·연결별 대화 격리와 목록에 본문을 넣지 않는 계약, 호스트 비활성화, 큐에서 미지원 스킬 필드 거절을 확인한다.

실제 모델 검사는 매 흐름마다 새 난수 파일을 만들고, 모델이 생성한 `Read` 호출·성공한 도구 관측·저장된 스킬 본문·최종 답의 난수 값 포함·짝이 맞는 대화 이력을 모두 요구한다. 직접 호출은 `RunRequest.skill`을 사용한다. 모델 호출 검사는 프롬프트에 스킬 이름 `inspect`와 인자 `secret.txt`를 지정하고 실제 `Skill`→본문 주입→`Read`→최종 답 순서를 확인한다. 따라서 일반 작업에서 스킬을 스스로 선택하는 정확도는 별도 평가 대상이다. 소스와 설치본의 Qwen3 8B는 직접 호출 2턴, 모델 호출 3턴에 완료했다. 각 실행의 파일 값과 원문은 별도로 보존했다.

Qwen3 8B Q4_K_M 가중치는 5,027,783,488 bytes, SHA-256 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 컨텍스트 8,192, 턴당 최대 2,048토큰·6턴, temperature=0, `enable_thinking=false`, `tool_grammar=false`, `maxCachedContexts=1`로 검사했다. 첫 4-context 구성은 RAM 정책에 의해 모델 로딩에서 거절됐다. 순차 검사에 필요한 캐시 컨텍스트 수를 1로 지정한 뒤 소스·설치본의 두 흐름을 통과했으며 메모리 예산·예약 보호나 호스트 기본값은 바꾸지 않았다. 0.5B는 기존 템플릿과 기본 로딩 옵션, 컨텍스트 8,192, 턴당 512토큰·6턴, temperature=0을 사용했다.

0.5B의 직접 호출은 실제 파일을 읽고 값을 최종 답에 포함했다. 모델 호출 경로는 `Skill`을 반복 호출하고 `Read`를 수행하지 않아 턴 한도에 도달했다. 이미 로딩된 스킬이라는 표시와 간결한 도구 결과를 적용한 뒤에도 소스·설치본에서 같은 실패를 관측했다. 이 실패 검사를 유지하며 작은 모델의 일반적인 스킬 실행 신뢰성을 보증하지 않는다. 최초 직접 호출에서는 스킬 리소스 디렉터리를 작업 디렉터리로 오해해 존재하지 않는 경로를 읽었다. 주입 메시지에 두 디렉터리와 도구의 상대 경로 기준을 명확히 한 뒤 직접 호출이 통과했다. 난수 값이나 필수 `Read` 조건을 프롬프트·호스트 답변으로 대체하지 않았다.

첫 회귀 묶음은 Release 38/39, Sanitizer 37/37이었다. Release의 `iiLocalLLM.mcp_official`은 기본 10초의 initialize 기한을 넘겨 실패했고, 위 표의 마지막 직렬 검사에서는 통과했다. 기존 간헐 초기화 지연의 근본 원인이 해결됐다고 해석하지 않는다. 새 API 단위 시험의 초기 충돌은 임시 JSON 객체에서 얻은 `QJsonValueRef`의 수명 문제를 `.value()` 복사로 고쳤다. 당시 ASan 대조에서는 충돌이 재현되지 않았다. YAML 구분자 뒤 공백·탭이 있으면 frontmatter를 본문으로 취급해 호출 제한 필드를 놓치는 경우도 새 시험으로 실패를 재현한 후 수정했다. 첫 설치 API 실제 모델 검사는 추가된 스킬 세션까지 성공했지만 마지막 세션 수를 3으로 기대해 실패했으며, 실제 4개 세션을 검사하도록 시험을 바로잡았다. 이 시험 작성 오류들을 제품의 추론 실패와 구분한다.

설치 API 검사는 스킬 목록을 HTTP·native IPC·일반 RPC CLI·사용자용 CLI에서 대조하고 다른 앱의 세션 접근 거절 및 실패 실행의 종료 코드 1을 확인했다. 실제 모델 검사에서는 사용자용 CLI의 스킬 호출 뒤 파일 값과 저장된 스킬 메타데이터를 확인했다. 공식 Python MCP 1.26.0의 stdio·HTTP 검사도 `iiLocalLLM.agent.skills.list`와 `iiLocalLLM.agent.run`의 직접 스킬 인자를 사용해 실제 `Read`와 난수 관측을 확인했다. 실제 Society·Dreamscapes UI에서 스킬을 사용하는 제품 검증은 아직 수행하지 않았다.

최종 설치는 `build/skills-current-stage`, 새 소비자는 `build/skills-current-consumer/build`이다. 라이브러리 SHA-256은 `48540a1174aacdb91b0d8eeacd9c5bd74a809743c7830db6530cd68dc4fa1964`, UUID는 `44DB732C-9094-3E08-B3C4-F59AFBA8D60C`이다. 소스 실제 추론 이후 전체 빌드·설치에서도 바이너리 해시가 같았다. 라이브러리 경로 환경변수를 제거하고 설치된 0.14.0의 실제 로딩을 확인했으며 `iillm`은 Qt Core/Network와 시스템 라이브러리만 링크한다. 마지막 검증 문서 갱신 후에는 같은 바이너리에서 설치 문서와 메타데이터 일치를 다시 확인한다.

명령·JUnit·실패 원문·모델 흐름·파일 동일성은 `build/skills-current-verification.json`, `skills-current-*`, `skills-routing-native*`, `skills-current-installed-wire/`에 있다. 첫 시도의 `skills-first-*`, `skills-final-*`, `skills-stage`, `skills-consumer/build`, `skills-delimiter-red.log`도 보존한다. 계약과 재현 구성은 [Skills.md](Skills.md), [MCPServer.md](MCPServer.md), 테스트의 CMake 옵션을 참조한다. 스킬의 fork·권한 추가·전용 훅·플러그인·원격 스킬·활성 스킬 재주입 등은 [HarnessParity.md](HarnessParity.md)에 **partial**로 남겨 둔다. 전체 하네스 목표는 진행 중이며, 이번 단계는 SDK와 Workspace 설치 검증 범위이다. 기기 재설치의 iPhone 제외 지시를 유지한다.

## 2026-09-14 MCP 초기화 지연 재현과 구조화 진단 (0.13.2)

C++ `mcp::RequestTimeoutError`에 요청 메서드·적용 기한·단조 시계 경과 시간·전송 계층 제출 여부를 보존했다. 기존 ErrorCode::Timeout 처리는 유지한다. 연결 관리자의 실패 상태는 connect/discover_tools 단계와 해당 단계의 경과 시간을 제공하고 로컬 RPC 시간 초과인 경우에만 request_timeout을 덧붙인다. 성공하면 이전 실패 정보를 지운다. 기본 기한 10초를 유지하며, 이 변경은 재현된 간헐 실패를 해결했다는 뜻이 아니다. Qt와 기존 C++ 계층을 재사용했고 새 생산 의존성이나 Python 런타임을 도입하지 않았다. 공개 오류 형식은 추가했으며 기존 구조체 레이아웃과 SOVERSION 0.13은 유지한다. 서버 정의의 initializeTimeoutMs·requestTimeoutMs로 호스트 기본값을 개별 재정의하며, ms 단위 양의 32비트 정수만 연결 전에 허용한다. 기한 변경은 reload에서 새 연결에 반영하고 삭제하면 호스트 설정을 복원한다.

| 검증 | 관측 결과 |
|---|---|
| Release 전체 빌드·비추론 CTest | **38/38 통과**, 52.03초 |
| ASan·UBSan 전체 | **36/36 통과**, 62.37초 |
| 새 설치 소비자, inference 이름 제외 | **15/15 통과**, 31.25초 |
| 소스 configured MCP 실제 추론 | **1/1 통과**, 7.18초 |
| 설치본 configured MCP 실제 추론 | **1/1 통과**, 7.20초 |
| 설치 API·CLI·공식 MCP stdio/HTTP | **3/3 통과** |

실패 기록: 이 표의 실행에서는 없음. 기한 필드 추가 후 최초 검사는 Release 36/38·Sanitizer 35/36였으며 수정 전 회귀의 프로세스 시작/RPC 구분 실패가 양쪽에 있었다. Release agent_transport의 세션 생성도 기존 HTTP 기한을 넘어 실패했다. 수정 후 단독 대조는 Release 2/2·Sanitizer 2/2였으며 위 표의 마지막 직렬 검사와 별도 결과이다. HTTP 제품 기한은 바꾸지 않았다. 이번 Release CTest는 inference 라벨을 제외한 범위이며 전체 59개 검사 통과로 해석하지 않는다. Sanitizer는 llama 비활성 Debug와 `ASAN_OPTIONS=malloc_context_size=0` 조건이며 할당·해제 스택 이력은 꺼져 있다. 이전 0.13.1의 모델 실패 기록은 아래에 유지한다. 이번에 다시 실행하지 않은 Qwen3 입력 큐·Task 등 모델 검사의 결과를 갱신하지 않는다.

initializeTimeoutMs=300을 지정한 응답 없는 loopback HTTP MCP 서버를 새 설치 daemon에 연결한 별도 시험에서는 인증된 HTTP·native IPC·CLI 세 경로의 실패 정보가 일치했다. 실제 단계 경과 시간은 315ms, initialize RPC 기한은 300ms, 경과 시간은 303ms, submitted=true였다. 인증 없는 HTTP 조회는 401로 거부됐고 토큰은 응답과 daemon 로그에 없었다. 이 시험은 모델을 로드하지 않는다. 증거는 build/mcp-deadlines-failure-wire-result.json이다. 기한 필드 추가 전 진단 설치본에서는 기본 10,000ms·요청 경과 10,011ms·단계 경과 10,029ms도 같은 세 경로에서 관측했다. 그 기록은 build/mcp-diagnostics-failure-wire-result.json에 보존한다.

TDD의 초기화·일반 stdio 요청·HTTP 요청·관리자 단계 진단 4개 회귀는 구현 전 모두 실패했다. 새 진단에서 tools/list의 실제 페이지 기한이 149ms로 관측되어 원래 전체 한도 150ms와 같다고 작성한 시험을 바로잡았다. HTTP 연결 거부를 반드시 특정 예외 형식으로 끝난다고 가정한 시험도 제거하고, 존재하지 않는 실행 파일로 비-RPC 실패의 분리를 검증했다. 100ms 호스트 기한 복원 검사는 프로세스 시작에서 먼저 소진되는 RuntimeUnavailable과 initialize RPC 시간 초과를 구분하도록 바로잡았다. 이후 최종 시험은 응답 없는 초기화, 목록 지연, 같은 연결의 목록 복구, 오류 정보 삭제, 인증 정보 제외와 기존 취소·늦은 응답·자동 재전송 금지를 검사한다. 초기 red/green 기록은 build/mcp-startup-diagnosis/에 그대로 보존하며 시험 작성 오류를 제품 결함으로 계산하지 않는다. 서버별 기한 필드 회귀 2개도 필드가 없던 설치 라이브러리에 새 시험을 링크하여 실패를 확인했다. 최종 검사는 지연된 초기화의 허용, 서버별 요청 기한, reload 후 새 기한, 삭제 후 호스트 기본값 복원, 잘못된 타입·범위를 검증한다. build/mcp-deadlines-red/red.log에 변경 전 결과가 있다.

수정 전 소스 0d18f0a의 공식 Python MCP 1.26.0 시험을 사전에 정한 30회 반복했다. 28회 통과했고 0번째·2번째는 initialize 시간 초과로 각각 12,538.46ms·11,174.19ms에 종료됐다. 기존 실행 파일·원래 서버·기본 10초를 사용했으며 Service 모델 로딩은 없었다. 반복을 성공할 때까지 연장하지 않았다. 원본 결과는 build/mcp-startup-diagnosis/original-1/에 있다.

별도 진단에서는 client 송신과 peer 진입·import 완료·수신을 기록했다. 첫 진단의 0~3번째는 Python import 완료 전에 기한이 소진됐다. 계측 전송 사례 1·3에서는 initialize가 각각 약 145ms·148ms에 제출됐고 종료 시 QProcess 잔여 송신 바이트는 0이었다. 4~29번째의 서버 종료는 진단 wrapper가 AsyncFile iterator를 잘못 호출한 결함이므로 SDK 실패에서 제외한다. wrapper를 고친 다음 진단 30회(라이브러리 전송 15회·계측 복사본 15회)는 모두 성공했다. 이때 초기화는 414.96~856.08ms, 전체 도구 호출·종료는 502.77~1,269.11ms였다. wrapper와 roots 구성이 원래 시험과 다르므로 이 결과를 원본 실패의 해결 증거로 합치지 않는다. run-1/run-2의 원문과 집계를 보존한다.

원래 공식 서버 코드에 Python -X importtime과 shell exec wrapper를 적용한 별도 10회 검사(실행 사이 15초)에서는 6회 성공·4회 initialize 시간 초과가 관측됐다. 5초를 넘긴 실행에는 해당 Python 자식만 1초간 sample을 수행했다. 실패한 0·2·3·8번째의 샘플은 각각 read 최상위 스택 55·83·80·65개를 기록했고 Python import 호출 경로 안의 파일 읽기였다. 0·2·8번째는 FastMCP import 완료 기록이 없었으며, 3번째는 FastMCP 완료 뒤의 추가 import도 기록됐다. 기록된 import self 시간의 합은 각각 9.021·9.439·10.343·9.072초이며 완료되지 않은 import를 포함한 전체 초기화 시간과 같지 않다. 이는 계측을 추가한 별도 진단으로, 샘플링 영향과 원래 무계측 시험을 구분한다. 원문·스택·명령·집계는 build/mcp-startup-diagnosis/import-profile-1/에 있다. 이 관측은 계측된 일부 실패에서 peer import 지연이 있었다는 근거다. 계측하지 않은 원본 실패 모두의 원인이나 운영체제·저장 장치의 지연 원인은 아직 확정하지 않는다. Qt 6.8.3의 [QProcess Unix 구현](https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/io/qprocess_unix.cpp#L1066-L1105)은 준비 대기 중 송신 가능 이벤트도 처리하며, 현재 기록에서 송신 버퍼 정체를 원인으로 확정하지 않았다. 분석본의 [MCP 연결 기한](https://github.com/Exhen/claude-code-2.1.88/blob/c8cd253554319f32ff64ff7000636199f720c9bc/source/src/services/mcp/client.ts#L433-L435)은 기본 30초와 MCP_TIMEOUT을 사용하지만 iiLocalLLM은 기존 C++ 옵션의 별도 계약을 유지한다.

서버별 기한 필드를 추가하기 전 중간 검사는 Release 비추론 37/38·Sanitizer 35/36·설치 소비자 14/15였고, 각 환경에서 공식 stdio 서버의 초기화가 한 번씩 실패했다. 그 결과를 최종 검사에 합치지 않으며 build/mcp-diagnostics-*와 당시의 stage·소비자를 보존한다.

최종 설치는 build/mcp-deadlines-stage, 새 소비자는 build/mcp-deadlines-consumer/build이다. 공개 헤더 34개, 소스·설치 바이너리 SHA-256 및 UUID, 실제 설치 라이브러리 로딩, 세 실행 파일의 0.13.2 버전과 Qt Core/Network만 사용하는 CLI를 확인했다. 라이브러리 SHA-256은 `cd401b2249bca7d5780dbdd37bb277042dc6bc0e3f1e5cbd76d447b9242cb697`이고 UUID는 `D5492472-8B1E-31BD-BAB1-AE45B15C2D68`이다. 마지막 검증 기록을 설치 문서에 반영하고 같은 바이너리에서 파일 일치를 다시 검사했다. 실제 왕복 검사를 문서만 바뀐 이유로 반복하지 않는다.

전체 명령·JUnit·로그·실패 원문·파일 동일성은 build/mcp-deadlines-verification.json과 mcp-deadlines-*에 보관한다. SDK의 별도 설치본까지 검증했으며 기본 SDK 설치나 앱·기기 재배포는 이번 단계에서 수행하지 않았다. iPhone은 사용자 지시로 제외하며 전체 하네스 목표는 진행 중이다. 계약은 [MCP.md](MCP.md), [ToolDiscovery.md](ToolDiscovery.md), 범위는 [HarnessParity.md](HarnessParity.md)를 참조한다.

## 2026-09-14 네이티브 도구 관측의 구조화 결과 보존 (0.13.1)

ServiceModel이 도구의 text만 모델에 전달해 data를 누락하던 결함을 수정했다. 이제 tool content는 text·data·is_error를 담은 JSON 관측이며 구조화 결과만 있는 앱 도구, 부분 읽기·검색 잘림·셸 종료 코드·중단 여부가 모델 입력에 포함된다. 원문 공백·줄바꿈·따옴표와 중첩 값은 보존하고 Message.metadata는 제외하며 도구가 data 안에 포함한 metadata 값은 보존한다. 측정과 생성이 같은 변환을 사용하여 구조화 결과가 실제 컨텍스트 예산에 반영된다. 기존 Qt JSON과 C++ 어댑터를 사용하며 새 생산 의존성은 없다.

| 검증 | 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 빌드 성공, **54/59 통과**, 742.57초 |
| ASan·UBSan 전체 | llama 비활성 Debug **36/36 통과**, 92.79초. ASAN_OPTIONS=malloc_context_size=0 |
| 새 설치 소비자 | observation-stage 및 observation-consumer/build, **23/25 통과**, 516.98초 |
| 설치 API·CLI·공식 MCP | 5/5 통과. API·CLI 35개 조건 및 Python MCP 1.26.0 stdio/HTTP 왕복 |
| 설치·로더 | 공개 헤더 34개 일치, 소스/설치 라이브러리 SHA-256·UUID 일치, 실제 0.13.1 stage 로드, 세 실행 파일 버전과 얇은 CLI 링크 확인 |

Release 실패: `iiLocalLLM.mcp_official` · `iiLocalLLM.tasks_inference` · `iiLocalLLM.tasks_catalog_thinking_control` · `iiLocalLLM.configured_mcp_inference` · `iiLocalLLM.discovery_inference`. 설치 소비자 실패: `iiLocalLLM.installed_tasks_inference` · `iiLocalLLM.installed_discovery_inference`. Sanitizer 실패: 없음. 단독 대조를 전체 검사 결과에 합치지 않는다. ASAN_OPTIONS는 할당·해제 스택 이력을 끄는 이전 검증 조건이며 ASan·UBSan 계측 자체는 유지한다. 기본 ASan 설정의 통과로 해석하지 않는다.

TDD에서는 구조화 결과 누락과 그 데이터가 토큰 측정에서 제외되는 현상을 새 회귀 두 개로 재현했다. 변경 전 두 사례가 모두 실패했고, 수정 뒤 두 사례와 기존 reasoning-only 거절 사례가 모두 통과했다. 원문 왕복, 중첩·null 값, 빈 text, 오류 상태, 호스트 metadata 제외, 측정/생성 요청 일치와 큰 data의 ContextOverflow를 검사한다. build/prompt-diagnosis/observation-red-tests.log 및 observation-green-tests.log를 보존한다.

변경 전 0.13.0의 입력 큐 오답도 별도로 진단했다. 고정 Qwen3 8B에서 동일한 모델 입력·seed·샘플링을 유지한 6개 요청을 기존 KV 컨텍스트와 새 컨텍스트로 각각 실행했다. 실제 토큰을 원문으로 변환한 프롬프트에 도구 원문이 모두 있었고 6개 요청의 raw 출력도 각각 일치했다. 후속/긴급 파일의 실제 값은 최종 답에서 예시 문장으로 바뀌었다. 따라서 이 관측에서 입력 누락·파서의 답 치환·KV 접두사 재사용은 오답을 설명하지 않는다. 모든 런타임·모델 원인을 배제한 결론은 아니다. 이 대조와 이후 발견한 data 누락 결함을 같은 원인으로 단정하지 않는다. 증거는 build/prompt-diagnosis/comparison.json과 native-trace.jsonl이다.

입력 큐의 실제 모델 검사는 0.13.0의 프롬프트·모델·생성 설정을 유지하며 판정은 부분 문자열 포함에서 완전 일치로 강화했다. Qwen3 8B Q4_K_M, 컨텍스트 8,192, 턴당 2,048토큰·최대 6턴, seed 0, temperature 0.7·top_p 0.8·top_k 20, enable_thinking=false·tool_grammar=false이며 /no_think를 추가하지 않는다. 모델에 전달되는 도구 관측 형식과 그 형식을 설명하는 시스템 지침은 이번 수정으로 바뀌었다. 결과는 한정된 수락 조건의 관측이며 일반적인 모델 정확도 보증이 아니다.

소스 입력 큐의 개별 관측은 다음과 같다.

- next: 통과. 기대값 `INPUT_af5f1b6bd967`, 최종 답(JSON 문자열 표기) `"INPUT_af5f1b6bd967"`. 제어 판정 `{"delivered":1,"first_reads":1,"history_paired":true,"next_reads":1,"queue_empty":true,"value_exact":true}`.
- now: 통과. 기대값 `INPUT_03861620395d`, 최종 답(JSON 문자열 표기) `"INPUT_03861620395d"`. 제어 판정 `{"bash_calls":1,"history_paired":true,"interrupted":1,"no_later_side_effect":true,"queue_empty":true,"root_not_cancelled":true,"shell_gone":true,"urgent_reads":1,"value_exact":true}`.

설치본 입력 큐의 개별 관측은 다음과 같다.

- next: 통과. 기대값 `INPUT_9ddbe8da0f50`, 최종 답(JSON 문자열 표기) `"INPUT_9ddbe8da0f50"`. 제어 판정 `{"delivered":1,"first_reads":1,"history_paired":true,"next_reads":1,"queue_empty":true,"value_exact":true}`.
- now: 통과. 기대값 `INPUT_7b04b7d6662d`, 최종 답(JSON 문자열 표기) `"INPUT_7b04b7d6662d"`. 제어 판정 `{"bash_calls":1,"history_paired":true,"interrupted":1,"no_later_side_effect":true,"queue_empty":true,"root_not_cancelled":true,"shell_gone":true,"urgent_reads":1,"value_exact":true}`.

추론 모드를 명시적으로 끈 소스 Task 검사에서는 실제 ToolSearch→TaskGet이 수행됐지만 최종 답이 임의 description 대신 subject인 Inspect artifact여서 실패했다. 이전 0.13.0의 해당 검사는 통과했으므로 이번 관측을 개선으로 표현하지 않는다. 설치본의 해당 검사는 통과했다. 두 실행은 서로 다른 임의값을 사용하므로 같은 모델 입력의 재실행이 아니다. 입력 누락 수정과 모델의 필드 선택 정확도를 별도로 추적한다. 0.5B의 TaskGet 생략 및 지연 MCP 검색 후 실행 누락도 실패 기록에 남긴다.

변경 전 원본 configured_mcp_inference의 별도 대조는 7.04초에 통과했다. 수정 후 전체 검사에서는 보강한 실패 기록으로 configured stdio 연결의 timeout, generation 0, tool_count 0을 확인했다. 이 값은 게시된 연결 상태이며 초기화와 도구 목록 요청의 어느 내부 단계에서 지연됐는지를 단독으로 증명하지 않는다. 별도의 mcp_official 검사도 MCP initialize request timed out으로 실패했다. 해당 시험은 Service 모델 로딩을 하지 않으므로 두 실패를 모델 로딩 때문이라고 단정할 수 없다. 반복된 상대 연결 준비 실패의 근본 원인은 아직 확정하지 않는다. 모델의 도구 실행/답변 실패와 구분한다.

라이브러리 SHA-256은 `ae11597ff2572e43c9432563ea1b1f628ca0df4ea0e73d5fc8b66545e98c414f`, UUID는 `D86818C1-5FFF-3A14-AD31-E8E32FE5F2B2`이다. llama 원본은 고정 아카이브와 핵심 소스 8개의 SHA-256을 다시 대조했다. 기존 아카이브 경계와 revision은 build/observation-verification.json에 기록한다. 구조화 결과의 별도 페이지화와 자동 압축 대상 개선, 일반 모델 신뢰성, 나머지 하네스 기능은 계속 남아 있다. 큰 구조화 결과는 기존 서비스 입력·컨텍스트 한도에 걸릴 수 있으며 묵시적으로 버리지 않는다.

이 단계는 SDK와 Workspace의 별도 설치본까지이며 기본 SDK 설치나 기기 앱 재배포는 수행하지 않았다. iPhone은 사용자 지시로 제외한다. 전체 하네스 목표는 진행 중이다. 계약은 [AgentHarness.md](AgentHarness.md), 범위는 [HarnessParity.md](HarnessParity.md)에 있다. 전체 로그·JUnit·원문은 build/observation-final-{release,sanitizer,consumer}-*, 설치 검사는 build/observation-installed-wire/, 종합 기록은 build/observation-verification.json에 보관한다.

최초 설치 검증에서 실제 왕복 5개는 통과했지만 설치 뒤 수정한 harness-parity.json의 파일 일치 검사가 실패했다. 문서와 대응표를 다시 설치하고 동일한 바이너리 SHA-256을 확인한 뒤 메타데이터·로더 대조를 통과했다. 바이너리 변경이 없어 성공한 왕복 결과는 재실행하지 않았다. 초기 실패와 최종 대조는 observation-installed-wire-checks.log 및 observation-installed-final-checks.log에 별도로 보존한다.

## 2026-09-14 영속 입력 큐와 실행 중 방향 전환 (0.13.0)

C++ `InputQueue`에 대화별 prompt/notification 저장, now/next/later 우선순위와 같은 종류의 묶음 전달을 구현했다. Engine은 현재 모델·도구 연산의 취소와 전체 실행 취소를 구분하며 미완료 도구 결과를 중단 기록으로 짝지은 뒤 후속 입력을 처리한다. 인증 API·native IPC·HTTP·CLI·MCP를 연결했고 유휴 큐는 명시적 runQueued로 시작한다. Qt의 QLockFile·QSaveFile과 기존 C++ 실행 계층을 재사용하며 새 생산 의존성은 없다.

| 검증 | 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 빌드 성공, **53/59 통과**, 752.05초. 기존 모델 검사 포함 |
| ASan·UBSan 전체 | llama 비활성 Debug **35/36 통과**, 95.50초. 아래 명시적 실행 조건 |
| sanitizer 실패 항목 단독 대조 | **1/1 통과**, 2.05초. 전체 결과와 별도 |
| 새 설치 소비자 | input-stage와 input-consumer/build에서 **20/25 통과**, 457.30초 |
| 실제 Qwen3 8B 입력 큐 | 소스 실패, 65.35초, 설치본 실패, 65.57초. next/now의 실제 호출·프로세스·최종 답을 함께 판정 |
| 설치 API·CLI·공식 MCP | 5/5 통과. API·CLI 35개 조건, 공식 Python MCP 1.26.0 stdio/HTTP 입력 제어와 기존 서버 동작 |
| 설치·ABI·로더 | 공개 헤더 34개 일치, 소스/설치 라이브러리 SHA-256·UUID 일치, 실제 0.13.0 stage 로드, 세 실행 파일 버전 확인 및 얇은 iillm 링크 검사 |

Release 실패는 `iiLocalLLM.tasks_inference` · `iiLocalLLM.tasks_catalog_inference` · `iiLocalLLM.tasks_catalog_discovery` · `iiLocalLLM.input_queue_inference` · `iiLocalLLM.configured_mcp_inference` · `iiLocalLLM.discovery_inference`이다. 설치 소비자 실패는 `iiLocalLLM.installed_tasks_inference` · `iiLocalLLM.installed_tasks_discovery` · `iiLocalLLM.installed_input_queue_inference` · `iiLocalLLM.installed_configured_mcp_inference` · `iiLocalLLM.installed_discovery_inference`이다. 기존 모델·전송 실패와 이번 입력 큐 검사의 결과를 분리하며 단독 통과를 전체 실행의 통과로 합치지 않는다.

Release 단독 대조에서는 RAM 보호로 거절됐던 tasks_catalog_inference가 84.69초에 통과했다. configured_mcp_inference는 28.01초 실행 후 다시 상대 연결 준비 실패로 종료했다. 해당 검사는 Service 모델 로딩 뒤 McpConnections를 생성하고 ready 상태를 확인하는 지점에서 실패했으며, Engine 생성·모델의 에이전트 호출에는 도달하지 않았다. 연결 지연의 근본 원인은 확정하지 않는다.

별도 설치 라이브러리의 연결 진단기는 모델 없이 eager 447ms·deferred 433ms, 모델 로딩 뒤 eager 437ms·deferred 430ms에 각각 ready가 됐고 직접 도구 조회도 2개를 반환했다. 이 진단기는 독립 앱 등록 경로와 미리 만든 fixture 파일을 사용하며 실제 추론은 하지 않는다. 전체 검사의 연결 실패를 재현하지 못했으므로 원인 해결이나 동일 조건 통과 증거로 해석하지 않는다. 근거는 build/input-mcp-diagnosis/와 build/input-queue-mcp-diagnosis.json이다.

입력 큐 C++ 검사는 10개 동작 사례를 포함한다. 우선순위·종류 분리, 스레드 12개·별도 프로세스 4개의 게시, transcript 게시 후 확인 실패 복구, 같은 ID의 본문 충돌, next/now/later 경계, 턴 한도·명시적 취소 뒤 미소비 입력 보존, 이벤트 관찰자의 재입력, 손상·symlink 저장 거절을 확인한다. API는 실행 1개·대기 0개인 조건에서 실행 중 긴급 입력과 앱 격리를 검사하며 MCP는 같은 연결의 실행 잠금과 입력 제어 경로를 검사한다.

실제 모델 수락 검사는 고정 Qwen3 8B Q4_K_M, 컨텍스트 8,192, 턴당 2,048토큰·최대 6턴, seed 0, temperature 0.7·top_p 0.8·top_k 20, tool_grammar=false·enable_thinking=false이다. 이번 프롬프트에는 /no_think를 붙이지 않는다. 첫 흐름은 실제 Read가 끝난 이벤트에서 next 입력을 등록하고 두 번째 파일을 읽어 임의 값을 답하도록 요구한다. 두 번째는 셸이 기록한 PID·준비 파일을 확인한 뒤 now를 등록한다. 셸 프로세스 종료, 중단 결과와 호출의 짝, 재실행 부재, 후속 Read와 임의 파일 값 답변을 모두 요구한다. 호스트는 답이나 도구 호출을 대신 생성하지 않는다.

최초 실제 모델 검사 **0/1 통과**, 138.05초에서는 두 Read와 입력 전달이 관측되었으나 최종 답을 파일 값 대신 `This is the content of next.txt.`라는 예시 문장으로 만들어 실패했다. 이 실행은 긴급 셸 검사에 도달하지 않았다. 이후 두 흐름을 모두 실행한 뒤 개별 판정하도록 시험을 보완했으며 프롬프트·모델 설정·수락 조건은 유지했다. 각 실행은 서로 다른 난수 파일을 사용하며 원문과 판정을 별도로 보관한다.

소스 전체 실행에서 next는 입력 전달 1회와 첫 파일·후속 파일의 Read 각 1회를 수행했지만 최종 답을 파일 값 대신 Read 모양 JSON으로 반환했다. now는 Bash 1회, 중단 기록 1회, 실제 셸 종료와 후속 Read 1회가 확인되었으나 답에서 INPUT_ 접두사를 누락했다. 따라서 실행 제어가 관측되어도 두 흐름 모두 전체 수락은 실패이다. 원문은 input-queue-final-release-LastTest.log의 phase=next/now 기록에 있다.

설치본도 next에서 두 Read와 입력 전달 1회, now에서 Bash 1회·중단 기록 1회·셸 종료와 후속 Read 1회를 관측했다. 그러나 next의 최종 답은 Hello, world! 예시 문장이었고 now는 파일 값 대신 중단 설명을 반환했다. 설치본에서도 원문 재현 조건은 통과하지 못했다.

sanitizer의 전체 실패는 `iiLocalLLM.agent_transport`이다. 세션 생성 단계에서 100ms HTTP 기한을 넘겼으며, 모든 빌드가 끝난 뒤 같은 실행 파일·설정의 단독 대조는 통과했다. 근본 원인은 확정하지 않는다. ASAN_OPTIONS=malloc_context_size=0을 명시했으며 이는 이전 단계의 Apple Objective-C atfork/ASan StackStore 충돌을 피하기 위해 할당·해제 스택 이력을 끈 조건이다. ASan·UBSan 계측은 유지하지만 기본 설정 통과로 해석하지 않는다.

TDD에서 헤더 부재, API/MCP 메서드 부재, 입력 종류 혼합과 transcript 동일 ID의 다른 본문 수용을 재현했다. 후자의 CTest 결과는 **1/2 통과**, 3.44초, 수정 후 관련 검사 결과는 **3/3 통과**, 3.73초였다. 공식 MCP 입력 제어를 포함한 후속 단독 검사는 **5/5 통과**, 42.71초였다. 새 API 시험의 별도 충돌은 임시 JSON 객체에서 얻은 QJsonValueRef의 수명 오류였다. LLDB의 QJsonValueConstRef::concrete 프레임으로 확인해 value() 복사로 수정했다. red 기록과 진단을 보존한다.

llama 원본은 CMake에 고정된 5202104b59ada9005db079eea43882a2b7bf5802 아카이브의 SHA-256과 핵심 소스 8개를 대조했다. 압축 소스에 자체 Git 저장소가 없어 그 안의 git rev-parse는 상위 SDK를 가리킨다. 해당 출력이나 ggml 빌드 문자열을 llama revision 증거로 사용하지 않는다. 근거는 build/input-queue-llama-provenance.json이다.

라이브러리 SHA-256은 `c663d9dc7e7164fed9aa4188fb39cd6c8df5c3578e46a2870be9bde15f15cf42`, UUID는 `64F63825-0240-3A82-B2BE-1C1BC825501A`이다. CancellationToken의 레이아웃과 Engine/API 옵션이 바뀌므로 소비자는 0.13 헤더와 라이브러리로 함께 다시 빌드한다. 이 단계는 SDK와 Workspace의 별도 설치본까지이며 기본 SDK 설치·기기 앱 재배포는 포함하지 않는다. iPhone은 사용자 지시로 제외한다.

자동 유휴 기동, 셸 완료 알림 생산, 첨부·slash/bash 입력 모드, 수신 에이전트 지정·Sleep 깨우기·팀 mailbox는 미완료이다. now는 협력 취소이며 이미 일어난 부작용을 되돌리지 않는다. 입력의 저장·전달은 모델의 작업 완료 증거가 아니고 응답 유실 뒤 재등록의 중복 방지도 제공하지 않는다. 전체 하네스 목표는 계속 진행 중이다.

계약은 [InputQueue.md](InputQueue.md), 전체 범위는 [HarnessParity.md](HarnessParity.md)에 있다. 증거는 build/input-queue-final-{release,sanitizer,consumer}-tests.log와 대응 JUnit XML·LastTest, build/input-queue-installed-wire/, build/input-queue-linkage.json, build/input-queue-native-LastTest.log, build/input-queue-verification.json에 보관한다.

별도 release 대조: **1/2 통과**, 113.35초. 대상: iiLocalLLM.tasks_catalog_inference, iiLocalLLM.configured_mcp_inference.

## 2026-09-14 네이티브 추론 모드 제어와 지연 도구 실행 (0.12.1)

llama.cpp 모델 로딩의 `options.enable_thinking` boolean을 기존 네이티브 Jinja 입력에 연결했다. 명시적 설정은 일반 text/chat과 구조화 conversation에 함께 적용한다. 옵션을 생략하면 기존 동작을 유지한다. 에이전트가 추론 내용만 받았을 때는 빈 응답과 구분하는 `protocol_error`를 반환한다. 기존 C++ llama.cpp 의존성을 재사용하며 가중치·템플릿 원본 수정이나 새 Python 추론 단계는 없다.

| 검증 | 최종 관측 결과 |
|---|---|
| Release 전체 빌드·CTest | 빌드 성공, **50/55 통과**, 717.29초. 기존 모델 검사도 그대로 포함 |
| Release MCP 연결 실패의 단독 대조 | **2/2 통과**, 97.36초. 전체 실행과 별도 관측 |
| ASan·UBSan 전체 | llama 비활성 Debug **32/33 통과**, 127.92초. 공식 MCP stdio 초기화 시간 초과 1건은 아래에 구분 |
| 해당 sanitizer 단독 대조 | **1/1 통과**, 1.11초. 전체 실행의 실패를 통과로 덮어쓰지 않음 |
| 새 설치 소비자 | `thinking-stage`와 `thinking-consumer/build`에서 **20/23 통과**, 362.50초 |
| 실제 Qwen3 8B 지연 Task 도구 | 소스 통과, 68.15초, 설치본 통과, 67.42초. ToolSearch 후 실제 조회·생성·수정 및 저장 값 검증 |
| 실제 Qwen3 8B 지연 MCP 도구 | 소스 전체 실패, 67.99초, 연결 실패 후 별도 대조 통과, 80.32초. 설치본 통과, 60.02초. 통과한 각 실행의 고정/임의 파일 값 두 대화에서 검색·원격 호출·진행 이벤트·최종 값·호출/결과 쌍 검증 |
| 네이티브 템플릿 계약 | 실제 llama runtime의 제어용 Jinja fixture로 boolean 타입, 명시적 true/false, 생략 기본값 및 두 프롬프트 경로 검사. 모델 응답 생성 없이 입력 토큰을 검증 |
| 설치 API·CLI·공식 MCP | 3/3 통과. API·CLI 28개 조건, 실제 daemon/얇은 CLI 및 공식 MCP Python 1.26.0의 stdio/HTTP 왕복 |
| 설치·로더 | 소스/설치 라이브러리 SHA-256·UUID `0A1D0C46-8200-3C8A-96BA-213B0D8C5A52` 일치. 실제 0.12.1 stage 로드, 세 실행 파일 0.12.1, iillm의 추론 라이브러리 미링크 확인 |

기존 조건의 원인을 별도 C++ Runtime 관측기로 재현했다. 첫 모델 출력은 닫힌 추론 구간 뒤의 ToolSearch였고 파싱된 호출을 실제 수행했다. 다음 입력에는 TaskGet과 ToolSearch 정의가 들어갔지만, 다음 출력은 `<think>` 뒤에 닫는 `</think>` 없이 TaskGet 모양의 JSON을 포함했다. 네이티브 파서는 그 문자열을 reasoning으로 반환하고 실행 가능한 호출은 반환하지 않았다. 따라서 이전 기록의 “빈 모델 턴”은 호출을 아예 생성하지 않았다는 뜻이 아니라 ServiceModel이 실행할 응답이 없었다는 뜻이다. 원문은 `build/discovery-diagnosis/build/native-trace.jsonl`과 `baseline.log`에 보존한다.

`/no_think` 소프트 지시와 템플릿의 `enable_thinking=false`를 구분한다. [Qwen3 공식 문서](https://huggingface.co/Qwen/Qwen3-8B)에 따라 별도 제어를 연결했고 추론 안의 도구 문자열을 실행하거나 누락된 경계를 자동 삽입하지 않는다. 새 ServiceModel 회귀는 Write 모양 문자열이 추론에 있어도 도구 호출·최종 텍스트로 승격되지 않는지 확인한다.

새 카탈로그 제어 검사는 Qwen3 8B Q4_K_M, `model://qwen3-8b-q4`, context 8,192, 턴당 2,048토큰·최대 6턴, seed 0, temperature 0.7·top_p 0.8·top_k 20, `/no_think`, `tool_grammar:false`, `enable_thinking:false`를 사용한다. Task의 기존 지연 검사와 비교할 때 추가한 운용 설정은 `enable_thinking:false`이다. MCP의 0.5B 기존 검사와는 모델·샘플링도 다르므로 MCP 결과를 추론 모드 한 가지의 인과 효과로 설명하지 않는다. 모델 가중치는 기존 고정 파일이며 새 다운로드는 없다. revision·크기·SHA-256은 [Tasks.md](Tasks.md)에 있다.

Task 수락 조건은 프롬프트에 없는 임의 설명을 실제 TaskGet으로 읽고, 작업을 정확히 한 번 만들고, 기존 제목·설명을 보존하면서 요청한 담당자·상태를 저장하는 것이다. MCP는 실제 시험 서버의 파일 값을 읽는다. 두 검사 모두 호스트가 호출이나 답을 만들어 넣지 않는다. 명시적인 검색·실행 프롬프트와 해당 모델 설정에서의 수락 결과이며 일반적인 자율 작업 능력 보증은 아니다.

Release 실패: `iiLocalLLM.tasks_inference` · `iiLocalLLM.tasks_catalog_discovery` · `iiLocalLLM.discovery_inference` · `iiLocalLLM.discovery_catalog_thinking_control` · `iiLocalLLM.agent_mcp_inference`. 설치 소비자 실패: `iiLocalLLM.installed_tasks_inference` · `iiLocalLLM.installed_tasks_discovery` · `iiLocalLLM.installed_discovery_inference`. 0.5B의 TaskGet 생략과 MCP 검색 후 실제 호출 생략, `enable_thinking`을 지정하지 않은 8B의 지연 도구 실패는 새 별도 검사의 통과와 구분한다. 기존 기본값·기대값·실패 항목을 제거하지 않았다.

Release의 새 8B MCP 검사와 기존 `agent_mcp_inference`는 모델의 에이전트 실행 전에 각각 `Configured MCP peer did not connect`, `MCP initialize request timed out`으로 실패했다. 전체 검사가 끝난 뒤 같은 두 항목만 대조한 결과가 위 표이다. 성공할 때까지 반복하거나 최초 결과를 바꾸지 않았다. 두 연결 지연의 근본 원인은 확정하지 않으며 모델이 실제 호출 후 잘못 답한 오류와 구분한다. 근거는 `build/thinking-focused-release-*`이다.

TDD의 변경 전 결과는 **0/2 통과**, 2.83초였다. 새 모델 옵션 거절과 reasoning-only 오류 구분 부재를 각각 재현했다. 구현 후 동일 두 검사 결과는 **2/2 통과**, 12.10초이며 근거는 `build/thinking-red-tests.*`, `build/thinking-green-tests.*`이다. 초기 Task 제어 단독 검사는 **1/1 통과**, 81.22초였으며 최종 전체 실행과 별도 기록이다.

sanitizer는 `ASAN_OPTIONS=malloc_context_size=0`을 명시했다. 아래 0.12.0에서 진단한 Apple Objective-C atfork/ASan StackStore 충돌을 피하기 위해 할당·해제 호출 스택 이력을 끈 조건이며 ASan·UBSan 계측은 유지한다. 기본 설정 통과로 해석하지 않는다. 이번 전체 실행에서는 sanitizer 오류 보고 없이 공식 MCP stdio 초기화가 시간 초과했다. 모든 빌드가 끝난 뒤 같은 실행 파일·설정의 단독 대조는 통과했으나 최초 지연의 원인을 확정한 것은 아니다. 근거는 `build/thinking-final-sanitizer-*`와 `build/thinking-focused-sanitizer-*`이다.

라이브러리 SHA-256은 `d6512cde9bffa41e4bea56970654d4c48f3d65a8b54fc427a16e51fc71ac2bfe`이다. 이 단계는 SDK와 Workspace의 별도 설치본까지이며 기본 SDK 설치나 Society·Dreamscapes 기기 앱 재배포를 포함하지 않는다. iPhone은 사용자 지시로 제외한다. MLX의 이 옵션, 모델 템플릿별 지원 전체, 점진적 추론·도구 스트리밍, 외부 provider 및 남은 하네스 기능은 미완료이다. 전체 하네스 목표는 계속 진행 중이다.

계약은 [NativeThinking.md](NativeThinking.md), 전체 범위는 [HarnessParity.md](HarnessParity.md)이다. 최종 증거는 `build/thinking-final-{release,sanitizer,consumer}-tests.log`, 대응 JUnit XML·LastTest, `build/thinking-installed-wire/`, `build/thinking-linkage.json`, `build/thinking-installed-loader.log`, `build/thinking-verification.json`에 보관한다.

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
