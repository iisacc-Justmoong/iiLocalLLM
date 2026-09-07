# 계층과 변경 지점

Service는 future/stream callback façade이다. 시작 시 읽기 전용 HardwareInfo를 확보하고 내장 런타임을 등록한다. Service::Impl::State가 ModelManager, SessionManager, ContextCacheManager를 소유한다. ModelManager는 ModelRegistry와 ModelResidencyManager를 조합한다. Scheduler worker가 상태 변경을 직렬화한다. 도메인 managers는 UI·IPC·HTTP를 참조하지 않는다. LocalIpcServer와 HttpApiServer는 같은 객체의 공개 Service API만 호출한다. 런타임은 Runtime.h / Hardware.h / Types.h와 하드웨어 탐지 계약에 의존하며 서비스 내부를 참조하지 않는다.

```text
iillm (tools/IpcClient.cpp; Qt Core/Network만 링크)
          ↓ Native IPC
iiLocalLLMD (tools/service_main.cpp)
          ↓
LocalIpcServer ─┐
HttpApiServer ──┴→ Service → Scheduler / SessionManager / ContextCacheManager
                    ↓
                ModelManager
                ├─ ModelRegistry → 별칭, 고정 원격 원본, 검증된 pull
                ├─ ModelResidencyManager → 메모리 예약, 활성 lease, LRU, keep_alive
                ├─ ModelCatalog → Models/manifest.json + weights/assets
                └─ RuntimeManager → Runtime 인터페이스
                                     ↑          ↑
                              LlamaRuntime   MlxRuntime → mlx_worker.py → mlx-lm
```

ModelManager는 install/remove/list/resolve/verify/load/unload를 조정한다. ModelCatalog는 manifest·URI·파일 설치·무결성·제거를 담당하며 Runtime/Service를 참조하지 않는다. RuntimeManager는 id → {ModelSpec, ExecutionSelection, shared_ptr<RuntimeModel>} 맵과 엔진 등록을 관리한다. 앱의 ModelLoadRequest에는 model URI/등록 별칭, contextTokens, options, keepAliveMs가 있다. ModelManager가 URI와 전체 파일을 검증한 뒤 런타임용 ModelSpec에 id, 실제 path, format, contextTokens, options를 전달한다. 클라이언트가 런타임이나 장치를 지정하는 필드는 없다.

RuntimeManager는 manifest 형식과 Runtime의 지원 장치 목록을 하드웨어 정책으로 평가한다. ModelInfo는 검증한 manifest·URI·적용 컨텍스트와 성공한 실행 선택을 반환하며 경로를 노출하지 않는다. 설치 파일은 영속 상태이고 로드 상태는 프로세스 내 상태이다. Service::installedModels()/models.list와 Service::models()/models.loaded로 구분한다. 상세 계약은 [ModelManagement.md](ModelManagement.md), [Residency.md](Residency.md)에 있다.

SessionManager는 sessionId → {modelId, messages}이다. 앱은 createSession에 model URI를 전달하고 내부 이력은 manifest의 id를 사용한다. IPC에서는 URI로 반환한다. messages에는 선택 system과 성공한 user/assistant 쌍만 저장한다. 세션의 modelId는 변경하지 않는다. chat capability는 설치된 manifest로 확인한다. 세션 생성은 모델을 로드하지 않으며 생성 실행 시 ModelManager::acquire가 필요한 모델을 확보한다.

ContextCacheManager는 sessionId → {modelId, reservedTokens, access, unique_ptr<RuntimeContext>}이다. 컨텍스트 확보 전 개수와 예약 토큰 총량을 검사하고 가장 오래된 access를 제거한다. 세션 이력은 남으므로 다음 요청에서 컨텍스트를 복구한다. 명시적인 unload는 세션 종료를 요구한다. 자동 LRU/만료는 beforeUnload 콜백으로 모델의 모든 KV를 먼저 제거하고 가중치를 내리며 세션 이력을 보존한다.

Runtime은 id/supportsModel/devices/load를 구현하고 estimateMemory를 재정의할 수 있다. supportsModel은 모델 형식을 검사하며 devices는 HardwareInfo의 실제 사용 가능한 장치 중 해당 엔진이 지원하는 RuntimeDevice를 반환한다. load는 서비스가 선택한 ModelSpec/RuntimeDevice/CancellationToken을 받아 지정 장치를 명시적으로 적용한다. RuntimeModel은 tokenize/createContext, RuntimeContext는 generate를 구현한다. 모델별 template·tokenizer를 runtime에 둔다. generate는 전체 prompt token을 받고 정확한 prefix KV를 재사용하며, CancellationToken과 TextCallback의 false를 처리한다. 결과에 생성·재사용 토큰 수를 반환한다.

