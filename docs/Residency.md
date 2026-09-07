# 모델 상주와 메모리 예산

ModelResidencyManager는 모델별 메모리 예약, 마지막 사용 순서, 만료 시각, 활성 요청 수를 관리한다. RuntimeManager는 실제 가중치를 소유하고 ModelManager가 두 계층을 연결한다. GUI·CLI·HTTP 클라이언트는 하나의 iiLocalLLMD에 접속해 같은 모델 인스턴스를 사용한다.

1. 정규 URI 또는 서비스 registry의 별칭으로 설치된 모델을 확인한다.
2. 상주한 모델이면 같은 인스턴스를 재사용하고 활성 lease를 잡는다.
3. 없으면 전체 파일 무결성을 확인하고 가중치·KV·런타임 여유분을 추정한다.
4. 예약 합계, 모델 개수, 현재 가용 RAM을 검사한다. 부족하면 활성 요청이 없는 모델을 최근 사용이 오래된 순서로 내린다.
5. 확보할 수 없거나 단일 모델이 예산을 넘으면 runtime load 전에 resource_limit으로 종료한다.
6. 모델을 로드하고 생성한다. 성공·오류·취소 후 lease를 반환하며 유휴 만료 시각을 갱신한다.

설치 파일, 대화 이력, KV, 모델 가중치는 별개의 수명이다. 자동 LRU/만료 시 해당 모델의 KV를 먼저 파괴한 뒤 가중치를 내린다. 유휴 세션의 대화 이력은 유지하며 다음 요청이 모델을 다시 로드하고 이력으로 prefill한다. 세션이 있다는 이유만으로 수 GB의 가중치를 고정하지 않는다. 명시적인 unload/remove는 기존 계약대로 해당 모델의 세션 종료를 요구한다.

## 설정

| ServiceOptions | daemon 옵션 | 기본 동작 |
| --- | --- | --- |
| memoryBudgetBytes | --memory-budget-mib | 0이면 물리 RAM에서 max(2 GiB, 25%)를 제외한다. RAM이 그보다 작으면 절반을 예산으로 둔다. 명시 예산은 물리 RAM을 넘지 않는다 |
| memoryReserveBytes | --memory-reserve-mib | 새 모델 예약 후 최소 256 MiB의 가용 RAM 여유를 요구한다 |
| maxModels | --max-models | 상주 모델 최대 4개. 초과 시 유휴 LRU 후보를 찾는다 |
| keepAliveMs | --keep-alive | -1/생략은 자동: RAM >8 GiB는 300,000ms, <=8 GiB는 0ms |
| defaultContextTokens | --context-tokens | 2048. manifest 상한 및 캐시 토큰 예산으로 제한한다 |

물리 RAM을 탐지하지 못하면 4 GiB를 기준으로 보수적인 자동 예산/수명을 사용한다. 메모리 바이트 설정은 서비스 배포 설정이며 앱의 모델 요청에 backend/device 또는 예산 변경 필드를 두지 않는다.

```sh
./build/iiLocalLLMD --models-root "$PWD/build/Models" --socket "$PWD/build/llm.sock" \
  --memory-budget-mib 18432 --memory-reserve-mib 512 --max-models 4 --keep-alive 5m
```

IPC models.load/chat 및 HTTP /v1/chat/completions의 `keep_alive`는 숫자 초 또는 `250ms`, `30s`, `5m`, `1h` 문자열이다. 0~7일 범위에서 밀리초 정밀도를 허용한다. 생략은 기존 모델 정책 또는 서비스 기본값을 사용한다. C++ ModelLoadRequest/ChatRequest/CompletionRequest는 keepAliveMs=-1로 생략을 표현한다. 8 GiB 이하에서도 호스트/요청이 명시한 수명을 적용할 수 있다.

```json
{"id":"turn","method":"chat","params":{"session_id":"SESSION_ID","prompt":"Hello","keep_alive":"5m","options":{"max_tokens":128}}}
```

활성 생성은 keep_alive=0이어도 중간에 해제하지 않는다. 0은 생성 종료 정리에서 바로 해제한다. 양수 만료는 worker가 유휴 상태에서 약 100ms마다 확인하고 각 작업 종료 후에도 확인한다. 다른 긴 작업이 worker를 점유하면 만료 처리는 그 작업 이후로 지연된다. 만료에는 wall clock이 아닌 monotonic clock을 사용한다. 모델 재사용/명시 load와 생성 종료가 사용 순서를 갱신하며 읽기 전용 ps는 수명을 늘리지 않는다.

## 메모리 값의 의미

MemoryEstimate는 weightsBytes + contextBytes + overheadBytes의 예약 추정치이다. 모델별 process RSS 측정값이나 OS의 하드 메모리 제한이 아니다. `ps --json`의 memory에 구성 요소와 산출 근거 basis가 있고 `stats`에 전체 resident_estimated_bytes, memory_budget_bytes, available_ram_bytes, model_loads, model_evictions가 있다. 가용 RAM을 측정할 수 없으면 available_ram_bytes는 null이며 예산 검사는 계속 적용한다.

GGUF는 파일 크기에 메타데이터의 layer 수·KV head 수·key/value dimension·context 길이·캐시 수를 적용한 f16 KV 예약을 더한다. MLX는 번들 크기와 config.json의 같은 attention 정보로 추정한다. 모델이 사용할 수 있는 캐시 수는 min(maxCachedContexts, maxCachedContextTokens/contextTokens)이다. 런타임/allocator 여유분은 max(256 MiB, 가중치 파일 크기의 25%)이다. attention 정보를 해석하지 못한 형식은 컨텍스트당 토큰당 256 KiB를 예약하며 외부 Runtime은 estimateMemory를 재정의할 수 있다.

macOS는 free+inactive 페이지, Linux는 /proc/meminfo의 MemAvailable, Windows는 GlobalMemoryStatusEx의 ullAvailPhys를 읽는다. macOS speculative 페이지는 free에 이미 포함되어 중복 합산하지 않는다. 통합 메모리는 RAM과 GPU 항목으로 두 번 더하지 않는다. 알려진 전용 GPU 메모리가 부족하면 llama.cpp 장치 로드에서 resource_limit을 반환해 CPU 후보로 넘어간다. 실제 GPU/컨텍스트 할당 실패도 기존 fallback 경로로 전달한다.

이 산식은 일반적인 attention/f16 KV를 기준으로 한다. 모델별 특수 state, 동적 MLX 그래프, mmap·압축·allocator 동작, 다른 프로세스의 동시 할당 때문에 실제 피크는 다를 수 있다. cgroup/job/container 제한과 여러 iiLocalLLMD 간의 전역 예산 조정은 구현하지 않았다. `memory-budget-mib`, context 및 캐시 상한으로 배포 기기에 맞게 조정한다. 실제 8 GiB 장비의 벤치마크 대신 작은 메모리 예산과 하드웨어 입력을 주입한 정책 테스트로 분기를 검증했다.
