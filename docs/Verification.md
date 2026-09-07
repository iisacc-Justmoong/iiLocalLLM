# 구현 검증 기록

2026-09-07, Apple M1 Max / 32 GiB 통합 메모리의 macOS, AppleClang 21, C++20, Qt 6.8.3 환경에서 실행했다. 테스트는 실제 저장소 소스와 build/ 아래 산출물을 사용했다.

| 검증 | 결과 |
| --- | --- |
| Release CTest | 13/13 통과: 레거시 API, 서비스/IPC, 상주 정책, CLI, HTTP, 모델 카탈로그, 하드웨어 정책, MLX 정책, llama Metal/CPU 추론, 독립 daemon, MLX Metal/CPU 추론 |
| 서비스 테스트 상세 | 20개 동작 테스트 통과; Qt init/cleanup 포함 22 passed |
| 모델 카탈로그 상세 | 8개 동작 테스트 통과; Qt init/cleanup 포함 10 passed |
| HTTP 상세 | 7개 동작 테스트 통과; Qt init/cleanup 포함 9 passed |
| 하드웨어 정책 상세 | 데이터 행 포함 23개 검증 통과; Qt init/cleanup 포함 25 passed |
| MLX 캐시·장치·CPU 스트리밍 | 10개 Python 테스트 통과; prefix/trim, 장치 명시, Metal 불가, EOS/최대 토큰 종료 |
| AddressSanitizer + UndefinedBehaviorSanitizer | 기본 CTest 8/8 통과; C++/Objective-C++·HTTP 검사, llama 비활성화, leak detection 비활성화 |
| 설치 패키지 소비 | 2/2 통과: 기존 API 및 manifest/catalog/URI/pull/residency/Service/Hardware/Runtime/IPC/HTTP 공개 API, HTTP 리스너 시작·종료 |
| 독립 서비스 프로세스 | 패키지 설치 → URI로 GGUF 로드 → Native IPC와 HTTP 동시 추론 → 재시작 후 같은 URI 사용 → 언로드·제거 및 SIGTERM 정리, HTTP 단독 실행 통과 |
| 설치 후 실행 경로 | DYLD_LIBRARY_PATH, DYLD_FRAMEWORK_PATH, LIBRARY_PATH, CMAKE_PREFIX_PATH 없이 설치 daemon의 실제 추론과 소비자 2/2 통과 |

서비스 테스트는 다중 턴, 캐시 재사용, 실패·취소 rollback, FIFO, 큐 상한, LRU, 예약 토큰 상한, 모델 언로드 제한, 컨텍스트 초과, 청크 경계 stop, 종료 중 진행/대기 작업 취소, UTF-8 경계, IPC 프레임 분할, 기존 endpoint 보호, 포화 상태 cancel, 연결 종료, 출력 버퍼 초과를 검증한다. hardware.get의 실제 응답, 모델 요청에서 runtime/backend/device/gpu_layers 지정 거부, 런타임을 지정하지 않는 모델 로드도 확인했다.

## Native IPC와 HTTP

HTTP 단위 테스트는 실제 loopback TCP 연결과 결정적인 테스트 엔진을 사용했다. GET /health와 /v1/models, 과거 user/assistant 이력을 포함한 JSON 생성, SSE의 역할·Unicode delta·finish_reason·usage·[DONE], stop 문자열 처리를 검증했다. URI/등록 별칭 입력과 모델 없음, JSON·숫자·역할·미지원 필드 오류, Content-Type·body 상한, Host/Origin 검사와 점유된 포트의 실패도 확인했다.

오류가 난 생성의 일반 JSON 500과 SSE error 종료, 클라이언트 disconnect 취소, 요청 deadline, 누적 출력 상한, 리스너 close 중 생성 취소를 검사했다. HTTP의 임시 세션과 캐시가 성공·실패·취소 후 모두 제거되었다. Native Service::chat이 진행 중인 상태에서 같은 FIFO를 포화시켜 HTTP가 queue_full/429를 받는 것과 대기 요청의 timeout/504를 확인했다.

독립 daemon을 --socket과 --http-port 0으로 동시에 시작하여 같은 model://test와 llama.cpp/Metal로 Native IPC 추론, HTTP JSON 추론, 전체 이력을 다시 전달한 HTTP SSE 추론을 수행했다. HTTP 사용 후 기존 IPC 세션 이력은 같았고 세션 개수도 그 세션 하나만 남았다. IPC unload 후 HTTP /v1/models도 빈 배열이 되었다. 서비스 재시작 후 같은 검증을 반복했으며 HTTP 전용 부팅도 확인했다. 종료 후 Unix socket·저장소 lock·TCP 리스너가 모두 해제되었다.

## 모델 관리

ModelCatalog는 엔진 없이 최소 manifest로 GGUF/MLX 구조의 패키지를 설치하고 전체 파일 inventory를 생성한다. 디렉터리 이름과 독립적인 URI, 별도 인스턴스의 재조회, 저장소 소유권 충돌, 중복 id, 불완전 manifest 진단, 다른 정상 모델의 조회를 검증했다. 파일 변조·누락·추가, 원본 해시 불일치, 경로 이탈·symlink 거부, 취소/실패 시 게시되지 않는 설치, 원본과 외부 symlink 대상이 보존되는 제거도 확인했다.

