# 외부 의존성 검토

서비스 고유 정책만 직접 구현하고 추론·토큰화·JSON·프로세스·IPC는 기존 라이브러리를 사용한다.

## 파일 패턴 매처 (0.19.0)

2026-09-15 [libgit2 v1.9.7](https://github.com/libgit2/libgit2/releases/tag/v1.9.7)의 유지보수 릴리스와 원본 C wildmatch를 확인했다. 전체 Git·TLS·전송 의존성 대신 약 10KiB의 C/H만 private object로 포함한다. 라이선스는 MIT가 아니라 **GNU GPL v2 + libgit2 Linking Exception**이다. [원문](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING)을 보존하고 수정 C/H·날짜·소스 해시·빌드 설명까지 설치 패키지 `licenses/libgit2-wildmatch-source/`에 제공한다. 한도·취소·재귀 제한을 추가한 변경점은 `third_party/wildmatch/PATCHES.md`를 따른다. 원본 다운로드 SHA는 `source.json`에 있고 수정 소스와 구분한다.

매처는 외부 Git 상태를 읽지 않는다. 설정 병합·경로·권한 수명은 C++ 도메인 코드이며 Qt JSON을 재사용한다. [node-ignore 7.0.5](https://github.com/kaelzhang/node-ignore/tree/7.0.5)는 독립 검사 기준으로만 사용했다. SDK에 JavaScript·Node 의존성을 추가하지 않았다. 독립 사례와 차이는 [PermissionSettings.md](docs/PermissionSettings.md)를 따른다.

## Bash 구문 파서 (0.18.0)

2026-09-15 공식 릴리스·소스·MIT 고지를 확인하고 [tree-sitter v0.27.0](https://github.com/tree-sitter/tree-sitter/releases/tag/v0.27.0)과 [tree-sitter-bash v0.25.1](https://github.com/tree-sitter/tree-sitter-bash/releases/tag/v0.25.1)을 선택했다. 유지되는 C runtime과 생성된 Bash C grammar를 private object로 포함한다. TypeScript, Rust/Node CLI, Python, 추가 공유 라이브러리는 생산 실행에 필요하지 않다. 라이브러리가 Bash AST를 만들고 iiLocalLLM 고유의 규칙·권한 수명·보수적 판정을 C++에서 처리한다.

- tree-sitter 압축 원본 1,020,259 bytes, SHA-256 `d35c96e68736bd9569d2757c3cc71052485f33082c3825f1aed9d0e86013a159`.
- tree-sitter-bash 압축 원본 543,006 bytes, SHA-256 `2e785a761225b6c433410ef9c7b63cfb0a4e83a35a19e0f2aec140b42c06b52d`.
- `cmake/PermissionParsers.cmake`에서 버전·해시를 고정하고 원본 MIT 라이선스를 `share/iiLocalLLM/licenses/tree-sitter.LICENSE` 및 `tree-sitter-bash.LICENSE`로 설치한다. 외부 파서 타입은 공개 SDK 헤더에 노출하지 않는다.
- 업스트림의 `TREE_SITTER_HIDE_SYMBOLS` 설정으로 런타임·문법 C 심볼의 공개를 막는다. 같은 호스트의 다른 tree-sitter 버전과 결합되지 않도록 macOS `nm` 회귀 검사에서 공개 심볼 부재를 확인한다.
- 문법 파서는 명령의 실제 부작용을 판정하는 샌드박스가 아니다. 지원하는 정적 구문과 입력·시간 상한은 [Permissions.md](docs/Permissions.md)를 따른다.

2026-09-14 에이전트 입력/출력 검증에 [jsoncons 1.9.0](https://github.com/danielaparker/jsoncons/releases/tag/v1.9.0)을 추가했다. 2026-08-07 릴리스와 JSON Schema 2020-12 지원을 공식 소스에서 확인했다. 약 1.7 MB 압축 아카이브의 header-only C++ 의존성이며 실행 프로세스·Python·네트워크 서비스는 추가하지 않는다. CMake는 아카이브 SHA-256 `f1017b36e4e034acd5c0f5f616bacf5d7a161d6d3a43ff9ddb73fd8dca4d3cd9`를 고정한다. Boost Software License 1.0 및 포함된 A5HASH의 MIT 고지를 `third_party/jsoncons`와 설치 패키지 licenses에 보존한다. 외부 공개 헤더에는 jsoncons 타입이 없다.

| 의존성 | 도입 이유·규모 | 라이선스·유지보수 |
| --- | --- | --- |
| Qt 6.8.3 Core/Network | 기존 Core에 Network 추가. Local Socket, QProcess, QJsonDocument, Unicode 변환, QCryptographicHash, QSaveFile, QLockFile 재사용 | 기존 Qt 설치 조건. 모듈별 LGPL/GPL/상용 배포 조건 적용 |
| llama.cpp | GGUF, tokenizer, chat template, ggml 장치 검사, CPU/Metal/CUDA/Vulkan, sampler, KV. 기본 ON이며 명시적 OFF 가능, SDK에 정적 링크 | MIT. API 변경을 관리하기 위해 커밋·아카이브 SHA-256 고정 |
| MLX / mlx-lm | Apple Silicon, sampler, prompt cache. 별도 Python 환경으로 기본 C++ 의존성 규모 제한 | MIT. 검증 릴리스 mlx 0.32.2 / mlx-lm 0.31.3 |
| cpp-httplib 0.54.1 | HTTP/1.1 parsing, body 상한, thread pool, chunked streaming, disconnect 검사. 약 767 KiB 단일 헤더를 내부에서 사용 | MIT. 배포 태그와 SHA-256 고정, 원본 고지 설치. TLS·압축 외부 의존성 비활성화 |
| OS 하드웨어 API | Metal/sysctl, DXGI, sysfs/sysconf. 별도 SDK 라이브러리 배포 없이 OS API를 재사용 | 플랫폼 제공 API. 추가 vendoring 없음 |

가속 모듈 빌드에는 플랫폼에 따라 Apple SDK, NVIDIA CUDA Toolkit, 또는 Vulkan SDK(glslc/SPIRV-Headers)가 필요하다. 설치되어 있는 도구 체인만 탐지하며 자동 다운로드·설치는 하지 않는다. CUDA Toolkit 및 GPU 드라이버의 배포 조건은 각 공급자 조건을 따른다. 모델 장치 선택은 ggml의 고정된 API를 사용하므로 GPU 탐지 라이브러리를 별도로 추가하지 않는다.

모델 관리에는 기존 Qt의 파일 복사·SHA-256·원자적 파일 저장·프로세스 간 잠금을 재사용한다. 모델 URI, manifest 필드, 설치/로드 수명은 iiLocalLLM의 도메인 계약으로 구현한다. 원격 pull은 이미 링크한 Qt Network의 QNetworkAccessManager를 사용한다. HTTPS/리다이렉트/전송 timeout은 Qt에 맡기고 서비스 고유 registry·별칭·해시 검증·원자적 설치만 구현한다. 추가 Hub SDK, HTTP 다운로드 구현, 데이터베이스를 도입하지 않아 유지보수·라이선스·의존성 규모를 유지한다. 엔진을 링크하지 않는 빌드에서도 ModelCatalog를 사용할 수 있다.

HTTP 도입 전 설치된 Qt HttpServer 6.8.3의 chunked responder와 cpp-httplib를 검토했다. cpp-httplib는 요청 body 상한·고정 thread pool·연결 종료 검사를 공개 API로 제공하므로 HTTP 파서를 직접 구현하지 않고 서비스 future를 연결할 수 있다. Qt HttpServer 모듈을 추가하지 않고 MIT 단일 헤더를 선택했다. 헤더는 third_party/cpp-httplib에 원본 그대로 보관하고 CMake 구성 시 해시를 검사하므로 HTTP 의존성의 추가 다운로드가 없다. 서비스 고유 JSON 매핑과 SSE 이벤트 조합만 직접 구현했다.

출처 확인일: 2026-09-07. 가중치를 SDK에 포함하거나 자동 설치하지 않으며 모델 라이선스는 별도로 적용한다.

- [llama.cpp 고정 커밋](https://github.com/ggml-org/llama.cpp/tree/5202104b59ada9005db079eea43882a2b7bf5802)
- 아카이브 SHA-256: `58345c999af65b5dec07b71601eabbb0a30cc1f8842d4f00190b87b5a776fc7e`
- [C API](https://github.com/ggml-org/llama.cpp/blob/5202104b59ada9005db079eea43882a2b7bf5802/include/llama.h), [llama.cpp MIT license](https://github.com/ggml-org/llama.cpp/blob/5202104b59ada9005db079eea43882a2b7bf5802/LICENSE)
- [mlx-lm](https://github.com/ml-explore/mlx-lm), [generation](https://github.com/ml-explore/mlx-lm/blob/6d21ce4b065a2e163fa6de76a9936c61aeb5784a/mlx_lm/generate.py), [cache](https://github.com/ml-explore/mlx-lm/blob/6d21ce4b065a2e163fa6de76a9936c61aeb5784a/mlx_lm/models/cache.py), [MIT license](https://github.com/ml-explore/mlx-lm/blob/main/LICENSE)
- [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [QLocalServer](https://doc.qt.io/qt-6/qlocalserver.html)
- [QCryptographicHash](https://doc.qt.io/qt-6/qcryptographichash.html), [QSaveFile](https://doc.qt.io/qt-6/qsavefile.html), [QLockFile](https://doc.qt.io/qt-6/qlockfile.html), [QDir::removeRecursively](https://doc.qt.io/qt-6/qdir.html#removeRecursively)
- [cpp-httplib v0.54.1](https://github.com/yhirose/cpp-httplib/releases/tag/v0.54.1), [고정 헤더](https://github.com/yhirose/cpp-httplib/blob/v0.54.1/httplib.h), [MIT license](https://github.com/yhirose/cpp-httplib/blob/v0.54.1/LICENSE)
- cpp-httplib httplib.h SHA-256: `5933c14b2d0f45212925ed18ca579841f5fce717f431fc20cec712423e905b10`
- [Qt 6.8 chunked responder](https://doc.qt.io/qt-6.8/qhttpserverresponder.html), [OpenAI Chat Completions reference](https://developers.openai.com/api/reference/resources/chat)

llama.cpp 정적 링크 시 원본 MIT 고지를 설치 패키지의 share/iiLocalLLM/licenses/llama.cpp.LICENSE에 포함한다. MLX 환경에는 원본 배포판의 고지를 유지한다. iiLocalLLM의 AGPL-3.0-only 선언은 외부 라이선스를 대체하지 않는다.

원격 pull/CLI 도입에는 이미 유지 중인 Qt 6.8.3 Core/Network를 재사용했다. CLI는 추론 SDK를 링크하지 않는다. Qt의 공유 라이브러리 라이선스 조건을 유지하며 추가 Hub 의존성은 없다. [QNetworkAccessManager](https://doc.qt.io/qt-6.8/qnetworkaccessmanager.html)와 [QNetworkRequest의 redirect/transfer timeout](https://doc.qt.io/qt-6.8/qnetworkrequest.html)을 사용한다.

기본 Qwen registry는 [공식 GGUF 저장소의 고정 revision](https://huggingface.co/Qwen/Qwen3-8B-GGUF/tree/7c41481f57cb95916b40956ab2f0b139b296d974)을 참조한다. commit/파일 크기/LFS SHA-256을 공식 API로 확인했으며 실제 가중치는 번들에 포함하지 않는다. 모델 원본의 라이선스는 해당 저장소를 따른다. 사용자가 pull을 실행하기 전 SDK 빌드·설치에서 모델을 다운로드하지 않는다.

2026-09-13 최소 대화용 모델로 [공식 Qwen2.5-0.5B-Instruct-GGUF](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/tree/9217f5db79a29953eb74d5343926648285ec7e67)의 Q4_K_M을 등록했다. 원본 모델 카드는 Apache-2.0을 선언한다. 491,400,032 bytes의 작은 모델로 최초 다운로드·메모리 비용을 낮추고, 기존 llama.cpp 어댑터와 Qt 다운로드 경로를 그대로 사용한다. 추가 런타임·Hub 라이브러리 의존성은 없다. 모델 revision·파일 크기·SHA-256은 공식 API와 대조하며 SDK에 가중치를 재배포하지 않는다. 모델 품질과 모델 계열의 업그레이드는 SDK 추론 인터페이스와 독립적으로 검토한다.


## 0.3.0 제어 카탈로그와 MLX 샘플러

2026-09-13 공식 선언을 커밋·SHA-256으로 잠갔으며 [ParameterSources.md](docs/ParameterSources.md)에 13개 프로젝트의 버전·라이선스·규모를 기록했다. 변환한 주석·타입 선언의 고지는 docs/parameter-licenses에 보관하고 설치한다. Transformers/PEFT/TRL/Accelerate/DeepSpeed/vLLM/SGLang은 Apache-2.0, llama.cpp/Ollama/LM Studio/MLX/mlx-lm은 MIT, PyTorch는 원본 BSD 계열 고지와 포함된 NOTICE를 따른다. 학습 프레임워크는 링크·실행 의존성으로 추가하지 않았다. 정적 추출에는 Python 표준 AST를, 객체 처리에는 기존 Qt JSON을 재사용한다.

MLX 0.32.2 + mlx-lm 0.31.3의 min-p 최소 후보 수 실행 오류를 실제 테스트에서 재현했다. 공식 커밋 `dcbcf786c0cf56f9a12fabe9468c887781431ae2`의 수정된 `sample_utils.py`를 변경 없이 `src/runtimes/mlx_sample_utils.py`에 포함하여 재사용한다. 파일 SHA-256은 `c93c1eef794725f9f7ce77b6212f61eb6d0fe17b9cd87c06cec6470ee12b07f2`이며 CMake에서 확인한다. 유지 중인 MIT 구현의 작은 단일 파일을 사용하며 샘플러 알고리즘을 별도로 재작성하지 않았다. 모델 로딩·생성·캐시는 검증된 기존 mlx-lm 0.31.3 의존성을 유지한다.

## llama.cpp common conversation support

The existing pinned llama.cpp source also supplies the native Jinja chat templates, tool grammar sampling and PEG output parser. No new inference process is introduced. The common static library is linked privately; its cpp-httplib symbols use a separate namespace to avoid collisions with the service HTTP server. OpenSSL downloads, LLGuidance and upstream subprocess support are disabled.

Its bundled nlohmann/json 3.12.0 is MIT licensed. Copyright notices and the MIT text are preserved in `third_party/llama-common/nlohmann-json.LICENSE`; common/base64.hpp is Unlicense (`third_party/llama-common/base64.UNLICENSE`). The existing llama.cpp and cpp-httplib license notices continue to apply.

## 프로젝트 지침 파서 (2026-09-14)

Markdown은 [MD4C release-0.5.3](https://github.com/mity/md4c/tree/472c417005c2c71b8617de4f7b8d6b30411d78f4), YAML은 [LibYAML 0.2.5](https://github.com/yaml/libyaml/tree/2c891fc7a770e8ba2fec34fc6b545c672beb37e6)를 사용한다. 둘 다 MIT 라이선스이며 공개 C++ 헤더로 외부 타입을 노출하지 않는다. 원본 C 파서를 private object로 정적 포함하므로 실행 프로세스·Python·추가 공유 라이브러리가 필요하지 않다. 원본 라이선스는 설치된 `share/iiLocalLLM/licenses/md4c.LICENSE`와 `libyaml.LICENSE`에 포함한다.

- MD4C 공식 태그·변경 기록에서 CommonMark 처리와 복잡도 관련 수정을 확인했다. 압축 원본 244,633 bytes, SHA-256 `353c346f376b87c954a13f3415ede2d51264cc61dc5abcd38ff1d2aa0d059b9e`.
- LibYAML은 릴리스 주기가 긴 C 파서다. 공식 태그 목록에서 안정판 0.2.5와 0.2.6-rc.1을 확인했으며 안정판을 고정했다. 압축 원본 85,055 bytes, SHA-256 `fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e`. frontmatter 입력·이벤트·깊이를 제한하고 alias는 거부한다.
- CMake `ContextParsers.cmake`에서 다운로드 해시를 검사한다. Markdown 토큰화와 YAML 문법을 직접 재구현하지 않고, iiLocalLLM 고유의 경로 범위·규칙 선택·순서·출처 조합만 C++에서 처리한다. glob의 regex 컴파일·매칭은 기존 Qt/PCRE2에 맡기며 지원하는 glob 표기와 차이는 ProjectContext.md에 명시한다.
- macOS 빌드에서 C 파서를 활성화하면서 ggml의 `.m` 파일과 SDK의 `.mm` 파일을 구분하도록 Objective-C와 Objective-C++ 언어를 명시했다. llama.cpp 원본은 수정하지 않았다.

## MCP HTTP 클라이언트 (0.7.0)

기존 Qt 6.8.3 Network를 재사용하여 HTTPS, HTTP 메시지 프레이밍과 소켓 처리를 맡겼다. Qt의 관리 상태·기존 배포 라이선스·추가 의존성 규모를 검토했으며 생산 패키지에 새 HTTP 또는 언어 런타임 의존성을 추가하지 않았다. 공통 JSON-RPC 처리, MCP 세션 및 SSE 이벤트 처리는 iiLocalLLM의 프로토콜 계약이다.

Qt는 응답 헤더 전 EOF에서 POST를 재전송할 수 있으므로, 공개 QIODevice 및 [DoNotBufferUploadDataAttribute](https://doc.qt.io/qt-6.8/qnetworkrequest.html#Attribute-enum)를 사용하여 이미 읽힌 업로드를 되감지 못하게 한다. 중도 종료한 SSE 연결은 캐시에서 정리한다. HTTP 요청 자체의 파서를 복제하거나 Qt private API를 사용하지 않는다. Qt 구현 참조는 [v6.8.3 qhttpnetworkconnectionchannel.cpp](https://github.com/qt/qtbase/blob/v6.8.3/src/network/access/qhttpnetworkconnectionchannel.cpp)이며 이 소스를 패키지에 복사하지 않았다.

공식 Python MCP SDK 1.26.0(MIT)와 해당 환경의 uvicorn은 HTTP 상호 운용 시험에만 사용한다. 자체 서명 TLS 거부 시험은 사용 가능한 OpenSSL 명령으로 build/tmp 안에 일회용 시험 인증서를 생성하며 생성된 키를 저장소·생산 패키지에 포함하지 않는다.

## MCP HTTP 서버 (0.8.0)

기존 [cpp-httplib 0.54.1](https://github.com/yhirose/cpp-httplib/tree/v0.54.1)의 HTTP 서버·동적 worker pool·chunked content provider를 사용한다. MIT 원문과 기존 고정 해시를 유지한다. 별도 네트워크 파서·TLS 라이브러리·Python 생산 서버를 도입하지 않는다. 연결별 MCP 상태, 인증 principal과 세션 연결, SSE 재개 기록 및 상한은 iiLocalLLM의 고유 전송 계약으로 구현했다. 암호 해시와 UUID에는 기존 Qt Core를 사용한다. 공식 Python MCP SDK 1.26.0 및 httpx는 독립 HTTP 클라이언트 시험에만 사용한다.

## 0.42.0 WebFetch HTML5 파서

[Lexbor v3.0.0](https://github.com/lexbor/lexbor/releases/tag/v3.0.0)을 2026-09-16 확인했다. 원본 archive SHA-256 `eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2`, 다운로드 5,777,325바이트이다. 라이선스는 [Apache-2.0](https://github.com/lexbor/lexbor/blob/v3.0.0/LICENSE)이며 LICENSE와 NOTICE를 설치한다. cmake/WebParser.cmake는 core/dom/ns/tag/html/encoding과 플랫폼 ports만 비공개 object로 컴파일한다. C API/심볼/헤더를 SDK에 공개하지 않으며 추가 동적 라이브러리는 없다. 전체 CSS/style/URL 엔진·예제·upstream 테스트를 제품에 링크하지 않는다. 기존 Qt Network를 조회에 재사용한다. 의존성 선정과 구현 차이는 docs/WebFetch.md를 따른다.
