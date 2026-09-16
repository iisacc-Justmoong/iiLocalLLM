# 파일 체크포인트와 복원

0.46은 C++ `FileCheckpoints`와 Engine의 사용자 메시지 경계를 연결한다. 참조는 `Exhen/claude-code-2.1.88`의 고정 커밋 `c8cd253554319f32ff64ff7000636199f720c9bc`, `source/src/utils/fileHistory.ts`, `cli/print.ts`의 `rewind_files`, `entrypoints/sdk/controlSchemas.ts`이다. 해당 구현의 동작을 분석하고 iiLocalLLM의 Qt·세션·권한 계약으로 구현했다. 소스 복사나 TypeScript 실행 의존성을 추가하지 않는다.

## 기록 계약

- 임베디드 호스트는 `EngineOptions::fileCheckpointsEnabled=true`로 활성화한다. daemon과 agent-enabled MCP는 기본 활성화하며 `--agent-no-file-checkpoints` / `--no-file-checkpoints`로 끈다.
- 승인된 직접 사용자 입력과 큐의 prompt가 전달되면 메시지 ID로 체크포인트를 만든다. 거부된 입력, notification, Stop/SessionStart 훅의 합성 문맥, 수동 압축은 사용자 체크포인트를 추가하지 않는다.
- 네이티브 Write·Edit·NotebookEdit가 저장되기 전에 원본을 기록한다. 같은 메시지에서 같은 파일을 여러 번 수정해도 최초 원본을 유지한다. 다음 메시지는 추적 파일들의 현재 상태를 기록한다.
- 파일이 없던 상태도 저장한다. 과거 체크포인트에 해당 파일 기록이 없으면 현재 작업 범위에서 최초로 관찰한 원본을 사용한다. 새 파일은 복원 시 삭제하며 0바이트 파일도 포함한다.
- 직접 C++/MCP 네이티브 수정은 별도 UUID 체크포인트를 만든다. `checkpointFiles()`로 수정 전에 명시적인 경계를 추가할 수도 있다. 이러한 ID는 사용자 메시지 ID가 아니며 대화 기록을 추가하지 않는다.
- `.ipynb`는 파일 전체 원문으로 복원한다. CRLF, 바이너리 바이트, QFile 권한 비트를 보존한다. 자동 추적되는 네이티브 편집은 기존 UTF-8/1 MiB 계약을 그대로 따른다.
- 계획 파일과 프로젝트 메모리는 해당 기능의 별도 수명에 속하므로 작업 파일 복원에서 제외한다. 자식 에이전트는 자식 세션에 기록하며 부모 체크포인트로 합치지 않는다.

## 저장·경계·실패

`sessionsDirectory/file-checkpoints/<session UUID>/state.json`과 SHA-256 주소의 `.blob`을 사용한다. Qt의 QSaveFile·QLockFile·QCryptographicHash를 재사용한다. 이 기능에 새 외부 라이브러리를 도입할 이유가 없어 의존성을 추가하지 않았다. 파일 내용은 중복 저장하지 않고, 새 manifest를 커밋한 뒤 도달할 수 없는 blob만 정리한다.

세션당 최근 100개 체크포인트, 전체 범위 합계 256개 추적 파일, 파일당 1 MiB, manifest 16 MiB, blob 합계 64 MiB로 제한한다. 체크포인트 sequence는 삭제 후에도 증가한다. 최초 원본은 오래된 체크포인트의 fallback에 필요하므로 참조가 남는 동안 보존한다. 기록 실패 시 네이티브 편집도 진행하지 않는다.

복원 preview JSON은 512 KiB까지 허용하며 초과하면 파일을 바꾸기 전에 거부한다. 도구의 text는 짧은 상태 설명이고 전체 변경 목록은 structured data에 한 번만 담는다.

모델 실행·직접 네이티브 파일 호출·복원은 같은 세션 lease를 사용한다. C++에서 직접 workspace ToolRunner를 연결할 때는 `bindWorkspaceContext(context, true)`의 guard를 호출이 끝날 때까지 보관한다. 파일 접근과 관계없는 기존 scope는 체크포인트 활성화만으로 추가 lease를 얻지 않는다. 동시 파일 작업은 busy 오류를 반환한다. 별도의 history lock도 프로세스 간 journal 쓰기를 보호한다. 압축 이후에도 파일 기록을 유지하고, 재시작은 manifest를 다시 읽는다.

체크포인트는 현재 canonical 작업 디렉터리와 worktree revision에 묶인다. 다른 작업 트리나 이전 revision의 기록은 현재 범위에서 복원할 수 없다. 변경된 additional directories와 host-private 경계, 네이티브 Write 경로 준비 및 현재 Write deny 규칙을 다시 적용한다. `RewindFiles`는 기존 ToolRunner의 훅·권한·Plan 모드·취소를 거치며 기본 Ask 정책에서는 호스트 승인 채널이나 명시적 허용 규칙이 필요하다. MCP wrapper의 허용과 실제 `RewindFiles`의 허용은 별도이다.

