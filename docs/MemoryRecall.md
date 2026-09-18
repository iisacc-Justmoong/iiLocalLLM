# 모델 기반 메모리 회상

`src/agent/MemoryRecall.h`는 프로젝트 메모리 주제를 로컬 모델로 선택하고 대화에 연결한다. 기존 Model·Service·ProjectMemory와 Qt를 사용하며 외부 서비스나 새로운 생산 의존성은 추가하지 않는다. 참조는 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `memdir/findRelevantMemories.ts`, `memoryScan.ts`, `memoryAge.ts`, `utils/attachments.ts`와 `query.ts`이다. 원본 소스나 프롬프트를 복사하지 않고 실행 계약을 구현한다.

## 선택과 전달

선택 모델은 현재 질의, 아직 보지 않은 주제의 상대 경로·이름·설명·유형·수정 시각, 최근 성공한 도구 이름을 받는다. 주제 본문과 MEMORY.md는 선택 입력에 넣지 않는다. 기본 모델은 대화의 모델이며 C++ 호스트는 별도 로컬 모델을 지정할 수 있다. 별도 문맥 ID, 도구 없는 JSON Schema 응답, 생각 출력 비활성화와 256 출력 토큰을 사용한다. 선택 모델이 도구를 호출하거나 잘못된 응답을 반환하면 실패 진단을 남긴다.

네이티브 Qwen 형식에서 생각 출력이 꺼져 있고 프롬프트가 이미 `<think>…</think>`를 닫았는데 생성 결과가 다시 `</think>`로 시작하면, 중복된 경계를 upstream 파서의 prefill에 반영한다. 답변 문자열을 사후 치환하지 않으며 본문 중간·코드·JSON의 태그와 생각 출력이 켜진 요청은 이 보정을 적용하지 않는다.

최대 5개를 선택하며 JSON Schema의 enum에도 실제 후보 경로만 열거한다. 후보 목록과 스키마를 합친 입력이 바이트 한도를 넘지 않도록 함께 제한한다. 모델이 만든 경로는 호스트의 후보 목록과 다시 대조하므로 스키마를 따르지 않는 Model 구현에서도 중복과 목록에 없는 경로를 제거한다. 선택한 파일은 실제 전달 시점에 SHA-256을 다시 검사한다. 변경·삭제·링크 교체가 있으면 해당 파일을 생략하고 진단한다. 선택에 실패해도 본 대화의 답변을 실패로 바꾸지 않는다.

기본 파일당 한도는 200줄·UTF-8 4,096바이트이다. 문자 경계를 지켜 자르고 잘림을 모델에 전달하는 본문과 메타데이터에 표시한다. 현재 모델 문맥에 있는 회상 메시지의 전체 크기는 안내 문구까지 포함하여 60KiB로 제한한다. 이 예산은 압축 후 실제로 남은 메시지를 기준으로 다시 계산한다. 2일 이상 된 메모에는 바뀔 수 있는 사실을 현재 증거로 확인하라는 문구를 붙인다. 저장된 문구는 다시 렌더링할 때 날짜에 따라 바뀌지 않는다.

회상은 user 역할의 과거 자료로 전달하며 새 사용자 입력이나 권한으로 승격하지 않는다. 원래 사용자 입력을 압축 시 보존할 때도 회상 메시지를 새 요구사항으로 고르지 않는다. 전체 내용을 전달한 경우 기존 native Read의 관측을 기록하므로 같은 세션·문맥 세대에서 Edit할 수 있다. 부분 전달은 읽기 완료로 인정하지 않는다. 실제 변경 권한·훅·현재 해시 검사는 기존 파일 도구가 그대로 적용한다.

## 비동기 수명

프로젝트 메모리가 활성화된 Engine은 새 사용자 입력마다 회상을 선행 실행한다. 한 단어뿐인 입력과 알림만 전달한 실행은 자동 검색을 시작하지 않는다. 모델 호출 경계에서 결과가 준비된 경우에만 회수하므로 미완료 선택을 기다리며 본 모델·도구 흐름을 멈추지 않는다. 이미 전달되었거나 성공한 파일 도구에서 관측한 경로는 선택과 전달 양쪽에서 중복 제거한다.

기본 제한 시간은 30초이다. 새 입력·취소·대화 종료 시 미사용 작업에 취소를 전달하고 합류한다. Model 인터페이스의 협조적 취소 계약을 따르며 독립 프로세스를 강제 종료하는 제한 시간이 아니다. 동일 Service의 로컬 모델 스케줄러는 선택과 본 모델 요청을 직렬화할 수 있으므로 부하·대기 시간은 공유한다. 짧은 대화가 선택보다 먼저 종료되면 결과를 늦게 transcript에 넣지 않는다.

`memory_recall` 이벤트는 선택·전달·생략·진단을 제공한다. 선택 사용량은 RunUsage의 `memoryRecallPromptTokens`, `memoryRecallGeneratedTokens`, `memoryRecallCachedTokens`와 JSON의 `memory_recall_*_tokens`로 분리한다. 일반 응답의 토큰 필드와 합산하려면 호스트가 두 범위를 더해야 한다. 취소된 모델 호출이 사용량을 반환하지 않으면 그 미보고 토큰 수는 확정하지 않는다.

## C++·API·MCP·CLI

`EngineOptions.memoryRecall`은 제한과 선택 모델을 설정한다. `memoryRecall.enabled=false`는 자동·명시적 모델 회상을 끄며 MEMORY.md 인덱스와 기존 메모리 파일 도구를 유지한다. `projectMemoryEnabled=false`이면 회상도 비활성화된다.

`Engine::recallMemory(sessionId, query)`와 인증 API `agent.memory.recall(session_id, query)`는 선택 결과·진단·usage·notes를 반환한다. 명시적 호출은 대화 기록을 변경하지 않는다. API는 기존 client ID별 프로젝트 저장소를 사용하며 다른 소유자의 세션 ID를 거부한다. MCP `iiLocalLLM.agent.memory.recall`은 query만 받고 연결 소유자의 세션을 사용한다. `iisacc/projectMemory.recallTool`에서 기능을 발견할 수 있다.

CLI는 `iillm --auth-file TOKEN_FILE agent memory recall SESSION PARAMS_JSON_FILE`을 사용한다. 파라미터 파일은 `{"query":"관련 프로젝트 질문"}`이다. daemon은 `--agent-no-memory-recall`, `--agent-memory-recall-model`을, agent-enabled MCP는 `--no-memory-recall`, `--memory-recall-model`을 제공한다. 메모리 설정을 일반 모델 인자나 원격 요청으로 변경할 수 없다.

## 범위

이 기능은 모델 기반 주제 회상과 자동 문맥 전달이다. 세션 기록 검색, 턴 종료 추출, dream 정리, Git worktree 공유, 전문 에이전트별 메모리와 원격 동기화는 별도 미완료 요구사항이다. macOS 외 플랫폼과 모든 앱의 회상 표시·설정 화면은 별도 검증이 필요하다. 실제 검증과 설치 증거는 Verification.md에 기록한다.

턴 종료 시의 별도 저장 작업은 0.39 [MemoryExtraction.md](MemoryExtraction.md)에 구현 계약과 남은 범위를 기록한다.
