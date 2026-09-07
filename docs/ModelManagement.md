# Model Manager와 영속 모델 카탈로그

앱은 `model://qwen3-8b-q4`를 전달하고 서비스가 설치 위치를 해석한다. 모델 설치·메타데이터·파일 검증은 ModelCatalog가 담당하며 추론 엔진을 참조하지 않는다. 서비스 내부 ModelManager는 이 카탈로그, ModelRegistry, ModelResidencyManager와 RuntimeManager를 조합한다. RuntimeManager는 검증된 경로를 받아 실행 엔진·장치를 선택하고 메모리에 올라간 모델의 수명을 관리한다.

```text
앱 / IPC: model://id
           ↓
      ModelManager
      ├─ ModelCatalog → Models/<package>/manifest.json + assets
      └─ RuntimeManager → Runtime → llama.cpp / MLX / 사용자 런타임
```

## API와 수명

| ModelManager 연산 | 공개 C++ Service API | IPC | 동작 |
| --- | --- | --- | --- |
| install | installModel(packageDirectory) | models.install | 로컬 패키지를 복사하고 검증한 뒤 카탈로그에 게시한다. 엔진을 로드하지 않는다 |
| pull | pullModel(reference, progress) | models.pull | 고정 원격 원본을 내려받고 검증한 뒤 설치한다. 기존 정상 설치는 재사용한다 |
| remove | removeModel(uri) | models.remove | 설치 파일을 제거한다. 로드 중이면 model_in_use이다 |
| list | installedModels() | models.list | 설치된 메타데이터와 로드 여부, 잘못된 패키지의 issues를 반환한다 |
| resolve | resolveModel(uri) | models.resolve | URI를 설치된 manifest에 대응시킨다. 파일 해싱은 수행하지 않는다 |
| verify | verifyModel(uri) | models.verify | 전체 파일의 크기·SHA-256·누락·추가와 형식 구조를 검사한다 |
| load | loadModel(ModelLoadRequest) | models.load | URI 해석·무결성 검증 후 런타임과 장치를 자동 선택한다 |
| unload | unloadModel(uri) | models.unload | 엔진의 모델을 해제한다. 설치 파일은 유지하며 세션이 남아 있으면 model_in_use이다 |

Service 제어 API는 future를 반환하며 상태 변경은 scheduler에서 직렬 실행한다. models()는 worker가 게시한 상주 모델 값 스냅샷을 읽어 추론 중에도 즉시 완료한다. `Service::models()`와 IPC `models.loaded`는 현재 메모리에 올라간 모델의 ModelInfo 배열을 반환한다. ModelInfo에는 모델 URI, manifest, 적용된 contextTokens/options, execution, 메모리 추정치, keepAliveMs, expiresInMs, activeRequests가 있다. 파일 경로를 받는 ModelSpec은 런타임 어댑터 계약이다. 앱의 ModelLoadRequest에는 `model`, `contextTokens`, `options`, `keepAliveMs`가 있다. 기존 상주 인스턴스는 호환 설정으로 재요청하면 재사용한다. 다른 context/options로 바꾸려면 명시적으로 unload한다.

독립적인 설치 도구는 공개 `ModelCatalog`를 직접 사용할 수 있다. 이 클래스는 동기식이며 같은 인스턴스의 동시 호출을 지원하지 않는다. 호스트용 `ModelCatalog::resolve()`의 ResolvedModel에는 실제 directory/entryPath가 있다. 앱용 `Service::resolveModel()`은 경로 없는 ModelRecord만 반환한다.

```cpp
iiLocalLLM::ServiceOptions options;
options.modelsDirectory = "/absolute/workspace/path/Models";
iiLocalLLM::Service service(options);

// 관리 도구가 원본 패키지 위치를 지정한다. 이미 설치된 id를 덮어쓰지 않는다.
auto installed = service.installModel("/absolute/path/qwen-package").get();
auto listing = service.installedModels().get();
auto record = service.resolveModel("model://qwen3-8b-q4").get();
auto verified = service.verifyModel(record.uri).get();
auto loaded = service.loadModel({record.uri, 2048}).get();
// 해당 모델의 세션을 모두 닫은 뒤 해제·제거한다.
service.unloadModel(record.uri).get();
service.removeModel(record.uri).get();
```

설치 정보는 프로세스 재시작 후에도 유지한다. 로드 상태·대화 이력·KV는 메모리 상태이므로 재시작 후 다시 만든다. 저장소는 ServiceOptions.modelsDirectory 또는 daemon `--models-root`로 정하며 기본값 `Models`는 프로세스 작업 디렉터리를 기준으로 한다. 추론 클라이언트는 이 저장소 경로를 알 필요가 없다.

