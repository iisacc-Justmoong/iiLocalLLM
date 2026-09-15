# 앱의 사용자 질문 화면

0.36.0은 C++ `agent::QuestionInbox`와 LVRS `UserQuestionsSheet.qml`을 제공한다. 기존 `AskUserQuestion`·`PermissionRequests`·Qt를 재사용하며 SDK 라이브러리에 Qt Quick 의존성을 추가하지 않는다. QML 화면을 사용하는 앱만 LVRS와 Qt Quick을 연결한다. 새 생산 라이브러리는 도입하지 않았다.

## C++ 연결

```cpp
#include <agent/QuestionInbox.h>
#include <agent/UserQuestions.h>

auto* inbox = new iiLocalLLM::agent::QuestionInbox(iiLocalLLM::agent::PermissionRequestsOptions{}, uiLifetime);
registry->add(iiLocalLLM::agent::userQuestionTool({false, "markdown"}));
toolOptions.hooks.append(inbox->hook());
```

두 생성자 중 하나를 명시하려면 첫 인수를 `PermissionRequestsOptions{}`로 작성한다. 새 채널을 만드는 생성자는 소멸 시 대기 요청을 닫는다. 기존 `shared_ptr<PermissionRequests>`를 전달하면 채널을 빌려 쓰므로 뷰 소멸만으로 채널을 닫지 않는다. 명시적인 `close()`는 두 경우 모두 대기를 취소한다. 앱 종료 시 서버의 작업 스레드를 기다리기 **전에** 수신부를 닫는다.

생성·읽기·`refresh`·`submit`·`reject`·`close`는 QObject의 GUI 스레드에서 수행한다. `hook()`이 반환한 콜백은 채널만 캡처하므로 작업 스레드에서 사용할 수 있다. 콜백은 native `AskUserQuestion`의 PermissionRequest와 `builtin.user-question` 출처만 처리한다. GUI 스레드에서 ToolRunner를 동기 실행하면 질문 답변을 처리할 수 없으므로 금한다.

`requests`는 도착 순서의 대기 요청 QVariantList이며 `errorString`은 입력 실패 이유이다. 100ms 타이머는 실제 JSON 스냅샷이 변경되었을 때만 `requestsChanged`를 발생시킨다. `submit(id, answers, annotations)`는 질문 원문을 키로 받는다. C++에서 원래 질문을 보존하고 스키마·주석·답변을 검증한 뒤 요청을 확정한다. 잘못된 답변은 요청을 소비하지 않는다. `reject(id, reason)`는 도구 실패를 돌려준다. 빈 answers는 건너뛰기이다. 취소·만료·연결 종료 후 늦은 응답은 실행을 재개하지 않는다.

기본 채널은 120초, 64개 요청, 총 8MiB 대기 한도를 갖는다. MCP 서버의 기본 요청 기한은 60초이며 클라이언트가 더 짧은 기한을 설정할 수 있다. 실제 대기는 가장 먼저 도달한 기한이나 취소에 종료된다. 앱 통합은 연결별 호출 취소를 전달하지만 질문 목록은 로컬 UI에 통합해서 보여 준다. 외부 MCP 호출자는 다른 요청을 조회하거나 UI 답변을 제출하는 도구를 받지 않는다.

## LVRS 화면 설치

CMake 패키지는 `IILOCALLLM_QML_DIR`을 설치 prefix의 `share/iiLocalLLM/qml`로 제공한다. 앱이 `UserQuestionsSheet.qml`을 Qt 리소스에 포함하면 SDK 원본 폴더 없이 실행할 수 있다. `Loader.setSource(url, {inbox: inboxObject})`로 required 속성을 생성 시 전달한다. SDK는 QML 플러그인을 새로 등록하지 않는다.

화면은 단일·다중 선택, 여러 줄 자유 입력, 여러 줄 메모, 선택지 미리보기, 부분 응답, 건너뛰기, 거절·Escape를 제공한다. 다음 요청이 도착해도 현재 답변과 포커스를 유지하며 현재 요청이 끝나면 다음 요청으로 이동한다. 질문 원문이 `__proto__`여도 일반 데이터 키로 보존한다. 모든 외부 텍스트와 미리보기는 PlainText로 표시한다. Markdown/HTML 서식 렌더링, 이미지 첨부, 인터뷰 모드, 전체 채팅 UI는 구현하지 않았다.

## MCP 앱 계약

Society와 Dreamscapes의 POSIX 데스크톱 빌드는 같은 화면을 리소스로 포함한다. `AskUserQuestion`은 `experimental.iisacc/userQuestions.responseChannel=local-ui`로 로컬 답변 경로를 알린다. `previewFormat=markdown`은 허용 입력이며 `previewRendering=plain-text`는 현재 표시 방법이다. 일반 앱 도구의 허용 규칙은 유지한다. native 질문은 전역 도구 실행 잠금을 점유하지 않아 기다리는 동안 탐색·갱신 같은 일반 호출이 계속 실행된다.

서버 도구의 `_meta["iisacc/userInteraction"]=true`는 가져오기 후 `requires_user_interaction`으로 전달한다. 이 표시는 권한을 허용하지 않으며 원격 이름으로 가져온 질문도 자동 검증 agent 훅의 도구 목록과 실행 경로에서 제외하는 데 사용한다. 표시하지 않은 임의의 외부 도구까지 대화형인지 판별하는 기능은 아니다.

## 검사

`iiLocalLLM.question_inbox`는 작업 스레드와 UI 응답, 입력 실패 후 재시도, 복수 소유자, 취소·만료·소멸, 빌린 채널 수명, MCP 질문 대기 중 앱 변경을 검사한다. `-DIILOCALLLM_TEST_LVRS_DIR=<installed-prefix>`를 설정하면 실제 LVRS 컴포넌트를 사용하는 `iiLocalLLM.question_sheet`를 추가한다. 마우스·키보드·입력 이벤트로 선택·한글 여러 줄 입력·메모·미리보기·대기열·포커스·취소·Escape를 검사한다. Qt 오프스크린 검사는 물리 모바일 기기나 실제 사용자의 앱 설치 증거와 구별한다.

실제 앱 모델 검사는 `iiLocalLLMUserQuestionsRuntimeSmoke <catalog> <model-uri> <private-endpoint-directory> <app-id>`로 실행한다. 호스트가 발견된 질문 도구 하나를 선택하도록 제한한 상태에서 네이티브 모델이 질문을 만들고, 로컬 UI에 입력한 검사용 코드를 다음 턴에서 소비하는지 확인한다. 검사용 코드는 콘솔의 `question_ui_ready`에만 출력하며 모델 입력에는 넣지 않는다. 이 검사는 자율적인 도구 선택 전체의 증거가 아니다.

공개 실행 파일의 표시 버전은 CMake `PROJECT_VERSION`을 공유한다. `iiLocalLLM.executable_versions`는 iillm·iiLocalLLMD·호환 서비스 실행 파일과 지원 플랫폼의 iillm-mcp를 실제 실행하여 `--version`이 프로젝트 버전과 일치하는지 검사한다.
