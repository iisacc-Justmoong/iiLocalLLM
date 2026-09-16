# 세션 분기와 보관 파일 복제 (0.47)

파일을 편집하거나 큰 도구 결과를 저장한 대화도 분기할 수 있다. C++ `Engine::forkSession`은 원본 세션의 lease를 잡고 대화·압축 경계·보관 파일·파일 체크포인트를 복제한 다음 새 transcript를 공개한다. 새 UUID와 `parent_session_id`를 사용한다. 원본 대화나 현재 작업 파일은 수정하지 않는다. 두 세션은 같은 작업 디렉터리를 사용하므로 이후 작업 파일 편집은 서로 보인다.

## 복사 범위

- 전체 분기: 메시지와 압축 기록 전체, 세션 `artifacts` 아래 일반 파일 전체, 수동 생성분을 포함한 체크포인트 전체를 복제한다.
- `through_message_id` 분기: 해당 메시지까지 포함한다. 도구 호출과 결과가 끊기는 경계는 거부한다. 남긴 메시지·시스템 프롬프트·압축 요약이 참조하는 보관 파일만 복제한다. 체크포인트는 남긴 메시지 ID에 대응하는 것만 보존하고 이후에만 추적된 파일은 제외한다. transcript 메시지에 대응하지 않는 수동/직접 편집 체크포인트는 이 형태의 분기에서 제외한다.
- 메시지의 text·도구 인자·data·content·metadata와 시스템 프롬프트·압축 요약에 있는 원본 artifact 경로를 새 세션 경로로 바꾼다. 일반 작업 파일 경로와 메시지/도구 호출 ID는 유지한다. 보관 파일의 원시 바이트·권한은 보존하며 파일 내용 안의 임의 경로는 수정하지 않는다. 경로 인식은 네이티브 도구가 기록하는 원시 절대 경로 기준이며 임의 인코딩 URL·binary 참조는 해석하지 않는다.
- 체크포인트의 workspace/revision 범위를 보존한다. 분기는 worktree 소유권이나 revision을 이전하지 않는다. 새 세션은 원래 작업공간에서 시작하며 현재 범위 밖의 이력은 복원할 수 없다.
- 런타임에서 체크포인트 기능을 꺼도 이미 저장된 이력은 복제한다. 다시 활성화한 Engine에서 읽을 수 있다.

복사본은 하드링크를 쓰지 않는다. 부모 보관 파일·blob의 삭제나 덮어쓰기는 자식 백업에 반영되지 않는다. `FileCheckpoints::fork`는 참조의 크기와 SHA-256을 확인하고 필요한 blob만 복사한다. 현재 작업 파일을 새로 스냅샷하거나 복원하지 않는다.

원본에서 이미 사라진 artifact를 복구하거나 모든 문자열 참조의 존재를 검증하지는 않는다. 누락된 artifact 참조는 분기 뒤에도 유효한 파일을 가리키지 않을 수 있다. 체크포인트는 필요한 blob이 없거나 손상되면 분기 전체를 거부한다.

`Agent.fork_context`도 부모의 불변 문맥이 참조하는 artifact를 자식 저장소로 복제한다. 미완료 부모 도구 호출은 기존 계약대로 미실행 표시로 짝을 맞춘다. 부모 체크포인트 전체를 넘기지는 않으며 자식 편집은 자식 이력을 만든다.

## 공개와 실패

Qt 6.8.3의 QSaveFile, QLockFile, 파일·JSON API를 재사용한다. 세션별 저장 계약에 속하는 복제이므로 새 의존성은 추가하지 않는다. artifact는 최대 4096개 디렉터리 항목, 32단계, 파일당 64 MiB, 복사 합계 256 MiB이다. 체크포인트는 기존 100개/256개 파일/64 MiB blob 한도를 유지한다. symlink·비정규 파일·손상된 blob·용량 초과를 거부하며 파일 복사 전후 크기·수정/메타데이터 시각·권한도 검사한다.

정상 오류 경로에서는 미완성 artifact·체크포인트·계획 파일·상속된 런타임 권한을 정리하고 새 세션을 공개하지 않는다. 복사 후 transcript의 원자적 게시까지 성공해야 완료이다. `SessionStore::fork`의 `beforePublish` 콜백은 이 경계를 제공한다. 외부 상태를 추가하는 호스트는 콜백 성공 이후 transcript 게시가 실패할 경우까지 정리해야 한다.