서비스 테스트는 ModelManager의 7개 연산, 설치와 엔진 로드의 분리, URI 전용 요청, 기본 컨텍스트 제한, manifest 상한, chat capability, 로드 중 제거 및 세션이 있는 모델의 언로드 거부를 검사했다. 설치 파일을 변조한 뒤 load를 호출했을 때 integrity_failure가 발생하고 테스트 엔진의 load 횟수가 증가하지 않았다.

실제 GGUF·MLX smoke는 기존 로컬 가중치를 임시 패키지로 복사하여 install → verify → URI load → 추론 → unload → remove를 수행했다. 독립 daemon 테스트는 CLI 설치 후 원본 패키지를 다른 경로로 옮기고 두 번 부팅하여 model://test로 실제 추론했다. 마지막에는 IPC install/load/unload/remove로 같은 패키지를 다시 관리했다. 두 종료에서 소켓과 카탈로그 소유권 잠금이 모두 해제되었다. 로컬 패키지 테스트는 배포자 서명 검증을 포함하지 않는다. 다운로드·상주 기능의 검증은 아래와 같다.

## CLI, pull과 모델 상주

`tests/cli_smoke.py`는 실행 파일 iiLocalLLMD와 iillm을 별도 프로세스로 띄운다. otool로 iillm이 Qt Core/Network만 사용하고 libiiLocalLLM/llama/ggml을 링크하지 않는 것을 확인했다. daemon이 없을 때 run은 연결 오류로 종료했다. models/pull/ps, 위치 독립적인 alias→URI 해석, daemon 재시작 후 설치 목록 복원, 기존 정상 모델 pull 시 추가 HTTP 전송이 없는 것을 검증했다.

loopback HTTP 원본에서 실제 약 19MB GGUF를 다운로드·설치했다. 잘못된 SHA-256, 잘못된 파일 크기(부족/초과), 허용하지 않은 리다이렉트를 실패 처리하고 staging/설치 잔여 파일이 없는지 확인했다. 느린 다운로드 중 SIGINT로 CLI를 종료하자 서비스가 pull을 취소하고 임시 파일을 정리했다. PTY의 대기 중 run도 SIGINT로 130 종료하고 세션을 정리했다. sanitizer의 같은 테스트는 엔진 없는 소형 파일을 사용했다.

실제 TinyStories GGUF에는 템플릿이 없으므로 서비스 호스트의 models.load options.chat_template=chatml을 명시한 뒤 unload했다. 이후 CLI run이 기억한 서비스 설정으로 자동 재로드하여 실제 텍스트를 생성했다. CLI → HTTP → stdin을 받는 CLI 순서의 생성에서 stats.model_loads가 증가하지 않았고, 정상 run 후 세션이 남지 않았다. ps의 메모리 추정치는 weights/context/overhead를 포함했다. keep_alive=0 생성 뒤 ps가 비고 cached_contexts=0임을 확인했다.

정책 테스트는 2개 모델이 공존하는 예산에서 세 번째 모델을 요청하고, 최근 사용이 가장 오래된 모델의 KV/가중치만 제거한 뒤 이전 세션 이력으로 다시 생성하는 것을 확인했다. 단일 모델이 예산보다 크면 runtime load 횟수가 0인 채 resource_limit이다. 추가 API 호출 없이 TTL이 만료되어 실제 모델 파괴가 실행되었고, keep_alive=0인 활성 생성은 보호되다가 취소 후 해제되었다. 활성 생성 중 models() future가 즉시 준비되고 active_requests=1, expires_in_ms=-1을 반환했다. HTTP의 자동 로드/재사용, keep_alive=0, 잘못된 duration도 별도 테스트했다.

실제 8 GiB 시스템의 동작은 측정하지 않았다. 8 GiB/32 GiB 하드웨어 입력에 따른 0ms/5분 기본 정책, 명시 설정 우선순위, LRU/활성 보호, 메모리 산술과 duration 범위를 결정적인 테스트로 검사했다. 실제 모델별 RSS/VRAM 피크의 정확도나 모든 특수 KV 아키텍처를 보증하는 검증은 아니다.

설치된 daemon/CLI로 고정 llama.cpp revision의 공개 LICENSE 1,078 bytes를 실제 HTTPS로 받아 SHA-256과 원자적 설치 결과를 비교했다. 기본 테스트는 계속 외부 인터넷에 의존하지 않는다.

기본 Qwen registry의 고정 revision/크기/LFS SHA-256은 공식 API에서 확인했다. 약 5GB Qwen 가중치를 실제로 다운로드하거나 추론한 검증은 수행하지 않았다. loopback TinyStories 전송 및 GGUF/MLX 실제 추론과 이 검증 범위를 구분한다.

## 하드웨어 자동 선택

