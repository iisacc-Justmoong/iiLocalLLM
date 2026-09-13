# LLM 제어 객체

0.3.0은 추론, 모델 로딩, 메모리·하드웨어·서빙, 사전학습·지도학습, 파인튜닝, 양자화, 선호도 학습의 설정을 버전이 있는 객체로 표현한다. 2026-09-13에 고정한 13개 외부 프로젝트와 iiLocalLLM 자체 설정을 포함한다. 현재 391개 그룹, 상속을 포함한 9,183개 필드, 중복을 제외한 4,610개 선언이다. 수치와 각 파일의 범위는 `catalog/parameter-coverage.json`, 커밋·파일 해시는 `catalog/parameter-sources.json`에 기록한다.

이 수치는 전 세계의 모든 독점 모델·플러그인·미래 버전의 설정을 열거했다는 뜻이 아니다. 조사 대상은 잠근 공식 소스의 설정 클래스·구조체, 선택한 함수·CLI 인자와 상속이다. 런타임에 생성되는 선택값, 임의 `kwargs`, 모델별 아키텍처, 환경변수 전체는 정적 열거의 보장 범위에 포함하지 않는다. 새 제공자의 정의는 `ParameterCatalog::fromJson()`으로 가져올 수 있다. 출처와 범위를 보존하는 객체 모델이므로 기존 필드명과 의미를 바꾸지 않고 새 카탈로그를 사용한다.

## 객체와 데이터 계약

| 객체 | 역할 |
| --- | --- |
| `ParameterCatalog` | 내장 정의 또는 외부 JSON 정의의 불변 스냅샷. 그룹·필드 조회, 타입·선언 제약 검증 |
| `ParameterGroupDefinition` | 원본 namespace/class, 설명, 단계, 기반 클래스, 상속을 펼친 필드 목록 |
| `ParameterDefinition` | 원본 이름·타입·설명, JSON 스키마, 기본값/기본값 표현식, 필수 여부, 읽기 전용·민감 필드, 실행 바인딩, 소스 증거 |
| `ParameterObject` | 한 설정 그룹의 값. 검증 후 수정, 기본값 조회, 원본 필드명의 JSON 가져오기/내보내기 |
| `ControlParameters` | 여러 제공자의 설정 객체를 `schema_version: 1` 문서에 함께 저장 |

`ParameterPhase`는 generation/loading/training/fine_tuning/quantization/serving/infrastructure이다. 종류는 boolean/integer/number/string/array/object/union/enum/null/external/opaque로 구분한다. 수치 범위, 선택값, nullable, 배열 원소와 튜플 길이, 중첩 객체 참조, 필수 필드, 문자열 길이·패턴을 검증한다. 알려지지 않은 필드와 지원하지 않는 스키마 키는 오류다.

`external`은 Tensor·Callable·Optimizer처럼 실행 중인 외부 객체가 필요한 타입이다. JSON 값으로 가장하여 전달할 수 없다. `opaque`는 원본이 Any 등으로 열려 있거나 정적으로 타입을 복원하지 못한 값이며 JSON 표현을 보존한다. Opaque 필드 목록은 coverage 파일에 공개한다. 타입 주석과 Field/Arg 메타데이터에 선언된 제약을 검사하며, 원본 라이브러리의 임의 Python 검증 코드나 하드웨어 조건을 실행하지 않는다. 실제 학습·서빙 전에 해당 도구의 검증이 이어져야 한다.

값의 세 상태를 구분한다.

- unset: 사용자가 지정하지 않음. `isSet()`은 false이고 기본 내보내기에 나타나지 않는다.
- null: 원본이 허용한 명시적 null. 기본값 자동 선택 등의 의미를 원본 도구에 맡긴다.
- default: `value()`가 반환하는 선언의 리터럴 기본값. 함수 호출·환경 의존 기본값은 `defaultExpression`으로 보관하며 평가하지 않는다.

`toNativeJson(true)`는 리터럴 기본값만 추가한다. 원본의 `__post_init__`를 실행한 최종 설정이 아니며 학습 기기, 경로, 분산 환경 등을 자동 결정하지 않는다. `set()`은 필드 단위 검증을 통과해야 값을 바꾸고, 전체 가져오기·내보내기는 교차 조건도 검사한다. 필수 필드 검사는 `validate(true)`로 요청한다. `fp16`과 `bf16` 동시 활성화는 Transformers/TRL 객체에서 거부한다. 실수 setter는 NaN/Infinity를 JSON null로 바꾸기 전에 거부한다.

