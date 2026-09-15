# 설치 검증

호스트 재설치는 현재 소스를 빌드하고 기본 회귀 테스트 및 독립 설치 소비자를 실행한다.
실제 모델을 사용하는 `inference` 라벨 검사는 모델 응답 평가 결과로 따로 기록한다.

```sh
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release -LE inference --output-on-failure
```

`AgentTransportTests::timeoutAndShutdown`은 실행 중인 요청의 HTTP 기한 만료와 서버
종료 시 취소를 검사한다. 가짜 모델은 취소될 때까지 기다린다. 세션 생성은 기존
IPC로 준비하여 디스크 작업과 검사 대상인 HTTP 실행의 100ms 제한을 분리한다.
HTTP 504, 모델 취소 횟수, 후속 IPC 실행과 종료 취소에 대한 단언을 유지한다.
이 기한은 테스트 fixture 설정이며 생산 서버의 기본값을 변경하지 않는다.
