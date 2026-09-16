# 세션별 작업 트리

0.44의 C++ `EnterWorktree`·`ExitWorktree`는 같은 대화에서 파일 도구·전경/백그라운드 셸·프로젝트 지침·권한 설정이 사용하는 실행 경로를 바꾼다. 프로세스 전역 cwd는 변경하지 않는다. 세션의 디스크 헤더에 기록된 원래 작업 공간은 인증 식별자로 유지하고, 실행 경로는 소유 작업 트리 상태에서 복원한다.

## 호출 계약

- `EnterWorktree({name?})`: 작업 트리를 만들거나 같은 소유자가 보존한 이름을 재개한다. 이름 생략 시 임의 이름을 권한 미리보기 단계에서 확정한다. 이름은 1–64자의 `/` 구분 영문·숫자·점·밑줄·대시이며 `.`·`..`·빈 segment와 Git에서 유효하지 않은 브랜치는 거절한다. 경로와 브랜치에는 `/`를 `+`로 바꾸며 브랜치는 `worktree-<name>`이다.
- `ExitWorktree({action:"keep"})`: 원래 실행 경로로 돌아오고 파일과 브랜치를 보존한다. 작업 트리가 외부에서 사라지거나 브랜치가 바뀐 경우에도 원래 경로가 존재하면 보존 종료할 수 있다.
- `ExitWorktree({action:"remove",discard_changes?:boolean})`: 소유 디렉터리와 브랜치를 삭제한다. 변경·untracked·ignored 파일 또는 생성 기준보다 추가된 커밋이 있으면 `discard_changes:true`가 필요하다. 이 값은 도구 인수이며 호스트 권한 검사를 우회하지 않는다.

기존 동명 브랜치를 `-B`로 초기화하거나 다른 세션의 작업 트리를 인수하지 않는다. Git 루트·공유 Git 디렉터리·작업 트리 Git 디렉터리·브랜치를 삭제 직전에 확인한다. 권한 미리보기와 실행 사이에 상태 revision, HEAD, 변경 경로 또는 파일 내용이 달라지면 삭제를 거절한다. 브랜치 삭제는 예상 HEAD를 지정한 `git update-ref --no-deref -d`로 수행한다. 잠긴 작업 트리에 이중 `--force`를 사용하지 않는다.

변경 내용 스냅샷은 10,000개 경로와 합계 64 MiB로 제한한다. 더 크거나 읽을 수 없는 변경, 특수 파일은 상태 진단에 표시하고 자동 삭제하지 않는다. Git 변경 내용 스냅샷을 완성하지 못하면 삭제 미리보기부터 거절하므로, 이후 상태가 달라져도 불완전한 미리보기를 삭제에 재사용하지 않는다. `keep`은 이 내용 스냅샷의 성공을 요구하지 않는다. 외부 프로세스의 동시 쓰기를 차단하는 OS 샌드박스는 아니다.

## 저장과 실패 처리

기본 위치는 `<주 저장소 부모>/.iilocal-worktrees/<저장소 경로 해시>/<name>`이다. 호스트는 다른 절대 경로를 지정할 수 있다. 참조의 `.claude/worktrees`와 다른 기본 위치는 원본 체크아웃에 중첩 저장소를 추가하지 않기 위한 것이다. 원본의 미커밋 파일은 자동 복사하지 않는다.

기준 커밋은 명시한 `baseRef`, 캐시된 `origin/HEAD` 또는 `origin/main`·`origin/master`, 원격 기본/현재 브랜치 fetch, 로컬 HEAD 순이다. `fetchMissingBase=false`는 fetch를 생략한다. 실제 `originalHeadCommit`을 응답에 포함하며, 재개 시 최초 기준을 유지한다. `sparsePaths`는 Git cone sparse checkout을 사용한다.

`<sessionsDirectory>/worktrees/<sessionId>.json`에 소유 상태를 원자적으로 저장하고 `QLockFile`로 같은 소유자의 전환을 배제한다. 디렉터리 0700·파일 0600을 적용하고 심볼릭 링크로 된 상태 경로를 거절한다. 생성 도중 실패·취소하면 원래 실행 경로를 유지하고 `failed` 기록과 생긴 파일·브랜치를 보존한다. 임의의 실패 잔여물을 자동 삭제하지 않으며 호스트가 기록을 보고 복구해야 한다. 작업 트리 제거 후 브랜치 삭제가 실패하면 `branch_retained`를 기록하고 성공으로 보고하지 않는다.

`WorktreeOptions::create/remove`는 호스트가 제공하는 비 Git VCS용 C++ 어댑터이다. 모델이나 API 인수로 실행 파일·콜백·저장 경로를 전달할 수 없다. 어댑터의 생성 경로 소유권과 변경사항 처리는 호스트 책임이며, 변경 상태를 확인할 수 없으므로 삭제에 명시적 discard가 필요하다. remove 콜백이 반환해도 디렉터리가 남으면 실패이다.

## 실행·권한·수명

