# Windows 실행

Qt 6.8의 MinGW-w64 헤더에 없는 Windows 10 스레드 전력 제어 ABI를 호환 헤더로 제공한다. 새 Windows SDK에서는 기존 정의를 사용하며 CPU llama.cpp 런타임을 그대로 빌드한다. `build/`의 native grammar 및 CPU 런타임 테스트로 연결과 실행을 검증한다.
# 네이티브 메모리 조회

Windows의 RAM 조회는 최소 Windows 헤더를 사용한다. Qt 헤더와 전체 RPC 헤더의 중복 정의를 피하면서 `GlobalMemoryStatusEx`를 실제로 호출하며, Windows 런타임 테스트는 사용 가능한 RAM 값이 존재하고 양수인지 확인한다.