여러 디렉터리를 하나의 트랜잭션으로 커밋하지는 않는다. 강제 종료/전원 차단은 transcript 없는 소유 디렉터리를 남길 수 있으며 자동 고아 정리는 아직 없다. 목록은 완성된 transcript만 반환한다. 동일 OS 사용자의 적대적 동시 파일 교체를 차단하는 샌드박스도 아니다.

원본 실행 중이나 다른 프로세스의 lease 점유 시 분기를 거부한다. 현재 권한을 상속하고 계획 파일은 별도 사본으로 만들며 새 계획 승인이 필요하다. 백그라운드 프로세스·입력 큐·Task/Todo·승인 요청·LSP 프로세스·worktree·과거 Read 관찰은 복사하지 않는다.

## C++ / API / CLI / MCP

```cpp
auto child = engine.forkSession(parentId);
auto prefix = engine.forkSession(parentId, messageId);
```

API `agent.sessions.fork`는 소유 `session_id`와 선택 `through_message_id`를 받는다. 새 `session_id`, `parent_session_id`, 메시지/압축 개수를 반환한다. 인증된 소유자만 호출할 수 있으며 기존 부모도 계속 사용할 수 있다.

C++ 소비자는 0.47 헤더와 라이브러리로 함께 재빌드한다. `SessionStore`의 공개 함수 인자가 늘어났으므로 이전 바이너리의 ABI 호환을 보장하지 않는다.

```sh
iillm --auth-file TOKEN_FILE agent sessions fork SESSION_ID
iillm --auth-file TOKEN_FILE agent sessions fork SESSION_ID PARAMS_JSON_FILE
```

MCP `iiLocalLLM.agent.fork`는 연결의 현재 대화를 분기하고 새 대화로 전환한다. 선택 `through_message_id`만 받으며 외부 `session_id`는 받지 않는다. 같은 연결에 다른 호출이 진행 중이면 거부한다. 실패 시 현재 대화를 유지한다. `experimental["iisacc/sessionFork"]`로 발견한다. 독립 `iillm-mcp`는 이 호스트 제어를 허용하고 임베디드 서버는 호스트 정책을 적용한다. 기본 모델 도구로는 등록하지 않는다.

MCP 네이티브 파일 도구도 Engine 세션의 artifact 디렉터리를 사용하므로 직접 편집 백업이 전체 분기에 포함된다. 연결 종료 시 현재 대화와 해당 연결에서 분기한 부모의 수명을 종료한다. 부모 transcript는 보존하지만 MCP에서 과거 부모로 다시 전환하는 선택 API는 아직 없다. 부모 백그라운드 작업은 자식에게 이전하지 않는다.

## 참조와 검증

기준은 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/utils/fileHistory.ts:922` `copyFileHistoryForResume`이다. 참조는 이전 백업을 새 세션으로 hardlink하고 실패 시 copy하며 백업 이전에 성공한 snapshot만 기록한다. iiLocalLLM은 독립 복사와 새 transcript 게시 전 전체 실패 처리를 사용한다. Claude SDK와 동일 wire schema라고 주장하지 않는다.

참조의 경로들을 같은 구현으로 취급하면 안 된다. `screens/REPL.tsx:1791`은 fork의 계획 파일을 별도로 복사하고, `log.fileHistorySnapshots`가 있을 때 백업 이전을 호출한다. `utils/sessionRestore.ts:470`은 fork가 원본 worktree를 소유하지 않도록 해당 메타데이터를 제외한다. `commands/branch/branch.ts:102`는 메인 대화 메시지와 content-replacement 기록을 새 transcript에 쓰며 파일 이력 레코드는 그 복사 목록에 없다. `entrypoints/agentSdkTypes.ts:268`의 공개 `forkSession`은 설명 주석과 달리 실제 본문이 미구현 오류를 던진다. 따라서 공개 SDK의 분기 구현이 완성되어 있다고 인용하지 않는다.

`session_fork_tests.cpp`는 바이트/권한·경로·메시지 경계·부모 삭제 후 복원·손상/용량/권한 상속 실패 정리를 검사한다. API·체크포인트·subagent 테스트도 확장했다. `notebook_wire.py --forks`와 `notebook_runtime_smoke --forks`로 실제 전송 및 모델의 자식 백업 읽기를 검증한다. 결과는 [Verification.md](Verification.md)에 기록한다. 대화 rewind UI·팀 공동 이력·원격 재개·전체 앱/플랫폼 검증은 남아 있다.
