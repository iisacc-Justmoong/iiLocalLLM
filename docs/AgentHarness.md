# C++ 에이전트 실행 계층

전체 목표와 미완료 영역은 [HarnessParity.md](HarnessParity.md) 및 `catalog/harness-parity.json`에서 추적한다. 이 문서는 현재 추가한 네이티브 실행 계층의 실제 계약을 설명한다. 기존 daemon의 HTTP에는 모델의 함수 도구 호출을 연결했다. 에이전트 실행/세션 관리 IPC·HTTP 메서드와 MCP 전체 호환은 아직 미완료이다.

## 계층

`agent::Engine` → `agent::Model` / `agent::ToolRunner` / `agent::SessionStore`로 나뉜다. ToolRunner는 ToolRegistry와 PermissionPolicy를 사용한다. 기본 도구와 앱 도구는 같은 계약을 사용하며 UI·HTTP·MCP를 참조하지 않는다. `ServiceModel`만 기존 추론 Service를 참조하는 어댑터이다. 하위 추론 런타임은 에이전트 계층을 참조하지 않는다.

공개 헤더는 `agent/Types.h`, `agent/Tools.h`, `agent/SessionStore.h`, `agent/Engine.h`이며 헤더와 구현은 같은 디렉터리에 둔다. 설치 후에도 `<agent/Engine.h>`로 사용한다. JSON Schema의 jsoncons 타입은 외부 ABI에 노출하지 않는다.

## 모델과 도구

Model은 대화·도구 정의·생성 옵션을 받고 ModelReply를 반환한다. 스트림과 취소는 별도 콜백/토큰이다. Model 구현은 서로 다른 실행에서 동시에 호출될 수 있다. 모델이 생성한 도구 ID가 없으면 호스트가 부여하며, 중복·재사용 ID 또는 잘못된 결과 연결은 실행 전에 거부한다.

현재 `ServiceModel`은 `Service::converse`의 구조화 대화 API를 사용한다. `ConversationRequest`는 OpenAI function-call 형식의 텍스트 메시지·도구 정의와 `auto/required/none` toolChoice를 받는다. llama.cpp에 고정된 upstream common의 Jinja 채팅 템플릿, 도구 문법 샘플러, PEG 응답 파서를 사용하며 도구 결과를 user 메시지로 바꾸지 않는다. 미지원 런타임은 RuntimeUnavailable을 반환한다. MLX 네이티브 도구 호출은 아직 미완료이다.

구조화 대화도 기존 Service의 단일 추론 스케줄러, 모델 무결성/메모리 정책, 제한된 KV 캐시를 사용한다. contextId는 모델별로 분리해 재사용하며 빈 ID는 일회성 컨텍스트이다. 대화 기록은 호출자가 소유한다. 잘못된 도구 ID 연결, 알 수 없는 도구, 잘린 호출을 거부한다. 컨텍스트 초과 시 도구 결과를 임의로 잘라내지 않고 ContextOverflow를 반환한다. 현재 텍스트 Delta는 완성된 응답의 파싱 후 전달한다. 토큰/도구 인자/추론 블록의 점진적 스트리밍은 후속 구현 대상이다.

Tool은 정의, 실행 함수, 선택적 도메인 검증, 입력별 동시 실행 판정을 갖는다. JSON Schema 2020-12를 기본 dialect로 사용하며 `$schema`가 있으면 지정 dialect로 검증한다. 네트워크 `$ref` resolver는 설치하지 않는다. 입력 변경 훅 이후에는 스키마와 도메인 검증을 다시 수행한다. 출력 스키마가 있으면 성공 결과의 data를 검증한다.

한 모델/도구 턴은 도구 정의·스키마·실행 함수를 같은 스냅샷으로 고정한다. 실행 도중 registry를 갱신해도 이전 정의와 새로운 실행 함수가 섞이지 않으며 다음 모델 호출부터 갱신 내용을 반영한다. 직접 ToolRunner를 사용할 때도 한 실행의 입력/출력 검증과 실행 함수는 같은 등록 항목에 묶인다.

정책 거부·스키마 실패·도구 오류는 isError 도구 결과로 모델에 전달한다. 취소·소비자 콜백 오류는 실행 자체를 종료한다. 긴 텍스트 결과는 세션 artifact에 보관하고 참조를 반환한다. 파일 쓰기·셸·외부 도구의 부작용이 자동 롤백되는 계약은 아니다.

## 세션·중단·동시성

SessionStore는 `<sessions>/<uuid>/transcript.jsonl`을 사용한다. 첫 줄은 버전·모델·작업 디렉터리를 담고, 이후 메시지는 parent_id로 연결한다. 실행 동안 QLockFile을 유지해 다른 프로세스의 동시 기록도 막는다. 마지막 불완전 레코드는 버리고, 중간 JSON 오류나 연결 불일치는 손상으로 처리한다. 프로세스 중단 복원을 지원하지만 fsync 기반 전원 장애 내구성을 보장하는 것은 아니다.

도구 요청은 실제 실행 전에 기록한다. 재개 시 결과가 기록되지 않은 요청은 `결과 미확인` 오류 결과로 닫으며 자동 재실행하지 않는다. 취소·실패 뒤에도 가능한 경우 남은 도구 요청에 대응 결과를 기록한다. 동일 세션에는 동시에 하나의 실행만 수락한다.

