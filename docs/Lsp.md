# C++ LSP 코드 탐색 (0.43.0)

`LSP`는 호스트가 지정한 언어 서버를 통해 `goToDefinition`, `findReferences`, `hover`, `documentSymbol`, `workspaceSymbol`, `goToImplementation`, `prepareCallHierarchy`, `incomingCalls`, `outgoingCalls`를 실행한다. Python이나 TypeScript 생산 런타임은 추가하지 않는다. C++ SDK, Engine 도구 루프, 인증 API·MCP와 thin CLI에서 같은 구현을 사용한다.

## 호스트 설정

`EngineOptions::lsp.servers`에 `LspServerOptions`를 지정한다. 빈 목록이면 기능을 등록하지 않는다. 각 설정은 고유 name, command, arguments, 확장자→languageId 매핑, 환경, initializationOptions와 settings를 갖는다. C++ `Lsp`를 독립적으로 만들고 `tool()`을 registry에 등록할 수도 있다. 모델 입력과 프로젝트 파일은 서버 실행 명령·환경을 추가하거나 수정하지 못한다.

daemon은 `--agent-lsp-config FILE`, 에이전트 MCP는 `--lsp-config FILE`을 받는다. 기존 private-file 검증으로 읽은 호스트 JSON만 사용한다. MCP CLI의 이 옵션은 현재 `--model`을 요구한다. LSP 자체는 모델 추론을 사용하지 않는다.

```json
{
  "servers": {
    "clangd": {
      "command": "/absolute/path/to/clangd",
      "args": ["--background-index=false", "--pch-storage=memory"],
      "extensionToLanguage": {".c": "c", ".h": "cpp", ".cpp": "cpp", ".hpp": "cpp"},
      "initializationOptions": {},
      "settings": {}
    }
  },
  "request_timeout_ms": 30000,
  "startup_timeout_ms": 15000
}
```

JSON의 선택적 `env`는 기존 호스트 환경의 지정 항목만 덮어쓴다. 환경 변수·셸 구문을 확장하지 않는다. 알 수 없는 필드, 중복 서버 이름·확장자, 잘못된 자료형과 한도를 거부한다. 긴 확장자를 우선하므로 `.d.ts` 같은 매핑이 가능하다. 명령 실행은 셸을 거치지 않는다. 서버 설치와 업데이트는 호스트의 책임이다.

## 도구·API·MCP

```json
{"operation":"goToDefinition","filePath":"src/main.cpp","line":10,"character":8}
```

네 필드는 모든 연산에 필수이다. line과 character는 1부터 시작하는 UTF-16 위치이며 surrogate pair 내부와 파일 밖 위치를 거부한다. `workspaceSymbol`에서만 선택적 `query`를 사용할 수 있다. 생략하면 참조 도구와 같이 빈 query를 보낸다.

결과의 `result`는 표시용 텍스트이고 `data`는 서버의 구조화 결과이다. `data` 내부 LSP 범위는 표준의 0 기반 UTF-16을 유지하며 표시용 위치만 1 기반으로 바꾼다. operation, filePath, resultCount, fileCount, server, truncated와 filtered_results도 반환한다. 호출 계층은 prepare 요청 결과의 첫 item과 opaque data를 보존해 후속 incoming/outgoing 요청에 사용한다.

`agent.info.lsp_enabled`로 가용성을 확인한다. 인증 API `agent.lsp.query`는 session_id와 도구 입력을 받는다. `agent.lsp.status`는 session_id만 받으며 해당 소유자의 서버 상태·기능과 문서 진단을 돌려준다. 주 transcript에 별도 호스트 조회를 추가하지 않는다. C++는 `Engine::runLsp`, `lspStatus`를 제공한다.

상태의 capabilities는 지원하는 9개 읽기 연산과 위치 인코딩을 요약한다. 진단 응답은 문서 단위로 최대 `maxResultCharacters * 2` bytes까지 담고 초과하면 `diagnostics_truncated`를 표시한다. 서버 명령·인수·환경·초기화 설정을 상태 응답에 포함하지 않는다.

```sh
iillm --auth-file private/token agent lsp query SESSION request.json
iillm --auth-file private/token agent lsp status SESSION
```

MCP는 `LSP`, `iiLocalLLM.agent.lsp.status`와 `iisacc/lsp` capability (`iisacc.lsp/1`)를 제공한다. 연결에 바인딩한 세션을 사용하며 호출자가 다른 소유자를 지정하지 못한다. 네이티브 LSP는 지연 공개·읽기 전용이고 일반 앱 실행 잠금 밖에서 자체 큐로 실행한다.

