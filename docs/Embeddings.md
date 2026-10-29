# 텍스트 임베딩

`Service::embedText(model, text, cancellation)`는 로컬 모델의 텍스트 임베딩 벡터를
비동기적으로 반환한다. 모델 manifest에는 `embedding` capability가 명시되어야 한다.
모델 설치·검증·메모리 예약·장치 선택은 기존 Service와 ModelManager가 소유하며,
호출만으로 모델을 다운로드하지 않는다. 호출은 기존 스케줄러에서 직렬화되며 취소,
비정상 차원, 0 벡터, 비유한 좌표를 오류로 반환한다.

교체 가능한 런타임은 `RuntimeModel`과 별도의 `EmbeddingRuntimeModel`을 함께 구현한다.
해당 인터페이스가 없는 런타임은 명시적으로 미지원 오류를 반환한다. llama.cpp GGUF
백엔드는 임베딩 컨텍스트에서 모델을 평가하고 문장 단위의 벡터를 반환한다. 모델의
기본 pooling을 사용하며 호스트가 모델 로드 옵션 `embedding_pooling`에 `model`,
`mean`, `last`, `cls`를 명시할 수 있다. 모델이 pooling을 정의하지 않았거나 순위 지정
전용 pooling을 사용하는 경우에는 오류를 반환한다. encoder-decoder 결합 모델은 현재
미지원이다. 입력은 최대 모델 문맥과 4,096 토큰 중 작은 한도를 초과하면 잘리지 않고
거부된다. query/document 접두어나 정규화 방식은 모델별 계약에 따라 호스트가 제공한다.

임베딩 컨텍스트의 메모리를 기존 예약 범위에서 사용하기 위해 동일 모델의 캐시된 생성
컨텍스트는 임베딩 직전에 해제된다. 대화 기록과 세션은 삭제하지 않으며 다음 생성에서
컨텍스트를 다시 만든다. 이 경로는 생성·임베딩 모델이 모두 해당 capability를 선언한
경우에도 동일하게 적용된다.
현재 각 입력마다 임베딩 컨텍스트를 새로 만들므로 대량 색인의 시간과 메모리 비용은
사용할 모델과 장치에서 별도로 측정해야 한다.

```cpp
auto vector = service.embedText("model://my-embedding-model", "query: original artwork").get();
```

`iiLocalLLM.service`는 모델 대역으로 capability 검사, 취소, 벡터 검증, 모델 해제와
세션 유지 동작을 검사한다. 실제 GGUF 임베딩 모델의 로딩·벡터 품질·장치 실행은 별도의
명시적인 실행 검증이 필요하며 이 자동 시험만으로 확인되지 않는다.
