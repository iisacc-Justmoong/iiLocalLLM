# 서비스의 하드웨어 검사와 자동 선택

앱은 모델과 대화만 지정한다. ModelLoadRequest는 model URI/contextTokens/options이며 runtime/backend/device/gpu_layers 필드가 없다. IPC 모델 요청과 daemon 설정도 같은 계약을 따른다. ModelManager가 URI·manifest·파일 무결성을 검증한 후 내부 ModelSpec에 실제 경로와 형식을 전달한다. 실행 선택은 ModelInfo.execution, models.load 응답, models.loaded의 읽기 전용 결과이다. 서비스 호스트의 런타임 등록과 Python 실행 환경 설정은 배포 구성에 해당한다.

## 시작 시 검사

Service 생성 시 detectHardware가 프로세스 내 최초 스냅샷을 만든다. 후속 Service 인스턴스는 같은 부팅 검사를 공유한다. Service.hardware(), hardware.get, daemon의 --hardware로 조회한다. GPU 교체·드라이버 변경 후에는 서비스를 다시 시작한다. 모델 로드 시 선택 장치가 사라졌거나 초기화에 실패하면 아래 재시도 정책을 따른다.

| 필드 | 근거와 의미 |
| --- | --- |
| cpu_architecture | 네이티브 CPU 아키텍처. macOS Rosetta에서도 hw.optional.arm64로 Apple Silicon을 확인한다 |
| ram_bytes | macOS sysctl hw.memsize, Windows GlobalMemoryStatusEx, Unix sysconf의 물리 RAM |
| apple_silicon | macOS에서 실제 ARM64 하드웨어인지 검사 |
| gpus[].id / name / vendor | OS 또는 가속 API의 어댑터. Apple/NVIDIA/AMD/Intel/unknown |
| gpus[].vram_bytes | 확인 가능한 전용 GPU 메모리. 통합 메모리 또는 확인 불가이면 null |
| gpus[].unified_memory | Metal hasUnifiedMemory 또는 ggml integrated GPU 유형. OS에서 확인 불가이면 null |
| gpus[].recommended_working_set_bytes | Metal의 권장 작업 메모리 예산. VRAM이나 전체 RAM으로 취급하지 않는다 |
| gpus[].available_backends | 해당 어댑터에서 초기화에 성공한 가속 API 목록 |
| metal_available / cuda_available / vulkan_available | 검사한 어댑터 중 초기화에 성공한 API가 있는지 여부 |
| diagnostics | 비포함 엔진 또는 초기화 실패 원인 |

Metal은 시스템 Metal API의 장치·명령 큐와 명령 완료를 검사한다. CUDA/Vulkan 및 llama.cpp용 Metal은 실제 패키지에 포함된 ggml backend registry에서 장치 속성을 읽고 backend를 생성·해제해 가용성을 확인한다. 드라이버 파일이나 환경변수의 존재만으로 가용하다고 보고하지 않는다. llama.cpp가 비활성화된 패키지는 그 엔진의 CUDA/Vulkan을 가용하다고 보고하지 않는다. 모델별 연산 및 전체 KV 할당의 성공까지 이 초기 검사로 보장하지는 않는다.

Windows는 DXGI에서 GPU vendor와 전용 메모리를, Linux는 sysfs PCI display controller에서 vendor와 제공되는 VRAM 정보를 추가 수집한다. OS가 알려주는 물리 어댑터와 ggml/Metal의 API 어댑터는 별도 id로 나올 수 있다. 동일 GPU가 반복되어도 메모리를 합산하지 않는다. 숫자를 확인할 수 없는 값은 0으로 추측하지 않고 null로 전달한다.

## 모델별 정책

