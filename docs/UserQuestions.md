# 사용자 질문과 호스트 응답

0.35.0은 C++ `AskUserQuestion`과 Engine·인증 API·IPC·MCP 호출을 제공한다. 기존 `ToolRunner`, JSON Schema 검증, `PermissionRequests`를 재사용하며 생산 의존성을 추가하지 않는다. Python은 전송 시험과 공식 MCP 클라이언트 시험에만 사용한다. 공개 EngineOptions가 추가되므로 ABI 0.35 헤더와 라이브러리로 소비자를 함께 다시 빌드한다.

## 사용

임베디드 호스트는 `EngineOptions.userQuestionsEnabled=true`로 켠다. `userQuestions.deferred`는 기본 true이며 ToolSearch로 공개한다. `agent::userQuestionTool(UserQuestionOptions)`을 독립 ToolRegistry에 등록할 수도 있다. `Engine::userQuestionTool()`은 사용 가능할 때 실제 도구를 반환하고, `runQuestionTool(sessionId, arguments, ...)`은 호스트가 소유 세션에 질문을 요청한다. 직접 호출은 모델 턴을 생성하거나 transcript에 메시지를 추가하지 않는다. 모델 호출은 정상 도구 결과와 함께 대화에 보관한다.

데몬과 모델을 설정한 MCP CLI는 기본으로 켠다. `--agent-no-user-questions`·`--no-user-questions`로 끄며 `--agent-question-preview html`·`--question-preview html`로 미리보기 형식을 선택한다. 기본 형식은 markdown이다. 실제 답은 기존 C++ PermissionResponse 콜백, PermissionRequest 훅 또는 `--agent-permission-requests`·`--permission-requests` 채널에서 받는다. 응답 처리기가 없으면 도구는 거부되고 답을 만들지 않는다. 검증 에이전트에는 질문 도구를 제공하지 않으며 하위 Engine은 이 자동 등록 옵션을 상속하지 않는다.

| 경로 | 요청/인식 | 응답 |
|---|---|---|
| C++ | `Engine::runQuestionTool` 또는 모델의 `AskUserQuestion` | `PermissionResponse.updatedArguments` |
| API·IPC | `agent.questions.ask`, `agent.info.user_questions_enabled` | `agent.permissions.pending/respond` |
| MCP stdio·HTTP | `AskUserQuestion`, `capabilities.experimental["iisacc/userQuestions"]` | `iisacc/permissions/pending`, `iisacc/permissions/respond` |

API 호출은 질문 객체에 `session_id`를 추가한다. MCP 호출에서는 소유자를 연결의 실제 Engine 세션으로 결정한다. 질문은 일반 실행 용량을 사용하며 답변·취소에는 기존 예약 제어 경로를 사용한다. MCP 표준 elicitation과 이 확장 채널은 별도 계약이다.

```json
{
  "questions": [{
    "question": "어떤 UI를 사용할까요?",
    "header": "UI",
    "options": [
      {"label": "Qt", "description": "네이티브 앱", "preview": "**Qt**"},
      {"label": "Web", "description": "브라우저"}
    ],
    "multiSelect": false
  }],
  "metadata": {"source": "society.preferences"}
}
```

호스트는 PermissionRequested 이벤트 또는 pending 결과의 `request.input`을 표시한다. 표시용 계약은 `request.permission_preview._meta.user_question`에 있으며 원래 입력, 미리보기 형식, 자유 입력·부분 응답 허용 여부, 바이트 한도를 포함한다. `updatedInput`에는 **같은 원래 질문과 metadata를 그대로 유지하고** 다음 두 필드를 추가한 전체 객체를 보낸다.

```json
{
  "answers": {"어떤 UI를 사용할까요?": "Qt"},
  "annotations": {"어떤 UI를 사용할까요?": {"preview": "**Qt**", "notes": "사용자 메모"}}
}
```

이 예시는 추가하는 필드만 나타낸다. 전체 응답은 `{ "request_id": "...", "decision": { "behavior": "allow", "updatedInput": { "questions": [...], "metadata": {...}, "answers": {...}, "annotations": {...} } }`이다. 인증 토큰은 모델에게 전달하지 않는다. `iillm --socket <socket> --auth-file <file> rpc agent.permissions.respond <response.json>`도 같은 계약을 사용한다.

## 입력·결과 계약