## 패키지와 manifest

```text
Models/
├─ qwen3-8b-q4/
│  ├─ manifest.json
│  ├─ model.gguf
│  └─ tokenizer.json
└─ llama-3.2-3b/
   ├─ manifest.json
   └─ model.gguf
```

설치는 `Models/<manifest.id>`로 디렉터리 이름을 정한다. 조회 시에는 디렉터리 이름 대신 manifest.id로 식별하므로 서비스가 종료된 상태에서 패키지 폴더 이름을 바꿔도 URI는 유지된다. 한 저장소에서 같은 id가 두 번 발견되면 list에 issue를 추가하고 그 URI의 resolve/load/remove를 already_exists로 거부한다.

다음은 설치 입력으로 허용하는 최소 GGUF manifest이다.

```json
{
  "id": "qwen3-8b-q4",
  "architecture": "qwen3",
  "format": "gguf",
  "quantization": "Q4_K_M",
  "context_length": 32768,
  "capabilities": ["text-generation", "chat", "tool-calling"]
}
```

| 필드 | 계약 |
| --- | --- |
| schema_version | 선택, 생략 시 1. 현재 1만 허용하며 알 수 없는 필드를 거부한다 |
| id | 필수, 1~128자 소문자 영숫자 및 내부 `._-`. 처음과 끝은 영숫자, Windows 예약 장치 이름 제외 |
| architecture | 필수, 공백만으로 이루어지지 않은 128자 이하 문자열 |
| format | 필수, 소문자로 시작하는 소문자·숫자·하이픈 32자 이하. 내장 어댑터는 gguf와 mlx |
| quantization | 필수, 공백만으로 이루어지지 않은 64자 이하 문자열. 비양자화는 예를 들어 none |
| context_length | 필수, 2~1,048,576의 정수. 이 모델에 요청할 수 있는 컨텍스트 상한 |
| capabilities | 필수, 중복 없는 이름 1~32개. 이름은 소문자로 시작하는 소문자·숫자·하이픈 64자 이하 |
| entry_point | 선택. gguf는 model.gguf, mlx는 패키지 루트인 `.`가 기본값. 다른 형식은 명시한 상대 파일 경로가 필요하다 |
| files | 설치 입력에서 선택. 각 파일의 path, size, sha256을 포함하는 배열. 설치된 manifest에는 반드시 전체 목록이 있다 |

ModelCatalog의 URI는 정확한 `model://id` 형식이다. Service는 추가로 registry에 등록된 별칭(예: `qwen3:8b`)을 정규 URI로 해석한다. 등록되지 않은 일반 id, 실제 경로, URL query/fragment, percent escaping, 추가 경로는 받지 않는다. `entry_point`와 files.path는 패키지 내부 상대 경로여야 한다. 절대 경로, `..`, 빈 구성요소, 역슬래시, 콜론, NUL과 구성요소 끝의 점·공백을 거부한다. 파일명은 대소문자를 무시했을 때 중복될 수 없다. symlink와 일반 파일이 아닌 자산도 허용하지 않는다.

GGUF 진입 파일은 GGUF magic을 확인한다. MLX 진입 디렉터리에는 config.json, tokenizer.json 또는 tokenizer.model, 하나 이상의 safetensors가 있어야 한다. 예를 들어 MLX 패키지는 `format: "mlx"`, `entry_point: "."`를 쓰고 manifest와 같은 디렉터리에 해당 파일을 둔다. GGUF가 포함하는 tokenizer 외에 별도 tokenizer 파일이 항상 필요한 것은 아니다. split GGUF나 추가 tokenizer 파일을 포함하면 다른 자산과 함께 목록에 기록한다. 실제 모델 지원 여부는 엔진 로드에서도 확인한다.

설치는 schema_version과 entry_point를 정규화하고, manifest.json 자신을 제외한 모든 파일을 다음 형태로 기록한다. 아래 해시는 형식을 보여 주는 예시이며 실제 값은 설치 시 계산한다.

```json
{
  "path": "model.gguf",
  "size": 123456,
  "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
}
```

manifest는 최대 4 MiB, 파일 목록은 최대 10,000개, 디렉터리 중첩은 최대 64단계이다. size는 0~2^53-1의 정수, sha256은 64자리 16진수이며 소문자로 저장한다. IPC의 모델 메타데이터 응답은 큰 files 배열을 생략하고 verify 응답에 checked_files/checked_bytes/issues를 제공한다. list/resolve의 성공이 가중치 무결성을 확인했다는 뜻은 아니다. 최소 manifest만 저장소에 수동 복사한 패키지는 무결성 목록이 없으므로 list의 issue로 보고한다. install을 통해 가져와야 한다.