두 도구는 직렬 실행 경계이다. 같은 모델 응답의 다음 도구부터 새 경로를 사용한다. 읽기 후 편집 기록에는 작업 공간 revision을 포함하여 전환 전 관찰을 재사용하지 않는다. 다음 모델 턴은 새 프로젝트 지침·스킬·메모리를 읽고 이전 메모리 prefetch를 폐기한다. 프로젝트·로컬 권한 설정을 새 경로에서 읽으며 명시한 CLI 추가 디렉터리는 원래 절대 경로를 유지한다. 원래 저장소를 자동으로 추가 허용 경로에 넣지 않는다.

외부 C++/MCP 도구 호출은 `Engine::bindWorkspaceContext`가 제공하는 호스트 전용 실행 범위를 사용한다. worktrees가 켜져 있으면 반환 guard가 transcript lease와 네이티브 실행 admission을 유지한다. 다른 실행 중인 대화에는 `ModelInUse`로 실패하며 wire 입력으로 실행 경로를 바꿀 수 없다. API 네이티브 작업과 모델 전환은 동일 엔진의 admission 경계를 공유한다.

세션 시작·종료 훅의 실행 경로와 모델 스냅샷도 현재 작업 트리를 사용한다. 기존 LSP 프로세스는 전환 전에 종료하고 다음 조회에서 새 루트로 시작한다. 백그라운드 셸은 시작 시 경로를 기록하므로 이후 부모의 전환에 영향을 받지 않는다. 이 세션의 네이티브 백그라운드 셸이나 서브에이전트 작업이 실행 중이면 작업 트리 삭제를 거절한다. `keep` 및 세션 종료는 작업 트리를 보존하며 재시작·재개에서 복원한다.

현재 `clear`는 활성 작업 트리가 있으면 거절한다. 먼저 `ExitWorktree keep`으로 나와야 한다. `fork`는 원래 작업 공간에서 시작하고 부모 작업 트리의 삭제 권한을 공유하지 않는다. 활성 작업 트리에서 기존 서브에이전트로 위임하는 범위, 자동 소유권 이전, 팀 격리, 작업 트리별 비동기 메모리 유지보수 전체는 아직 검증·완성되지 않았다.

## 공개 API와 CLI

임베딩 호스트는 `EngineOptions::worktrees.enabled=true`로 켠다. 도구는 기본 deferred이며 `worktrees.deferred=false`로 바로 공개할 수 있다. daemon의 에이전트 모드와 모델이 지정된 MCP CLI는 기본으로 활성화한다.

| 경계 | 진입점 |
|---|---|
| C++ | `Worktrees`, `Engine::worktreeTool`, `runWorktreeTool`, `worktreeStatus` |
| HTTP·native IPC | `agent.worktrees.enter`, `agent.worktrees.exit`, `agent.worktrees.status` |
| CLI | `iillm --auth-file FILE agent worktrees enter/exit/status SESSION [PARAMS_JSON_FILE]` |
| MCP | `EnterWorktree`, `ExitWorktree`, `iiLocalLLM.agent.worktrees.status` |
| 발견 | `agent.info.worktrees_enabled`, MCP `experimental.iisacc/worktrees` |

API 인수는 `session_id`와 해당 도구 인수이다. MCP는 연결 소유 대화에만 적용한다. 호스트 직접 호출은 transcript에 메시지를 추가하지 않는다. `agent.sessions.get.working_directory`는 실제 실행 경로이며 인증은 원래 헤더로 검사한다.

daemon은 `--agent-no-worktrees`, `--agent-worktree-config FILE`, MCP는 `--no-worktrees`, `--worktree-config FILE`을 제공한다. 설정 파일은 기존 private-file 읽기 계약을 따른다. 허용 필드는 아래와 같으며 모두 선택적이다.

```json
{"directory":"/absolute/worktrees","base_ref":"HEAD","fetch_missing_base":false,"sparse_paths":["src","tests"],"command_timeout_ms":30000}
```

## 참조와 의존성

고정 참조 `c8cd253554319f32ff64ff7000636199f720c9bc`의 두 도구와 `utils/worktree.ts`를 대조했다. 참조 설명의 HEAD 기반 문구와 실제 캐시된 원격 기본 브랜치 우선 동작은 다르다. 실제 기준 커밋을 증거로 사용한다. 기존 브랜치 초기화, 실패를 숨기는 cleanup, 승인 뒤 재검사 부족을 그대로 옮기지 않았다.

기존 Qt Core와 설치된 Git 명령을 사용한다. 새 라이브러리나 Python/TypeScript 런타임을 추가하지 않는다. Git은 유지보수되는 [GPLv2 외부 실행 도구](https://github.com/git/git/blob/master/COPYING)이며 SDK에 소스를 포함하거나 링크하지 않는다. 명령은 C++ 실행기의 program/argv 경로로 실행하고 취소·기한·출력 한도와 자식 프로세스 정리를 적용한다. 동작 근거는 [git-worktree](https://git-scm.com/docs/git-worktree), [NUL 구분 git-status](https://git-scm.com/docs/git-status), [git-update-ref](https://git-scm.com/docs/git-update-ref)이다.

검증 범위는 [Verification.md](Verification.md)에 기록한다. startup/tmux/PR 생성, ignored 파일 복사와 symlink 설정, 전체 WorktreeCreate/Remove 설정 훅, stale worktree 자동 정리, 원격·Windows·모바일 및 실제 앱 UI는 남아 있다. 이 구현으로 전체 Git 요구나 전체 하네스가 완료된 것은 아니다.