질문은 1~4개, 질문별 선택지는 2~4개이다. 질문 문자열은 배치 안에서, 선택지 label은 같은 질문 안에서 유일해야 한다. `multiSelect` 생략은 false로 해석한다. 호스트 UI는 자유 입력을 별도로 제공하며 모델은 Other 선택지를 만들 필요가 없다. 복수 선택 답은 참조 계약처럼 문자열이다. 예를 들어 `"Qt, Web"`이며 서버는 이를 재분해하거나 선택지와 일치하도록 강제하지 않는다. 사용자 자유 입력을 보존하기 위함이다. 생략한 질문은 미응답이며 전체 또는 부분 건너뛰기를 허용한다. Allow만 받으면 빈 answers를 반환한다.

`answers`와 선택적 `annotations`는 질문 원문을 키로 삼는다. annotations의 preview·notes는 호스트가 선택한 미리보기·메모이며 자동 추측하지 않는다. 알려지지 않은 질문 키, 바뀐 질문·선택지·metadata, 잘못된 타입은 거부한다. 초기 모델·API·MCP 입력과 PreToolUse 수정은 answers·annotations를 지정할 수 없다. PermissionRequest 훅/콜백/인증 앱의 응답은 신뢰하는 호스트 입력이다. 이 경계만으로 사람이 실제로 응답했는지를 증명하지는 않는다.

결과 객체는 questions·answers·선택적 annotations이다. metadata는 출력에서 제외한다. 텍스트 결과에도 동일한 내용을 JSON으로 포함하고 미응답을 구분한다. 큰 텍스트는 기존 ToolRunner 결과 보관·축약 계약을 따른다.

| 제한 | 값 |
|---|---|
| 전체 입력 | compact JSON UTF-8 262,144바이트 |
| 질문·선택지 설명 | 각 8,192문자 |
| header / label / metadata.source | 128 / 512 / 512문자 |
| 미리보기·답·메모 | 각각 32,768문자 |
| 요청 수·대기 시간·페이지·응답 크기 | 기존 PermissionRequests 호스트 설정 |

header 12문자는 표시 권고이며 강제 제한은 128이다. 빈 질문·header·label, NUL을 거부한다. HTML 모드는 태그가 있는 조각인지 검사하고 html/body/doctype/script/style 태그를 거부한다. 이는 참조의 의도 검사이며 **HTML 정화기가 아니다**. 이벤트 속성·URL 등은 별도 정화가 필요하고 앱은 원문을 그대로 innerHTML에 넣어서는 안 된다. Markdown도 호스트 렌더러의 안전한 처리가 필요하다. 이 SDK는 HTML·Markdown을 실행하거나 렌더링하지 않는다.

## 수명·권한·남은 범위

읽기 전용·동시 실행 가능 도구지만 Bypass에서도 실제 호스트 응답을 요구한다. 명시 Deny와 DontAsk를 유지하고, 계획 모드 중 답변은 계획 승인으로 취급하지 않는다. ExitPlanMode의 검토 전환은 계속 별도 도구이다. 채널 취소·만료·세션 종료는 대기를 끝내며 뒤늦은 응답으로 완료할 수 없다. 동일 응답 재전송은 멱등이고 충돌한 재전송은 AlreadyExists이다. `accepted=true`는 채널 수신 확인이므로 최종 도구 성공 여부도 확인해야 한다. 질문을 바꾼 호스트 응답은 수신 뒤 도구 재검증에서 거부될 수 있다.

참조는 로컬로 고정한 `Exhen/claude-code-2.1.88`의 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc` 중 `source/src/tools/AskUserQuestionTool/AskUserQuestionTool.tsx`, `source/src/components/permissions/AskUserQuestionPermissionRequest/AskUserQuestionPermissionRequest.tsx`이다. 저장소 유출 주장의 진위는 독립적으로 확인하지 않았다. TypeScript 소스나 시스템 프롬프트를 복사하지 않고 관측한 계약을 C++로 작성했다. 원본보다 엄격한 초기 답변 금지·질문 변경 금지·크기 제한을 적용한다.

React/터미널 질문 UI, 이미지 첨부, 계획 인터뷰의 중단·피드백 흐름, 자동 선택 미리보기 조합, 채널 환경별 UI 활성화와 Society·Dreamscapes·Congregation·Thinking Space의 실제 질문 화면은 남아 있다. 이번 단계는 SDK와 전송·모델 소비 검증이며 앱을 다시 패키징하지 않는다. 전체 하네스 상태는 [HarnessParity.md](HarnessParity.md), 실행 증거는 [Verification.md](Verification.md)를 따른다.

0.36.0의 C++ 수신부와 LVRS 앱 화면은 [QuestionUI.md](QuestionUI.md)에 설명한다. 텍스트 미리보기만 표시하며 이미지 첨부와 전체 인터뷰 UI는 남아 있다.