## 수명·동기화·권한

프로세스는 세션·canonical workspace·서버별로 분리하고 첫 호출에서 시작한다. 동일 소유자의 다음 호출은 초기화된 서버와 열린 문서를 재사용한다. SHA-256으로 변경을 확인해 didOpen 또는 버전이 증가한 didChange를 보낸다. incremental 서버에는 이전 문서 전체 범위를 교체하는 표준 change를 전달한다. 닫힌 세션과 Engine 종료에서 shutdown/exit를 시도하고 남은 프로세스를 종료한다. Unix에서는 자식 프로세스 그룹까지 정리한다.

Content-Length 프레이밍, 초기화 기능 협상, UTF-16 위치, workspace configuration/folders 응답, 설정 변경, 진단 알림, 요청 취소를 처리한다. ContentModified 응답은 같은 요청 기한 내에서 500/1000/2000ms 간격으로 최대 세 번 재시도한다. 실패한 연결은 폐기하고 후속 호출에서 제한된 재시작을 수행한다. 서버가 지원한다고 선언하지 않은 연산은 명시적으로 실패한다. 서버의 workspace/applyEdit는 거절하며 executeCommand를 노출하지 않는다.

파일은 실제 작업 루트와 호스트가 허용한 추가 디렉터리 안의 일반 UTF-8 파일이어야 한다. UNC 경로, 범위 밖 링크와 호스트 private paths를 거부한다. 준비 단계의 canonical 경로를 실행에 바인딩하며 링크 대상이 바뀌면 파일 본문 읽기와 서버 시작 전에 거부한다. 사용자 정의 권한 정책에는 재검사에서도 실제 등록된 네이티브 도구 정의·지연 공개 설정·스키마·경로 메타데이터를 전달한다. 기존 `LSP(path)` 규칙에 더해 `Read(path)` deny/ask도 적용하며 훅에서 바뀐 입력을 다시 검사한다. 결과 URI도 접근 가능한 로컬 경로만 남기고 git check-ignore로 제외된 파일을 걸러낸다. 별도 문서의 결과는 현재 읽기 정책의 Allow를 요구한다. 진단은 열린 문서와 현재 버전에 해당하는 알림만 보관하며 다른 세션에 노출하지 않는다.

언어 서버는 호스트가 신뢰해 실행하는 외부 프로세스이다. 파일 요청·결과 경계는 OS 샌드박스가 아니며 서버 자체의 인덱싱과 플러그인 실행을 제한한다고 주장하지 않는다. 서버 문서·진단은 실행 지시가 아닌 비신뢰 데이터로 취급해야 한다.

| 기본 한도 | 값 |
|---|---|
| 요청 / 초기화 기한 | 30초 / 15초 |
| 프레임 / 문서 | 16 MiB / 10,000,000 bytes |
| 표시 결과 | 100,000 UTF-16 코드 단위 |
| 서버 인스턴스 / 열린 문서 / 대기 요청 | 16 / 인스턴스당 128 / 16 |
| 진단 | 열린 문서당 512항목·64 KiB |

## 참조·의존성·남은 범위

고정 참조 `c8cd253554319f32ff64ff7000636199f720c9bc`의 `source/src/tools/LSPTool/`와 `source/src/services/lsp/`를 대조한다. 9개 연산, lazy 문서 열기, 호출 계층의 두 단계 요청, 읽기 권한, ignored 결과 제외와 표시용 위치에 대응한다. 실제 디스크 변경을 버전 갱신해 보내는 경로와 소유 세션별 프로세스 격리를 추가한다.

외부 라이브러리를 검토한 결과, 참조의 vscode-jsonrpc/TypeScript 런타임 대신 기존 Qt Core의 QProcess와 JSON을 사용한다. 표준 전송·수명 구현은 `LspProtocol.cpp`, 호스트 설정·권한·도구 결과는 `Lsp.cpp`로 분리한다. 분석 엔진은 직접 구현하지 않고 [clangd](https://clangd.llvm.org/) 등 호스트가 설치한 언어 서버를 사용한다. 표준 계약은 [LSP 3.17](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/)을 기준으로 한다.

IDE 연결·편집기 미저장 버퍼, 파일 시스템 watcher의 전체 프로젝트 알림, 동적 capability 등록, TCP 서버, 플러그인 자동 설치/추천, 모든 언어 서버와 플랫폼의 검증은 남아 있다. 문서 동기화는 호출 시 디스크 내용을 기준으로 한다. 실제 clangd·모델·전송·설치 소비자 검증 결과는 Verification.md에 별도로 기록한다.