설치된 daemon의 --hardware에서 cpu_architecture=arm64, apple_silicon=true, ram_bytes=34,359,738,368을 관측했다. GPU vendor는 apple, 이름은 Apple M1 Max, unified_memory=true이다. 전용 VRAM은 null로 보고하며 Metal 권장 작업 메모리 26,800,603,136 bytes와 구분한다. metal_available=true, cuda_available=false, vulkan_available=false이다. 시스템 Metal과 ggml의 어댑터 항목은 같은 물리 GPU를 가리킨다.

GGUF는 llama.cpp/metal, MLX 패키지는 mlx/metal을 자동 선택했다. 실제 IPC 모델 설정·요청은 URI를 사용하고 파일 경로·런타임·장치 필드가 없다. GPU 초기화 실패 시 CPU 재시도, vendor별 CUDA/Vulkan 분기, 잘못된 입력·취소의 비재시도는 주입한 하드웨어·실패 조건으로 검증했다. 어댑터의 실제 CPU 실행은 별도 로컬 모델 테스트로 검증했다.

## 실제 런타임 검증

llama.cpp 소스는 커밋 `5202104b59ada9005db079eea43882a2b7bf5802`를 고정했다. 테스트 모델은 ggml-org/models-moved의 `tinyllamas/stories15M-q4_0.gguf`이다. 첫 prompt 35 tokens, 생성 24 tokens, 다음 턴 캐시 재사용 58 tokens를 관측했다.

MLX는 Python 3.12.14, mlx 0.32.2, mlx-lm 0.31.3을 사용했다. 테스트 모델은 `mlx-community/SmolLM-135M-Instruct-4bit`, revision `642e06afe3fab57fd6cc518637c471af0a569e1e`이다. 첫 prompt 17 tokens, 생성 24 tokens, 다음 턴 캐시 재사용 41 tokens를 관측했다.

두 런타임에서 Metal 자동 선택 → 실제 생성 → 다음 턴 KV 재사용 → 세션 초기화 → 생성 도중 취소 → 이전 이력 유지/캐시 삭제 → 새 생성까지 통과했다. 모델 출력과 delta 결합 결과가 같은지도 확인했다. CPU에서도 두 턴 실제 생성과 KV 재사용을 확인했으며 llama.cpp 40 tokens, MLX 22 tokens의 재사용을 관측했다. llama.cpp CPU 실행 로그는 GPU offload 0/7 layers이다. 경량 fixture이므로 답변 품질이나 모든 모델 아키텍처의 지원을 입증하지는 않는다.

## 산출물과 범위

- `build/iiLocalLLMD`, `build/iilocal-llm-service`: llama.cpp 활성화 Release daemon.
- `build/iillm`: 추론 엔진을 링크하지 않는 Native IPC 클라이언트.
- `build/stage/`: Workspace 내부 설치 검증 패키지. 시스템 SDK 설치와는 별개의 staging이다.
- `build/consumer/build/`: 설치 패키지만 링크하는 소비자.
- `build/sanitizer/build/`: 기본 기능의 sanitizer 빌드.
- `build/Testing/Temporary/LastTest.log`: Release 테스트 상세와 실제 추론 출력.
- `build/install-verification.log`: 구성·빌드·테스트·설치·소비자 검증 로그.
- `build/hardware.json`: 설치 daemon의 실제 하드웨어 스냅샷.
- `build/cli-full-tests.log`: CLI/상주 기능의 전체 Release CTest 13/13 결과.
- `build/cli-final-tests.log`: HTTP keep_alive 및 CLI의 PTY 중단 검증 추가 후 관련 테스트 2/2 결과.
- `build/cli-sanitizer.log`: CLI/상주를 포함한 ASan/UBSan 기본 CTest 8/8 결과.
- `build/cli-sanitizer-final.log`: 최종 HTTP/CLI 관련 재검증 2/2 결과.
- `build/cli-install-verification.log`: 설치 패키지와 확장된 소비자 2/2 결과.
- `build/installed-daemon-verification.log`: 이전 IPC/HTTP 설치 daemon 검증.
- `build/installed-cli-verification.log`: 라이브러리 경로 환경변수 없이 설치 iiLocalLLMD/iillm의 다운로드·취소·재시작·동일 인스턴스 추론 검증.
- `build/https-pull-verification.log`: 설치 실행 파일의 실제 HTTPS 다운로드 및 SHA-256 검증.
- `build/models/`: 로컬 추론 fixture; 배포 패키지에는 포함하지 않는다.

Windows/Linux 네이티브 빌드와 Windows Named Pipe 실행, 실제 NVIDIA/CUDA 및 AMD·Intel/Vulkan 드라이버 실행, ONNX, 다른 모델의 template/가중치, 장시간 서비스 부하 및 모델 품질은 이번 실행에서 검증하지 않았다. HTTP는 문서화한 텍스트 Chat Completions 범위이며 전체 OpenAI API/SDK 호환성 인증은 아니다. CUDA/Vulkan 선택 분기는 정책 단위 테스트 범위이다. ONNX는 사용자 정의 Runtime 구현을 등록할 수 있는 확장 지점만 제공한다. 코드는 로컬 작업 트리에 반영했고 공개 배포나 원격 저장소 변경은 검증 범위에 포함하지 않았다.
