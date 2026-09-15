# 포화 상태의 제어 요청

0.28은 0.27의 독립 API/MCP 제어 작업 풀에 HTTP 응답 용량을 연결한다. 기존 cpp-httplib의 Response.user_data와 C++ 원자적 카운터를 재사용하며 새 생산 의존성은 추가하지 않는다. Response 수명은 헤더·본문·SSE 전송이 끝나거나 실패할 때까지 이어지므로 일반 JSON 응답도 용량 계산에 포함한다.

HttpOptions.workerThreads는 동시에 허용하는 일반 응답 수이다. maxControlRequests는 별도의 제어 응답 수이며 기본 2, 범위 1–16이다. 내부 HTTP 작업자 상한은 workerThreads + maxControlRequests + 4이다. 추가 4개는 요청 파싱·초과 요청 거부·health 등 짧은 전송 작업에 사용한다. 일반 작업, 제어 작업 모두 한도를 넘으면 429를 반환하며 접수하지 않는다. 일반 요청이 점유한 용량으로 제어 요청을 거부하지 않는다.

RpcHandler.isControlMethod는 신뢰된 호스트가 분류한다. 기본은 false이다. agent::Api는 agent.permissions.pending/respond, agent.cancel/status, agent.hooks.status/cancel과 agent.plan.get을 제어 메서드로 지정한다. 자격 증명과 인자 검증은 기존 dispatch에서 수행한다. JSON의 역할·메타데이터·이름 접두사로 제어 용량을 획득할 수 없다. 모델 생성과 /v1/models도 일반 용량에 포함한다.

MCP HTTP의 maxStreams와 maxStreamsPerSession은 일반 활성/보관 스트림을 제한한다. 보관 개수에는 background 스트림이 포함된다. maxControlStreams는 별도 전역 활성 제어 스트림 한도와 연결별 보관 제어 스트림 한도이며 기본 4, 범위 1–64이다. 0.34는 내부 작업자 maxStreams + maxControlStreams + 4개를 시작할 때 생성한다. 등록된 ServerOptions.controlHandlers 메서드와 ping만 제어 응답 스트림을 사용한다. 일반 요청이 섞인 2025-03-26 배치는 일반 용량을 사용한다. 알림·역방향 응답·DELETE는 기존처럼 응답 스트림을 만들지 않는다.

제어 스트림의 재접속도 원래 스트림의 분류를 따른다. 다른 소유자나 위조 메타데이터로 분류를 바꿀 수 없다. 완료 이력 제거는 같은 분류의 용량을 확보하고, 전체 이벤트/바이트 보관 한도는 기존처럼 공유한다. 소켓 종료 자체가 MCP 작업 취소를 뜻하지 않는 계약도 유지한다.

호스트 실행 파일도 같은 한도를 설정한다. iiLocalLLMD는 --http-workers(기본 8), --http-control-requests(기본 2), iillm-mcp는 --max-streams(기본 64), --max-control-streams(기본 4), --max-streams-per-session(기본 64)을 받는다. 명시한 용량 옵션은 --http-port와 함께 사용한다. 잘못된 숫자·범위·조합은 모델 초기화 전에 거부한다. MCP --request-timeout은 승인 대기와 모델 실행을 포함할 만큼 길게 지정한다.

0.28에서 공개 C++ 옵션 구조체와 RpcHandler 가상 인터페이스가 변경되었다. 현재 C++ 소비자는 ABI 0.34의 헤더와 라이브러리를 함께 사용해 다시 빌드해야 한다. 기존 바이너리에 라이브러리만 교체하는 호환성을 보장하지 않는다.

tests/http_tests.cpp는 일반·제어 독립 한도, 대기 승인, 느린 JSON/SSE 전송과 소켓 종료 후 용량 회수를 검사한다. tests/mcp_http_server_tests.cpp는 일반 활성·보관 용량이 모두 찬 상태의 제어 처리를 검사한다. 독립 표준 라이브러리 HTTP 클라이언트인 tests/mcp_http_server_wire.py는 혼합 배치·위조 분류, 두 종류 용량 포화 중 취소, 미완료 연결 종료 뒤 보관 한도와 재접속 소유권을 검사한다. tests/permission_requests_wire.py의 --control-capacity는 실제 daemon/MCP HTTP의 일반 용량을 1개로 제한한다. 선택적 --catalog로 로컬 모델의 Write 승인도 같은 HTTP 경로에서 검사한다. 실행 결과와 설치 소비자 증거는 [Verification.md](Verification.md)에 기록한다.

보장은 정상적으로 파싱되어 접수되는 애플리케이션 작업의 일반/제어 용량 분리이다. 연결 큐, 인증 처리, 읽기·쓰기 기한, 메시지/세션/이벤트 총량, 제어 용량 자체는 여전히 제한된다. 임의의 호스트 콜백 중단이나 무제한 트래픽 수용을 보장하지 않는다. 제어 메서드는 짧고 제한된 작업으로 구현해야 한다. 전체 하네스와 실제 앱 통합 검증은 [HarnessParity.md](HarnessParity.md)의 남은 범위를 따른다.

0.34에서 HTTP·MCP의 처리 스레드를 설정된 최대 일반/제어 용량과 초과 요청 처리 여유분만큼 처음부터 확보한다. 동적 풀의 초기 idle 집계 경합으로 제어 요청이 긴 작업 뒤에 대기하는 상황을 실제 계획 검토 검사에서 재현하여 수정했다. 최대 용량은 늘리지 않는다. [PlanMode.md](PlanMode.md)에 계약과 남은 범위를 기록한다.