1. ModelManager가 model URI를 해석하고 manifest와 전체 파일을 검증한다. 등록된 런타임의 supportsModel이 선언된 format과 실제 형식을 검사한다. GGUF magic은 llama.cpp, config.json·tokenizer.json 또는 tokenizer.model·safetensors 파일을 가진 MLX 디렉터리는 MLX가 처리한다. 자동 형식 변환이나 다운로드는 수행하지 않는다.
2. 각 런타임의 devices와 해당 하드웨어 어댑터의 available_backends를 교차 확인한다. 다른 GPU의 vendor와 API 가용성을 섞지 않는다.
3. Apple Silicon의 Apple GPU/Metal → NVIDIA/CUDA → AMD 또는 Intel/Vulkan → CPU 순서로 후보를 정렬한다. 현재 정책에서 NVIDIA에 CUDA가 없으면 CPU를 사용하며 NVIDIA/Vulkan으로 추측해 전환하지 않는다. Apple Silicon이 아닌 Intel Mac의 Metal도 이 정책에 포함하지 않는다.
4. 같은 가속 우선순위에서는 확인된 전용 VRAM 또는 Metal 권장 예산이 큰 장치를 먼저 쓰고, 같으면 device id로 정렬한다. 여러 GPU에 가중치를 분할하지 않는다. 같은 형식과 장치를 지원하는 런타임이 여럿이면 runtime id로 정렬한다.
5. 선택된 런타임이 그 장치로 로드한 뒤에만 모델을 등록한다. GPU load가 runtime_unavailable/runtime_failure/resource_limit로 실패하면 다음 후보, 최종적으로 CPU를 시도한다. 선택 결과에 실패 이유와 CPU 전환 근거를 보존한다. 잘못된 입력, 취소, timeout은 재시도하지 않는다. CPU도 실패하면 모델을 등록하지 않고 오류를 반환한다.

llama.cpp는 선택 장치 하나와 null로 끝나는 명시적 devices 배열, 모든 레이어 offload를 적용한다. GPU 컨텍스트와 KV를 한 번 사전 할당·해제하여 실패 시 모델 로드 단계에서 CPU로 전환할 수 있게 한다. CPU는 빈 devices 배열, n_gpu_layers=0, offload_kqv=false, op_offload=false를 적용한다. CPU 정책을 결정한 뒤 llama.cpp의 자동 GPU 선택을 다시 호출하지 않는다.

MLX worker는 서비스가 정한 metal/cpu를 받아 기본 장치를 설정하고 작은 연산을 실행한 뒤 mlx_lm을 import한다. mlx_lm의 생성 stream이 import 시 기본 장치를 캡처하기 때문이다. worker가 확인한 backend가 서비스 선택과 다르면 protocol_error이다. 이 어댑터는 Apple Silicon의 기본 Metal GPU와 CPU만 지원하며 MLX의 다른 가속 변형을 자동 지원한다고 간주하지 않는다. Python/MLX 의존성이 없으면 명시적으로 로드에 실패한다. mlx-lm 0.31.3의 stream_generate는 Metal 설치 여부만으로 GPU 메모리 예산을 읽어 CPU에서 실패하므로 CPU 경로는 같은 라이브러리의 generate_step과 detokenizer를 이용해 청크를 전달한다. 추론·샘플러·KV는 기존 라이브러리를 그대로 사용하며 GPU 전용 메모리 래퍼만 거치지 않는다.

모델 로드 후 실제 생성에서 드러난 연산 오류·추가 KV 메모리 부족은 해당 요청의 오류로 반환한다. 이미 스트리밍한 요청을 다른 장치에서 자동 재실행하지 않는다. 이 경우 기존 대화 이력은 유지하고 KV를 제거한다. RAM/VRAM 조회는 모델 메모리 사용량의 완전한 예측이나 여러 서비스 프로세스 간 GPU 메모리 조정을 의미하지 않는다.

## 의존성과 검증

추가 외부 추론 라이브러리를 도입하지 않고 기존 Qt와 고정 llama.cpp의 ggml registry, OS 제공 Metal/DXGI/sysctl/sysfs를 사용한다. CUDA/Vulkan은 해당 도구 체인이 설치된 배포 빌드에서 포함한다. [llama.cpp 빌드 문서](https://github.com/ggml-org/llama.cpp/blob/5202104b59ada9005db079eea43882a2b7bf5802/docs/build.md), [ggml device API](https://github.com/ggml-org/llama.cpp/blob/5202104b59ada9005db079eea43882a2b7bf5802/ggml/include/ggml-backend.h), [Metal 통합 메모리](https://developer.apple.com/documentation/metal/mtldevice/hasunifiedmemory)를 기준으로 계약을 확인했다.

tests/hardware_tests.cpp는 각 vendor/가속 조합, 다중 GPU 정렬, 미지원 장치, 어댑터 혼동 방지, 메모리 null, 형식별 런타임 선택, GPU 실패 시 CPU 재시도, 취소·입력 오류의 비재시도를 검증한다. tests/service_tests.cpp와 daemon_smoke.py는 실제 IPC에서 자동 선택과 수동 지정 거부를 확인한다. 실제 Metal/CPU 실행 결과와 미검증 플랫폼은 [Verification.md](Verification.md)에 기록한다.
