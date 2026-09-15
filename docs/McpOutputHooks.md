# MCP 도구 결과 변경 훅

0.32.0은 `PostToolUse`의 `hookSpecificOutput.updatedMCPToolOutput`을 C++ 도구 실행 경로에 연결한다. 명령·HTTP 훅 및 C++ `HookResult`가 성공한 MCP 도구의 모델 관측을 교체할 수 있다. 새 런타임 의존성 없이 기존 MCP 콘텐츠 검증기와 훅 실행기를 재사용한다.

## 설정과 결과

```json
{
  "hooks": {
    "PostToolUse": [{
      "matcher": "mcp__society__.*",
      "hooks": [{"type": "http", "url": "http://127.0.0.1:9042/observations"}]
    }]
  }
}
```

위 HTTP 주소는 설정 예시이다. 호스트가 운영하는 엔드포인트는 기존 `tool_response`의 `text`, `data`, `content`를 JSON POST로 받고 다음과 같이 응답한다. 명령 훅도 같은 입력을 stdin으로 받고 같은 JSON을 stdout으로 출력한다.

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PostToolUse",
    "updatedMCPToolOutput": "선택된 자산은 3개이다.",
    "additionalContext": "이 결과는 현재 선택 범위에 한정된다."
  },
  "suppressOutput": true
}
```

문자열은 MCP `text` 블록 한 개로 바뀐다. MCP 콘텐츠 블록 배열도 허용한다. `text`, `image`, `audio`, `resource`, `resource_link`의 필수 필드는 기존 MCP 검증기로 검사한다. 미디어를 텍스트로 조용히 버리지 않으며, 이후 모델 어댑터가 미디어를 지원하는지는 별도 계약이다. 빈 배열 `[]`은 관측을 비운다. 참조 구현의 truthiness와 같이 빈 문자열·null·false·0은 변경하지 않는다. true·0 이외 숫자·객체·잘못된 콘텐츠 블록은 비차단 오류로 기록하고 유효한 결과를 보존한다. C++ 콜백에도 같은 검사를 적용한다.

문자열이나 배열을 적용할 때 원래 `ToolResult.text`, `data`, `content`를 함께 교체한다. 원래 `structuredContent`가 다른 모델 입력이나 MCP 재전달에 다시 나타나지 않도록 `data`는 빈 객체로 만든다. 이전 훅의 `additionalContext`와 현재 피드백은 텍스트에 보존한다. 원래 `_meta`는 호스트·앱용 메타데이터로 그대로 유지하며, 이 필드로 `_meta`·오류 상태·권한·도구 실행 제어 정보를 만들 수 없다. C++ 사용자 정의 Model에는 기존 Message.metadata 계약이 유지된다.

이는 관측 교체 기능이다. 원래 요청 인자, 훅 입력, 이미 발생한 진행 이벤트, 훅 진단, 원래 `_meta`, 외부 도구의 부작용까지 지우거나 되돌리는 기능은 아니다. 모든 원본 데이터의 삭제나 비밀정보 제거를 보장하지 않는다.

## 실행 순서와 경계

1. 입력·권한 검사를 거쳐 MCP 서버를 호출한다. 서버 응답의 MCP 콘텐츠와 원래 `outputSchema`를 검증한다.
2. 성공한 가져오기 MCP 도구에 `PostToolUse`를 실행한다. `mcpTools()`가 설정하는 C++ `Tool.isMcp`로 구분한다. 이름 접두사나 원격 `_meta`, 일반 도구의 공개 metadata만으로 이 구분을 만들 수 없다.
3. 유효한 변경 결과를 적용한 뒤 `ToolFinished`, 영구 transcript, 다음 모델 호출 및 API/MCP 응답을 구성한다. 실패·권한 거부·입력/출력 스키마 오류를 성공 결과로 바꾸지 않는다. `continue:false`와 취소는 기존 중단 경로를 따른다.

같은 CommandHooks 실행기에서 일치한 명령·HTTP 훅은 같은 입력으로 병렬 실행한다. 마지막으로 완료한 **유효한** 변경을 사용한다. 잘못된 결과나 값이 없는 후속 훅은 이전 변경을 지우지 않는다. 차단·중단과 피드백 병합은 기존 규칙이다. 호스트가 등록한 독립 C++ Hook 콜백은 순서대로 실행하므로 다음 콜백은 변경된 결과를 받는다. 콜백이 오류 상태로 바꾸면 후속 명령 콜백은 기존 동작대로 `PostToolUseFailure`를 선택한다.

MCP 도구를 다시 MCP 서버로 내보낼 때, 훅이 설정된 가져오기 도구는 `tools/list`에서 원격 `outputSchema`를 생략한다. 출력이 문자열/블록 관측으로 바뀔 수 있어 원래 구조화 결과 스키마를 외부 클라이언트에 보장할 수 없기 때문이다. 원격 서버가 실제 반환한 구조화 결과의 내부 검증은 계속 수행한다. 훅이 없는 가져오기 도구와 일반 도구의 출력 스키마는 유지한다.

## 참조 대조와 검증 범위

참조는 `Exhen/claude-code-2.1.88`의 고정 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`를 읽었다. 외부 하네스 코드를 실행하거나 런타임 의존성으로 포함하지 않았다.

| 참조 위치 | 확인한 동작 |
|---|---|
| `source/src/types/hooks.ts`, PostToolUse 스키마 | 선택 `updatedMCPToolOutput` 필드 |
| `source/src/utils/hooks.ts`, JSON 결과 처리와 훅 결과 생성 | truthy 값만 전달 |
| `source/src/services/tools/toolHooks.ts`, runPostToolUseHooks | MCP 도구에만 결과 변경 전달 |
| `source/src/services/tools/toolExecution.ts`, PostToolUse 처리 | 변경된 MCP 관측으로 tool_result 구성 |
| `source/src/utils/mcpValidation.ts`, MCPToolResult | 문자열 또는 공급자 콘텐츠 블록 배열 |

참조 스키마의 `unknown`을 그대로 모든 JSON 허용으로 옮기지는 않았다. iiLocalLLM은 문자열과 MCP 프로토콜 콘텐츠 배열을 검증하며, Anthropic 전용 이미지/source·tool_use 블록이나 임의 객체는 허용하지 않는다. 구조화 관측을 바꾸려면 JSON을 text 블록에 직렬화한다. 원격 `_meta` 유지와 C++ 호스트 콘텐츠 계약을 구분한다.

회귀 테스트는 `tests/mcp_output_hooks_tests.cpp`에 둔다. 실제 모델·설치본·전체 테스트의 실행 결과는 [Verification.md](Verification.md)에 기록한다. 이 기능의 검증은 전체 하네스, Society/Dreamscapes의 실제 앱 UI, 다른 운영체제의 완료를 뜻하지 않는다.
