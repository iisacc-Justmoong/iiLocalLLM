# 외부 의존성 검토

서비스 고유 정책만 직접 구현하고 추론·토큰화·JSON·프로세스·IPC는 기존 라이브러리를 사용한다.

| 의존성 | 도입 이유·규모 | 라이선스·유지보수 |
| --- | --- | --- |
| Qt 6.8.3 Core/Network | 기존 Core에 Network 추가. Local Socket, QProcess, QJsonDocument, Unicode 변환, QCryptographicHash, QSaveFile, QLockFile 재사용 | 기존 Qt 설치 조건. 모듈별 LGPL/GPL/상용 배포 조건 적용 |
| llama.cpp | GGUF, tokenizer, chat template, ggml 장치 검사, CPU/Metal/CUDA/Vulkan, sampler, KV. 선택 빌드, SDK에 정적 링크 | MIT. API 변경을 관리하기 위해 커밋·아카이브 SHA-256 고정 |
| MLX / mlx-lm | Apple Silicon, sampler, prompt cache. 별도 Python 환경으로 기본 C++ 의존성 규모 제한 | MIT. 검증 릴리스 mlx 0.32.2 / mlx-lm 0.31.3 |
| cpp-httplib 0.54.1 | HTTP/1.1 parsing, body 상한, thread pool, chunked streaming, disconnect 검사. 약 767 KiB 단일 헤더를 내부에서 사용 | MIT. 배포 태그와 SHA-256 고정, 원본 고지 설치. TLS·압축 외부 의존성 비활성화 |
| OS 하드웨어 API | Metal/sysctl, DXGI, sysfs/sysconf. 별도 SDK 라이브러리 배포 없이 OS API를 재사용 | 플랫폼 제공 API. 추가 vendoring 없음 |

가속 모듈 빌드에는 플랫폼에 따라 Apple SDK, NVIDIA CUDA Toolkit, 또는 Vulkan SDK(glslc/SPIRV-Headers)가 필요하다. 설치되어 있는 도구 체인만 탐지하며 자동 다운로드·설치는 하지 않는다. CUDA Toolkit 및 GPU 드라이버의 배포 조건은 각 공급자 조건을 따른다. 모델 장치 선택은 ggml의 고정된 API를 사용하므로 GPU 탐지 라이브러리를 별도로 추가하지 않는다.

모델 관리에는 기존 Qt의 파일 복사·SHA-256·원자적 파일 저장·프로세스 간 잠금을 재사용한다. 모델 URI, manifest 필드, 설치/로드 수명은 iiLocalLLM의 도메인 계약으로 구현한다. 원격 pull은 이미 링크한 Qt Network의 QNetworkAccessManager를 사용한다. HTTPS/리다이렉트/전송 timeout은 Qt에 맡기고 서비스 고유 registry·별칭·해시 검증·원자적 설치만 구현한다. 추가 Hub SDK, HTTP 다운로드 구현, 데이터베이스를 도입하지 않아 유지보수·라이선스·의존성 규모를 유지한다. 엔진을 링크하지 않는 빌드에서도 ModelCatalog를 사용할 수 있다.

HTTP 도입 전 설치된 Qt HttpServer 6.8.3의 chunked responder와 cpp-httplib를 검토했다. cpp-httplib는 요청 body 상한·고정 thread pool·연결 종료 검사를 공개 API로 제공하므로 HTTP 파서를 직접 구현하지 않고 서비스 future를 연결할 수 있다. Qt HttpServer 모듈을 추가하지 않고 MIT 단일 헤더를 선택했다. 헤더는 third_party/cpp-httplib에 원본 그대로 보관하고 CMake 구성 시 해시를 검사하므로 기본 빌드에 다운로드가 없다. 서비스 고유 JSON 매핑과 SSE 이벤트 조합만 직접 구현했다.

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