객체는 값으로 복사된다. `ControlParameters::object()`로 가져온 복사본을 수정한 후 `set()`으로 다시 넣어야 컬렉션이 바뀐다. 동시 읽기는 불변 카탈로그를 공유하며, 같은 값 객체를 여러 스레드에서 수정할 때는 호출자가 동기화한다.

## 학습·파인튜닝 예제

```cpp
#include <iiLocalLLM.h>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

using namespace iiLocalLLM;
ParameterObject training("transformers.TrainingArguments");
training.set("output_dir", "build/training-output");
training.set("learning_rate", 0.0002);
training.set("per_device_train_batch_size", 2);
training.set("gradient_accumulation_steps", 8);
training.set("bf16", true);

ParameterObject lora("peft.LoraConfig");
lora.set("r", 16);
lora.set("lora_alpha", 32);
lora.set("target_modules", QJsonArray{"q_proj", "v_proj"});

ControlParameters settings;
settings.set(training);
settings.set(lora);
const auto document = settings.toJson();
const auto restored = ControlParameters::fromJson(document);
const auto trainerArguments = restored.object("transformers.TrainingArguments").toNativeJson();
const auto loraArguments = restored.object("peft.LoraConfig").toNativeJson();
```

저장 형식:

```json
{
  "schema_version": 1,
  "objects": {
    "transformers.TrainingArguments": {"learning_rate": 0.0002, "per_device_train_batch_size": 2},
    "peft.LoraConfig": {"r": 16, "lora_alpha": 32},
    "trl.GRPOConfig": {"beta": 0.04, "num_generations": 4}
  }
}
```

학습 객체는 원본 도구에 전달할 설정의 생성·검증·직렬화를 제공한다. iiLocalLLM이 역전파, 데이터셋 전처리, 옵티마이저 실행 또는 학습 작업 실행을 수행하지는 않는다. Python 코드에서 검증된 JSON을 읽고 `TrainingArguments(**values)`나 `LoraConfig(**values)`에 전달할 수 있다. Tensor·콜백·모델·옵티마이저 인스턴스는 실행 애플리케이션에서 별도로 구성한다.

## 실제 생성에 적용

`iiLocalLLM.GenerationOptions`의 16개 필드는 SDK, IPC의 `chat.options`, HTTP의 `/v1/chat/completions`에서 사용한다.

| JSON 필드 | 기본값 | 의미·제약 |
| --- | --- | --- |
| max_tokens | 256 | 생성 한도 1..1,048,576; 모델 컨텍스트 예산을 별도 적용 |
| temperature | 0.7 | 0..10, 0=greedy. HTTP 호환 인터페이스는 0..2 |
| top_p / top_k | 0.9 / 40 | nucleus 확률 (0,1] / 상위 후보 수, K=0은 끔 |
| seed | 0 | uint32 난수 시드. 다른 엔진·장치의 동일 출력을 보장하지 않음 |
| stop | [] | 최대 16개, 각 1..1024자; 중지 문자열을 출력에서 제외 |
| min_p | 0 | 최대 후보 대비 최소 상대 확률 0..1 |
| typical_p | 1 | typical sampling (0,1]. 활성화는 llama.cpp만 지원 |
| min_keep | 1 | 최소 후보 수. MLX에서는 min-p에 적용; 실제 어휘 크기 검사 |
| repetition_penalty | 1 | 부호를 고려한 반복 패널티 (0,100], 1=끔 |
| repetition_context_size | 64 | 패널티 기록 토큰 수; -1=전체, 0=끔 |
| presence_penalty / frequency_penalty | 0 / 0 | 존재·출현 빈도에 대한 가산 패널티, 각각 -2..2 |
| xtc_probability / xtc_threshold | 0 / 0.1 | XTC 적용 확률 0..1 / 후보 임계값 0..0.5 |
| logit_bias | {} | 십진수 int32 토큰 ID → -100..100 가산 로짓. 어휘 범위 밖 ID 거부 |

```cpp
GenerationOptions options = generationOptionsFromJson({
    {"max_tokens", 128}, {"temperature", 0.7}, {"min_p", 0.05},
    {"repetition_penalty", 1.1}, {"frequency_penalty", 0.2}
});
ChatRequest request;
request.sessionId = sessionId;
request.prompt = "앞의 대화를 이어서 설명해 줘.";
request.options = options;
auto response = service.chat(request).result.get();
```

일부 제공자 객체에서도 명시한 필드만 변환할 수 있다.

```cpp
ParameterObject native("llama.common_params_sampling");
native.set("min_p", 0.05);
native.set("penalty_repeat", 1.1);
request.options = generationOptionsFromParameters(native);
```