Engine은 제한된 QThreadPool과 수락 대기열을 사용한다. 도구는 입력별 동시 실행 가능 여부에 따라 묶으며 기본 최대 10개이다. 동시 실행 불가 도구는 앞선 묶음의 완료 후 실행한다. 입력을 바꿀 수 있는 훅이 있으면 현재 스케줄러는 도구 실행을 직렬화한다. 이벤트 콜백은 실행별로 직렬화하지만 서로 다른 실행 간에는 동시에 호출될 수 있다.

모델 응답 전체가 반환된 뒤 도구를 스케줄링하는 경로가 현재 구현이다. Claude 분석본의 콘텐츠 블록 완료 시점부터 실행하는 최적화는 별도 미완료 항목이다.

## 권한과 기본 도구

RulePolicy는 Default/AcceptEdits/DontAsk/Bypass/Plan을 제공한다. 명시적 deny가 먼저 적용되고 ask는 allow보다 우선한다. Plan은 읽기 전용 도구만 허용한다. DontAsk는 남은 ask를 deny로 바꾼다. Ask는 호스트의 PermissionCallback이 있어야 허용할 수 있다. 자동 분류기·내용별 세부 규칙·OS 샌드박스는 아직 추가되지 않았다.

`registerWorkspaceTools`는 한 canonical workspace에 Read/Write/Edit/Glob/Grep/Bash를 등록한다. 이 registry를 다른 작업 디렉터리의 세션에 재사용하지 않는다. 파일 작업은 기존 symlink 해석을 거쳐 workspace 밖을 거부하고, 현재 세션의 artifact는 읽기만 허용한다. 경로 검증은 OS 샌드박스나 외부 프로세스의 경로 교체까지 막는 파일 잠금과 다르다.

- Read: UTF-8, 최대 1 MiB, 줄 범위. 완전 읽기 상태와 SHA-256을 기록한다.
- Write/Edit: 기존 파일의 완전 읽기·내용 일치를 요구한다. 변경 전 백업과 QSaveFile 쓰기를 사용한다.
- Glob: 파일 패턴, 최대 1,000개. Git ignore·완전한 globstar 호환은 미완료이다.
- Grep: Qt 정규식, 최대 10,000개 파일·100개 일치, 파일당 1 MiB. rg/LSP 고급 검색은 미완료이다.
- Bash: 작업 디렉터리에서 별도 프로세스 실행, 시간/출력 제한, Unix 프로세스 그룹 취소. OS 샌드박스와 백그라운드 작업 관리, 셸 환경/cwd 지속, Windows 프로세스 트리는 미완료이다.

## 앱에서 사용하는 예

```cpp
#include <agent/Engine.h>

auto tools = std::make_shared<iiLocalLLM::agent::ToolRegistry>();
iiLocalLLM::agent::registerWorkspaceTools(*tools, workspace);
iiLocalLLM::agent::EngineOptions options;
options.sessionsDirectory = sessionsDirectory;
iiLocalLLM::agent::Engine engine(
    std::make_shared<iiLocalLLM::agent::ServiceModel>(service), tools,
    std::make_shared<iiLocalLLM::agent::RulePolicy>(), options);
auto session = engine.createSession("model://my-model", workspace);
auto handle = engine.run({session.id, "Read the project and explain it"}, onEvent);
// UI 스레드에서 future.get()을 호출하지 않는다.
```

앱은 ToolRegistry::add로 자체 기능을 등록할 수 있다. metadata의 app_id 같은 식별자는 앱의 도구 출처를 표현하며 인증을 대신하지 않는다. 실제 앱별 연동, 발견 및 MCP/API 전송은 대응표에서 별도 검증한다.

Service는 ServiceModel과 Engine보다 오래 살아야 한다. Engine 파괴는 수락한 실행을 취소하고 작업 스레드를 join한다. 이벤트·모델·도구 콜백에서 Engine을 파괴하거나 자신의 future를 기다리지 않는다. Qt UI를 갱신할 때는 앱이 자신의 UI 스레드로 이벤트를 전달한다.

## 검증

`iiLocalLLM.agent`는 스키마·정책·훅 재검증·세션 잠금/복원·도구 반복·취소·병렬 실행·턴 제한·파일 변경 감지·셸 제한을 검사한다. `iiLocalLLM.agent_local_inference`는 실제 Qwen GGUF가 Read를 선택하고, 프롬프트에 없는 임의의 파일 값을 최종 답변에 반환하는지 확인한다. 설치된 패키지의 `iiLocalLLM.installed_agent_consumer`는 외부 C++ 프로그램에서 앱 도구·모델·세션 ABI를 검증한다. 각각의 실행 결과는 Verification.md에 별도 기록한다.

Native ServiceModel의 도구 오류는 tool 역할의 `Tool error:` 결과로 전달한다. 기본 llama.cpp 로그에서는 생성 토큰을 포함하는 debug 메시지를 내보내지 않는다. 모든 CTest 임시 디렉터리는 build/tmp 아래에 생성한다.