## 무결성과 저장소 동작

install은 저장소 안의 임시 디렉터리에 1 MiB 청크로 파일을 복사하며 SHA-256을 계산한다. 입력 manifest가 files를 제공하면 원본이 그 목록과 정확히 일치해야 한다. QSaveFile로 정규화한 manifest를 쓰고 복사본 전체를 다시 검증한 후 같은 저장소 안에서 디렉터리를 rename하여 게시한다. 중복 id나 기존 목적지에는 덮어쓰지 않는다. 실패·취소 시 임시 복사본을 정리하고 원본은 보존한다. 큰 설치·검증 작업도 서비스 종료 취소를 확인한다.

처음 로드하거나 eviction 후 다시 로드할 때는 전체 검증을 통과하기 전 추론 엔진을 호출하지 않는다. 이미 상주한 인스턴스의 재사용은 파일을 다시 해싱하지 않는다. payload 손상·누락·추가가 있으면 verify는 valid=false와 issues를 반환하고 load는 integrity_failure로 끝난다. 잘못된 URI나 설치되지 않은 모델은 future 오류이다. 손상된 manifest는 list.issues로 진단한다. 정상적인 다른 패키지는 계속 조회할 수 있다.

해시는 설치 기준과 현재 파일의 일치를 검증한다. 입력에 신뢰할 수 있는 해시가 없으면 처음 복사한 파일이 기준이다. 서명이나 배포자 인증은 제공하지 않으며, manifest와 파일을 함께 수정할 수 있는 사용자를 인증하는 수단도 아니다. architecture·quantization·capabilities는 모델 제작자의 선언이며 가중치 내용과의 완전한 의미 검증을 수행하지 않는다. `chat` 선언이 없는 모델은 세션 생성을 거부하지만 `tool-calling` 선언만으로 도구 실행 API가 생기지는 않는다.

저장소 첫 접근 시 `.iilocal-llm.lock`으로 소유권을 얻고 카탈로그가 파괴될 때 해제한다. 같은 저장소를 두 서비스나 관리 도구가 동시에 소유하면 model_in_use이다. 실행 중인 서비스의 설치·삭제는 IPC로 요청한다. 파일을 서비스 외부에서 동시에 수정하는 것은 지원하지 않는다. 동작 중인 설치·삭제를 카탈로그 소유자 사이에서 직렬화하는 잠금이며 OS 사용자 사이의 인증을 대체하지 않는다.

remove는 언로드된 모델의 디렉터리를 숨겨진 삭제 임시 디렉터리로 이동한 뒤 제거한다. 파일 삭제가 실패하면 storage_failure와 남은 임시 위치를 알린다. 강제 종료로 남은 `.install-*`/`.remove-*`/`.pull-*`는 공개 카탈로그에 노출하지 않으며 자동 복구·청소 기능은 아직 없다. 체크섬이 손상된 가중치도 유효한 manifest로 식별 가능하면 제거할 수 있다. 원본 설치 패키지와 다른 모델은 삭제하지 않는다.

## 컨텍스트와 엔진 분리

ModelLoadRequest.contextTokens가 0 또는 생략이면 manifest.context_length, ServiceOptions.defaultContextTokens(기본 2048), maxCachedContextTokens 중 최솟값을 사용한다. 명시한 요청이 manifest 상한을 넘으면 context_overflow, 캐시 예산을 넘으면 resource_limit이다. 가중치 용량과 KV의 실제 메모리는 이 토큰 상한만으로 예측하지 않는다.

ModelManager는 로드 시 검증한 manifest를 ModelInfo에 보관한다. 세션 생성은 설치된 manifest로 chat capability를 확인하고 첫 생성에서 필요할 때 모델을 로드한다. 메타데이터나 가중치를 갱신하려면 모든 세션을 닫고 unload → remove → install → load 순서로 교체한다. 등록 별칭과 고정 원본 다운로드는 서비스의 ModelRegistry가 담당하고 기존 ModelCatalog에는 네트워크 의존성을 넣지 않는다. pull은 ModelPullHandle의 future/progress/cancellation으로 제어한다. 기본 Qwen 원본 및 registry 형식은 [CLI.md](CLI.md), 상주·LRU·keep_alive는 [Residency.md](Residency.md)에 있다. 델타 업데이트와 자동 최신 버전 추적은 제공하지 않는다. 런타임 및 가속 장치 정책은 [HardwarePolicy.md](HardwarePolicy.md), 로컬 요청 형식은 [IPC.md](IPC.md)를 따른다.
