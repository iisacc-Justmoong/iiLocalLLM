# 네이티브 모델의 추론 모드 제어 (0.12.1)

llama.cpp 모델 로딩의 `options.enable_thinking`은 boolean이다. 생략 시 기존 동작을 유지하며 구조화 대화의 기본값은 true이다. 명시한 true/false는 고정된 llama.cpp의 Jinja 템플릿 입력 `enable_thinking`에 전달한다. 템플릿이 이 입력을 지원해야 하며, 모든 모델이 추론 모드를 전환할 수 있다는 뜻은 아니다. MLX의 모델 로딩 옵션은 현재 이 설정을 지원하지 않으며 요청을 거절한다.

```cpp
iiLocalLLM::ModelLoadRequest request{"model://qwen3-8b-q4", 8192};
request.options["enable_thinking"] = false;
request.options["tool_grammar"] = false;
service.loadModel(request).get();
```

같은 모델이 이미 로드되어 있으면 먼저 unload한 뒤 변경된 옵션으로 다시 로드한다. `ModelInfo.options`에서 수락된 설정을 확인한다. daemon의 기존 `models.load` RPC에도 `options` 객체로 전달할 수 있다. 도구 문법 제어와 추론 모드 제어는 독립적이다. 어떤 조합에서도 도구 파싱·실행 전 스키마·권한 검사는 적용한다.

명시적 추론 모드 설정은 일반 text/chat과 구조화 conversation 양쪽에 적용한다. 일반 텍스트 경로는 이 설정이 있을 때 동일한 Jinja 템플릿으로 프롬프트를 구성한다. 옵션을 생략한 일반 텍스트 경로는 기존 formatter를 유지한다. 구조화 대화의 예산 측정과 생성은 같은 설정을 사용한다. 원래 모델 템플릿이나 가중치를 수정하지 않는다.

Qwen3의 `/no_think`는 모델에 전달하는 소프트 지시이며 닫는 `</think>`를 포함한 구간이 여전히 필요하다. `enable_thinking=false`는 템플릿 단계에서 선택하는 별도의 제어이다. [Qwen3 공식 모델 문서](https://huggingface.co/Qwen/Qwen3-8B)의 두 모드를 구분한다. 현재 의존성은 [llama.cpp 고정 커밋의 템플릿 입력](https://github.com/ggml-org/llama.cpp/blob/5202104b59ada9005db079eea43882a2b7bf5802/common/chat.h)이며 새 의존성이나 Python 추론 단계를 추가하지 않았다.

관측한 Qwen3 8B의 지연 도구 검색 실패에서는 모델이 `<think>` 뒤 `</think>`를 닫지 않고 TaskGet 호출을 생성했다. 네이티브 파서는 이를 reasoning으로 분류했다. ServiceModel은 reasoning만 있고 최종 답이나 실행 가능한 호출이 없으면 `protocol_error`로 거절하며 이 경우와 완전히 빈 응답을 구분한다. 추론 텍스트 안에 적힌 도구를 실행하거나 누락된 경계를 자동 삽입하지 않는다.

`tests/reasoning_runtime_smoke.cpp`는 실제 llama 런타임과 제어용 Jinja 템플릿으로 타입·기본값·true/false 및 두 프롬프트 경로를 검증한다. `tests/service_tests.cpp`는 추론 안에 도구 형태의 문자열이 있어도 실행 가능한 호출로 바뀌지 않는지 검증한다. `IILOCALLLM_TEST_AGENT_THINKING_CONTROL=ON`은 기존 카탈로그 수락 검사에 `enable_thinking=false`를 적용한 별도 지연 공개 검사를 추가한다. 기존 기본 설정의 검사를 대체하지 않는다.

공식 MCP Python 클라이언트 환경이 함께 설정되면 `tests/agent_runtime_smoke.cpp`의 카탈로그 검사도 추가한다. 같은 Qwen3 샘플링과 추론/문법 설정에서 MCP 도구를 검색·호출하고 고정 회귀 값과 새 임의 파일 값을 실제 결과로 읽는지 확인한다. 각 대화의 검색 성공, 실제 원격 호출, 진행 이벤트, 최종 값과 호출/결과 쌍을 모두 요구한다. 실제 결과와 모델 조건은 [Verification.md](Verification.md)에 기록한다.