객체 호출·파괴는 서비스 worker에서 실행한다. 종료 시 작업을 취소하고 future를 완료한 뒤 worker에서 컨텍스트→모델→런타임을 파괴한다. worker는 façade의 수명과 독립인 Impl을 참조한다. 외부 런타임도 주기적으로 취소를 확인하고 callback을 보관하거나 별도 스레드에서 호출하지 않아야 한다.

## 채팅 트랜잭션

Service::chat은 기존 세션에 새 prompt를 넣는다. Service::complete는 model URI와 전체 messages를 받고 worker에서 임시 세션을 만든다. 입력은 선택적 첫 system과 user/assistant 순서를 검증한다. 과거 응답은 이력으로 사용하고 마지막 user만 생성 대상으로 삼는다. 두 메서드는 같은 Impl::generate, PromptEngine, RuntimeContext 경로와 FIFO를 사용한다. 임시 세션과 KV는 성공·오류·취소 후 worker에서 제거한다. 전송 어댑터에 별도의 추론 엔진이나 세션 저장소를 두지 않는다.

FIFO 항목 실행 시 최신 세션을 읽고 상주 모델을 acquire한다. 활성 lease는 성공·오류·취소 후 release하며 마지막 사용 시각과 만료를 갱신한다. 요청이 없어도 Scheduler가 100ms 주기로 유지 관리를 실행한다. models.loaded/Service::models는 worker가 게시한 값 스냅샷을 읽으므로 추론 중에도 ps가 응답한다. 변경과 파괴는 worker만 수행한다. PromptEngine이 이력 사본에 user를 추가하고 토큰화한다. contextTokens-maxTokens를 초과하면 가장 오래된 user/assistant 쌍을 제거하며 반복한다. system과 최신 user만 남아도 초과하면 오류이다.

컨텍스트 확보와 started 이후 StopFilter를 거쳐 delta를 전달한다. 결과와 최종 취소 여부를 확인한 뒤 완결 턴을 반영한다. started/delta 콜백 예외는 consumer_failure로 종료하고 이력 변경을 취소한다. finished 콜백 예외는 확정한 결과 및 future 완료를 바꾸지 않는다.

오류·취소·stop 중단 시 KV를 폐기한다. llama.cpp는 토큰 prefix를 비교하고 마지막 prompt token을 재평가해 현재 logits를 만든다. 부분 제거가 불가능하면 메모리를 비우고 prefill한다. MLX는 실제 layer offset과 정확한 token prefix를 기록하며 안전하게 trim할 수 없는 cache는 재사용하지 않는다.

HttpApiServer는 cpp-httplib의 한정된 전송 thread pool에서 요청을 받아 Service future/handle을 기다린다. 서비스 callback은 mutex로 보호한 크기 제한 큐에 SSE 프레임을 넣으며 socket 쓰기는 전송 스레드가 수행한다. 연결 종료·deadline·출력 초과·서버 종료는 생성 취소로 연결한다. LocalIpcServer는 Qt 이벤트 루프에서 비동기 future와 스트림 inbox를 처리한다. 한쪽의 연결 종료가 다른 전송 계층의 요청 핸들을 취소하지 않는다. HTTP의 세부 계약은 [HTTP.md](HTTP.md)에 있다.

ONNX 등은 서비스 호스트에서 Runtime/RuntimeModel/RuntimeContext를 구현하고 registerRuntime으로 등록한다. 사용자 형식의 manifest는 format과 상대 entry_point를 선언하고 어댑터가 ModelSpec.format 및 실제 파일을 확인한다. 앱에는 등록·선택을 위한 IPC 메서드를 노출하지 않는다. 기존 HardwareInfo에 없는 가속 API는 하드웨어 탐지에도 지원을 추가해야 한다. 같은 형식·장치를 여러 런타임이 지원하면 runtime id의 사전 순서를 적용한다. ONNX 어댑터와 디스크 세션 저장은 아직 구현하지 않았다. 장치 정책과 메모리 정보의 의미는 [HardwarePolicy.md](HardwarePolicy.md)를 따른다.