각 필드의 `bindingKey`와 `nativeBindings`로 지원을 조회한다. 바인딩이 없는 필드는 원본 JSON으로 내보낼 수 있지만 생성 옵션 변환 시 `RuntimeUnavailable`을 반환한다. 원본 제공자의 전체 기본값·무제한 토큰 표기·필터 순서·컨텍스트 정책을 복제하지 않으며 iiLocalLLM 공통 제약을 다시 검사한다. `requireNativeBinding(runtime)`은 명시된 필드마다 해당 바인딩 존재를 검사한다.

llama.cpp는 prompt 전체로 sampler history를 초기화하고 이후 토큰은 upstream sampler가 반영한다. MLX는 KV 재사용 및 청크 prefill에서 생략된 토큰을 logits processor 앞에 복원한다. 양쪽 모두 이전 대화가 패널티 기록에 포함된다. 샘플러 구현은 해당 엔진을 재사용하며 필터 순서와 결과 분포가 엔진 간 같다는 보장은 없다.

## CLI·IPC

데몬을 실행한 상태에서 다음과 같이 조회한다. CLI는 Qt Core/Network만 링크하고, 카탈로그 조회·검증도 데몬에 요청한다. 설정 조회에는 모델 설치나 로딩이 필요 없다.

```sh
./build/iillm --socket "$PWD/build/chat.sock" parameters
./build/iillm --socket "$PWD/build/chat.sock" parameters trl.GRPOConfig
./build/iillm --socket "$PWD/build/chat.sock" parameters transformers.TrainingArguments docs/examples/training.json
./build/iillm --socket "$PWD/build/chat.sock" parameters peft.LoraConfig docs/examples/lora.json --defaults
./build/iillm --socket "$PWD/build/chat.sock" run qwen2.5:0.5b "안녕하세요" --options docs/examples/generation.json
```

`parameters GROUP FILE`은 필수 필드까지 검사한 원본 JSON을 stdout으로 반환한다. `--defaults`는 리터럴 기본값 추가, `--redact`는 민감하다고 표시된 필드와 중첩 참조의 해당 필드를 가린다. 임의 opaque JSON 내부의 비밀을 발견하는 기능은 아니므로 공개 로그에는 원본 설정을 그대로 기록하지 않는다. 가린 출력은 설정 재입력용이 아니다.

`--options FILE`은 공통 생성 JSON을 읽는다. 명시한 `--temperature`, `--max-tokens`는 파일의 해당 값보다 우선하며, 지정하지 않은 CLI 기본값은 파일 값을 덮어쓰지 않는다. 최대 입력 파일 크기는 1 MiB이다.

IPC 메서드는 `parameters.list`, `parameters.get {group}`, `parameters.validate {group, values, defaults?, redact?}`이다. 목록은 그룹 요약, get은 상세 정의, validate는 오류 없는 경우 원본 형식의 객체를 반환한다. 기존 프레임·출력 크기 제한이 유지된다.

## 재생성·배포

```sh
python3 -B scripts/fetch_parameter_sources.py
python3 -B scripts/generate_parameter_catalog.py
cmake -S . -B build
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

공식 소스는 `build/parameter-sources`에만 다운로드하며 커밋과 SHA-256을 검사한다. 모든 소스가 캐시되어 있으면 재생성에 네트워크가 필요 없다. 평상시 CMake 빌드·실행은 내장 카탈로그를 사용하며 학습 프레임워크 설치나 네트워크 접근을 요구하지 않는다. 잠금 파일, 생성기, 카탈로그, coverage, 테스트를 함께 갱신한다. 내보내기에 필요한 상세 카탈로그·출처 목록·문서·외부 고지는 설치 패키지에도 포함한다.

0.3.0에서 `GenerationOptions`의 공개 레이아웃이 변경되었다. 공유 라이브러리 ABI는 `0.3`이며 0.2 소비자는 다시 빌드해야 한다. CMake 버전 호환성은 같은 minor 버전으로 제한한다. 기존 JSON의 여섯 생성 필드는 그대로 사용 가능하며 잘못되거나 알 수 없는 옵션은 오류로 반환한다.

MLX 샘플링 헬퍼는 공식 커밋 `dcbcf786c0cf56f9a12fabe9468c887781431ae2`의 원본 파일을 포함한다. 배포판 mlx-lm 0.31.3의 min-p/최소 후보 수 API 충돌을 수정한 파일이며, 모델 생성·캐시는 mlx-lm 0.31.3을 유지한다. 파일 해시를 CMake와 카탈로그 테스트에서 검사하고 최소 후보 수 및 기록 패널티를 수치 테스트한다.
