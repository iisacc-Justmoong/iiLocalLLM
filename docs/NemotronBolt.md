# Nemotron Bolt 호환성과 제한된 메모리 실행

Nemotron 3.5 Bolt의 일부 GGUF는 전역 `expert_count`에 최대 전문가 수인 496을
기록하면서 개별 레이어에는 240·304·368·432·496개를 보관한다. 고정된 기존
llama.cpp 로더는 모든 레이어에 전역 수를 적용하여 첫 라우터 텐서에서 차원 오류를
반환했다. 이는 패키지 체크섬 검사와 별개의 런타임 호환성 문제이다.

`cmake/LlamaNemotronBolt.cmake`는 upstream
[`cd0a5b6a180dff0048e5fd993098b21c0b2c91b8`](https://github.com/ggml-org/llama.cpp/commit/cd0a5b6a180dff0048e5fd993098b21c0b2c91b8)의
레이어별 전문가 처리만 기존 고정 소스의 빌드 사본에 적용한다. 원본 다운로드와
모델 가중치는 수정하지 않는다. 라우터·전문가 텐서는 실제 레이어 수로 읽으며,
그래프도 동일한 수로 라우팅한다. top-k보다 작거나 전역 최대값보다 큰 수는 거부한다.
지원하지 않는 원본 코드 형태는 CMake 구성 단계에서 실패한다.

실제 GGUF를 사용한 선택적 회귀 검증은 다음 옵션으로 활성화한다. 시험은 다운로드를
수행하지 않으며 모델 로드·네이티브 CPU 생성·종료 토큰·비어 있지 않은 답장을 검사한다.

```sh
cmake -S . -B build -DIILOCALLLM_TEST_NEMOTRON_BOLT_GGUF=/absolute/model.gguf
cmake --build build --parallel 4
ctest --test-dir build -R '^iiLocalLLM.nemotron_bolt$' --output-on-failure
```

이 검증은 공개 Runtime API의 단일 CPU 컨텍스트를 사용한다. Service의 상주 메모리
예산과 여러 캐시 컨텍스트의 사전 예약 정책은 변경하지 않는다. 이 정책을 거치는
제품의 모델 수락 여부와 텍스트 외 모달리티의 성공은 각각 별도 검증 대상이다.

## 2026-10-23 실제 에이전트 인사 왕복

로컬 `Nemotron-Bolt.safetensors`의 `iild-safetensors-package-v1`에서
`NVIDIA-Nemotron-3.5-Bolt-74B-A9B-24GB.gguf` 본체를 읽기 전용으로 추출했다.
22,018,326,528바이트의 SHA-256은
`91d63363e782c8a3d5f4be42816eba8e0d42dd2530bd07a7d0d2dc440599af75`이며,
패키지에 기록된 값 및 공식 배포 파일 메타데이터와 일치했다.

공개 `iiMultimodalAgent::agent::Engine`에 호스트 Model 어댑터를 연결하고,
이 어댑터가 실제 iiLocalLLM Runtime의 대화 준비·생성·파싱을 호출했다.
입력은 `안녕!`, 시스템 지시는 `Say hello.`, 온도는 0, 출력 상한은 64토큰,
컨텍스트는 2,048토큰이었다. 답장은 다음과 같다.

> 안녕하세요! 무엇을 도와드릴 수 있을까요?

실행 ID는 `9a9cb3d6-00e3-4ad9-80d2-3f7054afa7b0`이다. 에이전트 상태는
`completed`, 오류는 `none`, 입력은 22토큰, 생성은 14토큰이다. 호스트는 네이티브
종료 토큰과 비어 있지 않은 파싱 결과, 도구 호출 부재를 확인한 뒤 답장을 반환했다.
모델 로드 이후 에이전트 왕복은 18.25초, 프로세스 시작부터 종료까지는 588.43초였다.
외장 스토리지에서 약 22GB를 읽는 시간이 전체 실행의 대부분을 차지했다.

성공한 설정은 `memoryMapWeights=false`, `maxGpuLayers=1`,
`validateTensorData=true`와 프로세스 범위의 `GGML_METAL_NO_RESIDENCY=1`이다.
이 모델에서 실제 오프로딩은 출력 레이어 하나이며 반복 레이어는 CPU에 남았다.
Metal 모델 버퍼는 544.02MiB였다. 가중치는 소유 메모리로 읽었고 재양자화나 변환은
수행하지 않았다. 성공 실행에는 mmap 권고를 가로채는 별도 라이브러리도 사용하지 않았다.

실행 결과·네이티브 토큰 로그·에이전트 transcript는 Workspace의
`build/nemotron-agent-20261023/`에 보관한다. 기본 데몬의 `models.load`는 앞선
검사에서 클라이언트 시간 초과였으며, 이번 성공은 명시한 호스트 Engine 경로에
해당한다. 설치본 교체, 기본 Service의 예산 정책, 이미지·음성 입력은 검증 범위에 없다.

## 앞선 실패와 원인 구분

레이어별 전문가 수 수정 전에는 `blk.1.ffn_gate_inp.weight`에서 496개와 240개의
차원 불일치로 실패했다. 수정 후 CPU 모델과 컨텍스트는 생성되었지만 mmap을 쓴
CPU 생성은 1,800초 안에 완료되지 않았다. 일부 검사에서 큰 `MADV_WILLNEED`
권고만 미루는 동적 라이브러리와 일시적인 페이지 고정을 사용했으나 답장은 받지 못했다.
페이지 고정은 해제하고 보조 프로세스도 종료했다.

전체 Metal 오프로딩 시도는 `IOGPUMetalResidencySet::requestResidency`에서 대기했다.
종료 신호 뒤 커널 종료 상태가 오래 유지된 이전 검사 프로세스도 최종적으로 사라졌다.
상주 힌트만 끈 다음 시도는 모델 로드 후 실제 `llama_decode`에서
`kIOGPUCommandBufferCallbackErrorOutOfMemory`를 반환했다. 소유 메모리 읽기와
오프로딩 제한을 함께 적용한 위 실행은 답장을 생성하고 정상 종료했다.
이는 모델 데이터 결함과 구분해야 하는 로더·메모리 운용 문제이다.

SDK 최종 빌드와 기본 회귀 검사 23개가 모두 통과했다. 회귀 실행 시간은 262.52초이며,
기존 llama.cpp·MLX·CLI·대화·데몬 검사와 새 Metal top-9·소유 메모리 검사를 포함한다.
최종 결과는 `final-validation-summary.json`, `final-regression.xml`에 기록한다.
앞선 21개 회귀 실행의 MLX 시간 초과 2건과 수정 도중의 파일 읽기·시작 시간 초과
로그도 보존했다. 최종 실행에서는 이 검사들이 모두 통과했다.

대형 모델 회귀는 선택적 fixture를 명시한 경우에만 등록된다. 이번 검사 뒤 기본
`build/`의 fixture 경로 옵션을 비워 일상적인 회귀 실행에서 대형 모델 검사가
자동으로 수행되지 않도록 복구했다.

## 제한된 메모리에서의 호스트 설정

`createLlamaRuntime(LlamaRuntimeOptions)`는 호스트가 가중치 저장 방식과 GPU
레이어 상한을 선택하는 추가 생성 함수이다. 기본 생성 함수의 정책은 유지한다.
`memoryMapWeights=false`는 mmap 대신 소유 메모리로 가중치를 읽는다.
`maxGpuLayers=-1`은 기존 전체 오프로딩이며, 0 이상의 값은 오프로딩 레이어
상한이다. 0이면 컨텍스트와 projector도 CPU를 사용한다. -1 미만은 거부한다.
이 값들은 모델 manifest나 생성 요청의 선택 사항이 아니며, 클라이언트의
장치 선택 금지 계약은 유지한다. Service의 메모리 예산을 변경하거나 가중치를
변환하지 않는다. 호스트는 사용 가능한 메모리에 맞는 구성을 선택해야 한다.

대형 mmap 모델을 Metal에 연결하면 전체 파일 범위의 상주 또는 명령 제출 단계에서
메모리 대기가 발생할 수 있다. `GGML_METAL_NO_RESIDENCY=1`은 네이티브 장치 초기화
전에 해당 호스트 프로세스에만 적용할 수 있는 upstream 옵션이다. 이는 상주 힌트를
끄며, 실제 GPU 버퍼에 필요한 메모리나 생성 연산을 생략하지 않는다.

Metal 배치 MoE의 top-k=9 커널 누락도 작은 행렬에서 재현했다. 9개 전문가와
64개 토큰을 사용하는 계산은 수정 전 누락된 커널 이름을 출력하고 종료 코드 139로
실패했다. upstream [#30699](https://github.com/ggml-org/llama.cpp/pull/30699)의
한 줄 specialization을 다운로드된 `build/` 의존성 사본에 적용한다. 원본 shader는
`build/llama-patches/mul_mm.original.metal`에 보존한다. 외부에 명시한 소스 체크아웃은
수정하지 않으며, 해당 체크아웃에 수정이 없으면 구성 단계에서 거부한다.
`iiLocalLLM.metal_top9`는 실제 GPU 결과 73,728개를 수학적으로 알려진 값과 비교한다.
이 배치 커널 수정은 별도의 회귀로 검증했으며, 22토큰 인사의 메모리 부족 오류와
같은 원인으로 단정하지 않는다.
`iiLocalLLM.llama_resident_inference`는 별도 작은 GGUF로 메모리 복사 방식의 CPU
생성과 다음 턴의 캐시 재사용을 검증한다.

호스트는 `validateTensorData=true`로 네이티브 로더의 텐서 데이터 검사를 활성화할 수
있다. 검증 실행은 이를 사용하며, 부적합한 텐서가 검출되면 생성하지 않고 실패한다.
이 검사는 인사 답변의 품질이나 모델의 모든 기능을 보장하지 않는다.
