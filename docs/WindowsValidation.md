# Windows 실행 검증

Bash 도구는 Windows에서도 Bash 명령 계약을 유지한다. Git for Windows의 bin/bash.exe를 PATH에 둔다. LSP fixture의 표준 입출력은 binary 모드로 UTF-8 Content-Length 프레임을 보존한다. 링크 테스트는 .lnk 대신 실제 Windows 심볼릭 링크를 만들며 HTTP peer fixture는 소켓 전용 select 대신 reader thread와 Queue로 파이프를 읽는다.

Windows의 권한 오류 fixture는 POSIX chmod 대신 실제 파일 공유 모드로 read/write 또는 rename을 거부한다. private path 비교는 Windows의 대소문자 비구분 규칙을 따르며 managed fragment의 점 접두 파일을 명시적으로 제외한다.

레거시 transcript migration은 파일 lease를 닫은 다음 QSaveFile의 atomic commit을 수행한다. Windows worktree 경로 fixture는 공백을 포함하되 Win32가 정규화하는 끝 공백은 사용하지 않는다. 기존 compaction 및 sparse checkout 회귀 테스트로 검증한다.

Windows에서는 Git for Windows Bash로 전경·백그라운드 명령을 실행하며 Job Object의 KILL_ON_JOB_CLOSE로 자식 프로세스를 함께 정리한다. Bash AST 권한 규칙은 동일하게 적용된다. transcript 읽기는 파일이 속한 볼륨의 부모 디렉터리를 기준으로 하여 C:와 D: 사이의 경계 오판을 방지한다. ShellTasks, 권한 규칙, 검증 훅 및 디렉터리 심볼릭 링크 교체 회귀 테스트를 실행한다. Windows Job 종료는 POSIX TERM trap을 전달하지 않으므로 trap 출력 사례는 POSIX 전용이며, Windows에서도 종료 후 자식 프로세스와 지연 파일 생성이 남지 않는지 검증한다.

Teams 재시작에서는 이전 JSON 레코드를 읽은 핸들을 닫은 뒤 원자적 교체를 수행한다. worktree 셸 테스트는 Git Bash의 /d/... 경로 표현과 네이티브 D:/... 메타데이터를 각각 확인한다. HTTP 지연 읽기 테스트는 Windows TCP 연결 수립 전 수신 버퍼를 제한해 실제 backpressure를 만든다.

Windows 셸의 초기 스레드를 CREATE_SUSPENDED로 생성하고 Job Object에 배정한 뒤 재개하여 매우 짧은 명령의 종료 경합과 자식 생성 경합을 방지한다. Git MSYS ps의 PID 표로 살아 있는 프로세스를 검사하고, MSYS가 반환한 시그널 종료의 shifted wait status는 정상 종료 코드로 노출하지 않는다. 복구 시 ShellTasks와 Subagents도 입력 레코드 핸들을 닫은 뒤 원자적으로 교체한다. 근거: https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects 및 https://doc.qt.io/qt-6.8/qprocess-createprocessarguments.html

CommandHooks의 명령 admission은 Windows 데스크톱의 Git for Windows Bash를 허용하며, SessionEnd 테스트가 명령 실행과 저장 완료 후 실행을 검증한다. 16개 동시 쓰기 테스트는 데이터 유일성과 원자적 claim을 유지하면서 부하 상태에서도 잠금 획득을 기다릴 수 있도록 명시적 15초 fixture 예산을 사용한다.

큰 JSON 응답은 httplib의 헤더 버퍼에 본문 전체를 다시 복사하지 않고 Content-Length provider를 통해 64KiB 단위로 전송한다. 느린 JSON 및 SSE 클라이언트, 별도 제어 요청의 용량 유지와 연결 종료 후 반환을 실제 TCP 소켓으로 검증한다.
