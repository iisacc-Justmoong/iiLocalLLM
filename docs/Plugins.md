# 로컬 플러그인 저장소와 실행

0.52는 C++ `PluginStore`, `PluginSnapshot`, `PluginRuntime`을 제공한다. 로컬 패키지를 설치하고 활성 버전을 선택한 뒤, 호스트가 시작할 때 스킬·명령·에이전트·훅·MCP·LSP 설정을 기존 실행기에 연결한다. 별도 프로덕션 의존성은 추가하지 않는다. 파일·JSON·잠금·원자적 저장에는 Qt를, Markdown 메타데이터에는 기존 YAML 파서를 사용한다.

참조 대상은 [Exhen 분석본의 고정 커밋](https://github.com/Exhen/claude-code-2.1.88/tree/c8cd253554319f32ff64ff7000636199f720c9bc)이다. 저장소의 진위나 실제 서비스 전체와의 동일성을 검증했다는 뜻은 아니다. 기준은 `source/src/utils/plugins/schemas.ts`, `pluginLoader.ts`, `pluginDirectories.ts`, `loadPluginAgents.ts`에서 관찰한 manifest, 표준 디렉터리의 가산 규칙, 캐시·데이터 분리, 이름 공간이다. 아래 차이와 미구현 항목을 유지한다.

## 설치와 선택

```sh
iillm-plugins --store /private/host/plugins install /sources/my-plugin
iillm-plugins --store /private/host/plugins list
iillm-plugins --store /private/host/plugins inspect
iillm-plugins --store /private/host/plugins disable my-plugin
iillm-plugins --store /private/host/plugins enable my-plugin
iillm-plugins --store /private/host/plugins uninstall my-plugin
```

`install`은 기본적으로 활성화하며 `--disabled`로 비활성 설치한다. 같은 이름의 새 소스를 설치하면 선택된 revision을 갱신한다. 버전 문자열이 같아도 내용이나 실행 비트가 다르면 다른 SHA-256 캐시를 생성한다. 이미 존재하는 revision은 덮어쓰지 않고 무결성을 확인한다.

저장소는 명시적인 절대 경로를 받는다. 패키지에는 `.claude-plugin/plugin.json`의 `name`이 필요하다. 로컬 ID는 1–64자의 ASCII 영숫자·점·밑줄·하이픈이며 첫 글자는 영숫자이다. `version`은 최대 128자의 메타데이터이며 semver 범위 해석을 의미하지 않는다.

`installed.json`의 스키마는 `iisacc.plugins/1`이다. 프로세스 간 잠금과 `QSaveFile`로 선택 상태를 저장하고, 상태 파일의 경로를 신뢰하지 않고 `cache/<name>/<sha256>`를 계산한다. 가변 데이터는 `data/<name>`에 둔다. 갱신·비활성화·제거는 이전 캐시와 데이터를 보존한다. 실행 중인 호스트는 처음 선택한 revision을 계속 사용하고 다음 시작 때 새 상태를 적용한다. 파일 시스템을 외부에서 수정한 경우 snapshot 해시 검사와 문서 로드 시 SHA 검사가 변경을 거부한다.

기본 한도는 64개 플러그인, 패키지별 8,192개 항목·128 MiB, 단일 파일 8 MiB, JSON 1 MiB이다. `.git` 항목은 복사하지 않는다. 심볼릭 링크·특수 파일·상위 경로 이동은 거부하며 실행 비트는 보존한다. 호스트 저장소와 도구 workspace는 서로 포함하지 않는 경로여야 한다.

## Manifest와 실행기 연결

| 입력 | 처리 |
|---|---|
| `skills/**/SKILL.md` | 기존 Skill 파서·권한 검사·인라인/fork 실행 사용 |
| `commands/**/*.md` | 같은 Skill 실행기로 로드하는 기존 형식의 명령 |
| `agents/**/*.md` | 기존 프로필·Subagents·Teams에 전달 |
| `hooks/hooks.json` | 기존 `CommandHooks`의 command/HTTP/prompt/agent 훅 |
| `.mcp.json` | 기존 `McpConnections`의 stdio·Streamable HTTP |
| `.lsp.json` | 기존 `Lsp`의 stdio 서버 |
| manifest의 추가 경로·배열 | 표준 디렉터리와 함께 등록, 같은 파일 중복 제외 |
| inline hooks/MCP/LSP | 파일 설정과 같은 실행기에 전달 |
| `dependencies` | 활성 로컬 이름 참조 검사, 누락·비활성·순환이면 해당 플러그인 구성 차단 |

명령·스킬·에이전트 이름은 `plugin:nested:name` 형태이다. 에이전트는 frontmatter `name`을 우선하고 없으면 파일 이름을 사용한다. 플러그인 내부의 skill `agent`와 agent `skills` 참조는 같은 플러그인의 발견된 이름에 연결한다. 호스트/프로젝트의 기존 우선순위와 하위 에이전트의 도구·모델·권한 제한은 유지한다.

서버 이름은 참조의 addPluginScopeToServers/addPluginScopeToLspServers와 같은 `plugin:<plugin-name>:<server>`이며 최종 길이는 128자 이하이다. MCP 도구는 참조의 buildMcpToolName/normalizeNameForMCP와 같이 ASCII 영숫자·밑줄·하이픈 이외의 문자를 밑줄로 바꾼다. 예를 들어 내부 ID plugin:sample:peer의 echo는 `mcp__plugin_sample_peer__echo`이다. 정규화 충돌이나 전체 128자 초과는 거부한다. 기존 SDK의 일반 MCP 서버 이름 규칙은 유지한다. LSP에서 두 서버가 같은 확장자를 등록하면 기존 충돌 검사가 거부한다. MCP 파일 설정은 inline 플러그인 정의보다 높은 우선순위를 가지며 명시적인 호스트 파일이 전체 서버 정의를 교체할 수 있다.

문서·LSP 설정의 `${CLAUDE_PLUGIN_ROOT}`와 `${CLAUDE_PLUGIN_DATA}`는 한 번만 치환한다. 훅은 두 환경 변수를 받고 MCP는 서버별 치환 환경을 사용한다. 치환 결과 안의 다른 변수처럼 보이는 문자열을 다시 평가하지 않는다. Skill 인수 치환·세션 변수는 기존 규칙을 적용한다. 명령의 모델 입력을 셸 문자열에 추가하지 않는다.

## C++ 호스트와 앱 전송

`PluginRuntime`은 실행기 위의 구성 계층이다. Engine에는 불변 snapshot 데이터와 기존 실행기 콜백만 전달하며 Engine이 PluginRuntime 구현을 참조하지 않는다. 호스트에서 `Engine`, `Subagents`, `Teams`, `McpConnections`를 만들기 전에 한 번 연결한다.

공개 옵션 구조체가 변경되므로 C++ 소비자는 0.52 헤더와 라이브러리를 함께 사용해 재빌드한다. 플러그인 설치는 workspace 파일 도구의 접근 범위를 넓히지 않는다. 패키지 리소스는 구성된 MCP/LSP/훅 실행기가 사용하며 기존 파일 도구의 경계는 유지한다.

```cpp
agent::PluginStore store({pluginStoreDirectory});
agent::CommandHookOptions hooks;
hooks.workingDirectory = workspace;
auto plugins = std::make_shared<agent::PluginRuntime>(store.snapshot(), hooks);

agent::EngineOptions engineOptions;
engineOptions.sessionsDirectory = sessionsDirectory;
agent::AgentProfileOptions profiles;
agent::McpConnectionOptions mcpOptions;
mcpOptions.workingDirectory = workspace;
agent::PluginRuntime::attach(engineOptions, profiles, mcpOptions, plugins);
// profiles -> SubagentOptions.profiles / TeamsOptions.profiles
// mcpOptions -> McpConnections; engineOptions -> Engine / ApiOptions.engine
```

하위 Engine은 구성된 옵션을 상속한다. 프로필 필터를 적용한 다음 플러그인을 다시 붙여 도구를 넓히지 않는다. `SkillOptions.sources`와 `AgentProfileOptions.pluginSources`는 호스트가 선택한 문서 경로·이름·해시를 기존 파서로 전달하는 공개 경계이다.

daemon은 `--agent-plugins <store>`, MCP 호스트는 `--plugins <store>`를 받는다. 후자는 `--model`이 필요하다. 인증된 native IPC/HTTP API의 `agent.plugins.list`, MCP의 `iiLocalLLM.agent.plugins.list`, C++ `Engine::plugins()`는 동일한 활성 snapshot 정보를 반환한다. 인증 IPC CLI의 iillm agent plugins도 같은 조회를 제공한다. 설치·선택 변경은 C++ `PluginStore`와 호스트 CLI에서 수행한다. 모델 도구나 API 호출에 설치 권한을 추가하지 않는다.

공개 상태에는 revision, 활성 선택, 의존성 차단, 구성 항목 수, 미지원 기능 이름을 담고, 패키지 경로·명령·프롬프트·환경 값은 제외한다. `configured`는 구성 성공이다. MCP 연결·LSP 프로세스·모델 추론 성공을 뜻하지 않으며 각각의 기존 상태/실행 결과로 확인한다. 호스트가 스킬·프로필 발견이나 하위 에이전트를 비활성화하면 그 실행 제한이 우선한다.

## 남은 범위

원격/Git/marketplace 획득·검색·업데이트 정책, marketplace 범위와 semver 의존성 해석, manifest 없는 패키지 추론, MCPB, 실행 중 reload/revoke, 캐시 GC, userConfig·settings·outputStyles·channels, 명령 메타데이터의 일부 표현은 아직 구현하지 않았다. 미지원 manifest 필드는 `unsupported_features`에 남긴다. 지원하지 않는 MCP/LSP 필드·전송을 요구하는 서버는 미지원으로 표시하고 등록하지 않는다. 기존 실행기에서 지원하지 않는 문서 메타데이터·훅 형식은 해당 실행기의 실패/미지원 계약을 따른다. 전체 플러그인 호환 상태는 `partial`이다.

검증은 `tests/plugin_tests.cpp`의 설치 수명주기·무결성·의존성·실제 프로세스 및 API/MCP 사례와 `tests/plugin_runtime_smoke.cpp`의 설치된 패키지 → 스킬 → MCP 도구 → 응답 경로로 구성한다. 실제 모델 실행 여부는 결과의 `native_inference`로 구분한다. 최종 실행 수치와 패키지 검증은 [Verification.md](Verification.md)에 기록한다.