DontAsk 정책에서는 복원 대상에 대한 `Write` 허용도 필요하다. `NotebookEdit`만 허용된 파일을 복원하기 위해 Write 거부를 우회하지 않는다. CLI 호스트의 좁은 경로 규칙으로 필요한 복원 범위를 허용할 수 있다. `iillm-mcp --model`은 연결 제어 wrapper만 호스트 규칙으로 허용하며, 실제 `RewindFiles` 및 파일 쓰기 검사는 유지한다.

복원 전 모든 대상과 원본 blob의 크기·해시를 검사한다. 경로나 조상 symlink, 디렉터리/비정규 파일, 범위 밖 경로, 손상된 journal/blob은 거부한다. 권한 질문에서 보여 준 preview fingerprint와 실제 복원 직전 상태가 달라져도 중단한다. 복원 뒤에는 기존 Read 관찰을 폐기하므로 다음 편집 전에 다시 읽어야 한다.

파일 하나의 저장은 원자적이나 전체 파일 집합의 트랜잭션은 아니다. 실행 중 외부 쓰기·취소·IO 실패는 앞서 복원된 파일을 되돌리지 않으며 `complete=false`, `filesRestored`, `errors`로 실제 결과를 반환한다. 이 구현은 동일 사용자 권한의 적대적 프로세스에 대한 OS 파일시스템 샌드박스가 아니다.

## C++ / API / CLI / MCP

```cpp
auto history = engine.fileCheckpoints(sessionId);
auto point = engine.checkpointFiles(sessionId); // 선택적 수동 경계
auto preview = engine.rewindFiles(sessionId, point["message_id"].toString(), true);
auto result = engine.rewindFiles(sessionId, point["message_id"].toString());
// ToolResult::isError 및 data.complete/filesRestored/errors 확인
```

인증 HTTP `/v1/rpc`와 native IPC는 동일한 메서드를 제공한다.

| 메서드 | 파라미터 |
|---|---|
| `agent.checkpoints.list` | `session_id` |
| `agent.checkpoints.create` | `session_id` |
| `agent.checkpoints.rewind` | `session_id`, `message_id`, 선택 `dry_run` boolean |

`agent.info.file_checkpoints_enabled`가 기능 가용성을 표시한다. rewind는 기존 도구 응답 형태인 `text`, `result`, `is_error`를 반환한다. `result`에는 `canRewind`, `message_id`, `dryRun`, `complete`, `filesChanged`, `changes`, `filesRestored`, `fingerprint`가 있으며 실제 적용 결과에 `errors`가 포함된다. 현재 `changes`는 생성·수정·삭제와 바이트 크기를 제공하며 행 삽입/삭제 통계는 제공하지 않는다. checkpoint를 찾을 수 없거나 preview 자체가 실패하면 ToolResult 오류이다.

```sh
iillm --auth-file FILE agent checkpoints list SESSION
iillm --auth-file FILE agent checkpoints create SESSION
iillm --auth-file FILE agent checkpoints rewind SESSION PARAMS_JSON_FILE
```

MCP는 `experimental["iisacc/fileCheckpoints"]`와 `iiLocalLLM.agent.checkpoints.list/create/rewind`를 공개한다. MCP 도구의 인자는 API와 같지만 `session_id`를 받지 않으며 연결 소유자에 고정한다. 동일 네이티브 Write/Edit/NotebookEdit 핸들러가 원본을 기록한다. 임의 세션 ID나 백업 경로를 wire 입력으로 받지 않는다.

## 남은 참조 대응

대화 자체 rewind/메시지 선택 UI, 변경 행 수·IDE 알림, 파일 artifact를 가진 세션 fork/resume-copy, 부모·팀과 자식의 공동 체크포인트, Bash의 제한된 simulated-sed 경로 연결은 아직 구현되지 않았다. 일반 Bash/외부 프로그램의 미추적 파일 수정은 포착하지 않는다. 이미 추적된 파일의 다음 사용자 경계 상태만 포착한다. 외부에서 만든 수정도 복원 대상 원본과 다르면 덮어쓸 수 있으므로 preview의 변경 파일을 확인해야 한다.

기존 artifact fork 제한을 유지하며 새 파일만 기록된 세션도 기록을 조용히 버리는 fork 대신 명시적 미지원 오류를 반환한다. files와 sessions의 대응 상태는 partial이다. SDK 검증은 제품 앱 재설치나 모든 플랫폼의 실기기 동작을 의미하지 않는다.
