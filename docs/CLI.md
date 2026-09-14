# iillm과 iiLocalLLMD

`iillm → Native IPC → iiLocalLLMD → Runtime` 관계이다. iillm은 Qt Core/Network만 링크하며 SDK의 Service나 추론 런타임을 생성하지 않는다. daemon 연결이 없으면 명시적인 연결 오류로 종료한다. `iiLocalLLMD`와 기존 `iilocal-llm-service`는 동일 서비스 진입점이다.

```sh
export IILLM_SOCKET="$PWD/build/llm.sock"
./build/iiLocalLLMD --socket "$IILLM_SOCKET" --models-root "$PWD/build/chat/Models" --http-port 8080
# 별도 터미널에서도 저장소 디렉터리로 이동하고 설정한다.
export IILLM_SOCKET="$PWD/build/llm.sock"
./build/iillm pull qwen2.5:0.5b
./build/iillm models
./build/iillm run qwen2.5:0.5b
./build/iillm ps
```

| 명령 | 의미 |
| --- | --- |
| pull MODEL | daemon이 registry 원본을 다운로드·검증·설치한다. 정상 설치가 있으면 검증 후 재사용한다 |
| models | 설치 모델 목록과 loaded/unloaded를 표시한다 |
| run MODEL [PROMPT] | daemon 세션에서 생성하며 delta를 stdout으로 출력한다. PROMPT가 없고 터미널이면 대화, 파이프이면 stdin 전체를 한 prompt로 처리한다 |
| ps | 현재 상주한 모델, 예약 메모리 추정 GiB, 선택한 backend, 활성 여부 또는 만료까지 시간을 표시한다. 추론 중에도 응답한다 |

`--json`은 models/pull/ps 결과와 run의 최종 GenerationResult를 JSON으로 출력한다. 대화 모드에서도 매 턴 JSON 한 줄을 즉시 flush하며 안내와 프롬프트는 stderr로 출력한다. `run --max-tokens 128 --temperature 0 --system "..." --keep-alive 5m`을 지원한다. temperature는 유한한 0~10 값이며 기본 0.7, 0은 greedy 생성이다. 잘못된 값은 세션 생성 전에 거부한다.

대화는 같은 세션에서 이력과 KV를 재사용한다. `/clear`는 이력과 KV를 비우고 최초 system prompt를 유지하며 모델은 기존 상주 정책을 따른다. `/bye`와 `/exit` 또는 EOF는 대화 종료이다. 이 명령들은 터미널 대화에서만 해석하며 단발 prompt/파이프 입력은 그대로 모델에 전달한다. 정상 종료와 처리 가능한 오류/신호에서는 세션을 닫는다. Ctrl+C는 생성/입력 대기를 취소하고 130으로 종료한다. daemon은 연결이 끊긴 CLI의 생성과 pull을 취소한다. OS의 강제 종료처럼 CLI가 정리할 수 없는 경우 세션 이력은 IPC의 기존 재접속 계약에 따라 남으며, 모델의 유휴 만료를 막지는 않는다.

```sh
./build/iillm run qwen2.5:0.5b "Explain KV caching." --max-tokens 128 --temperature 0 --keep-alive 5m
printf 'Summarize this text.\n' | ./build/iillm run qwen2.5:0.5b --keep-alive 0
./build/iillm ps --json
```

`--socket`이 우선이고 다음은 `IILLM_SOCKET`, 마지막은 사용자별 기본 endpoint이다. 기본 Unix endpoint는 Qt 임시 디렉터리 아래 `iiLocalLLMD-<home 경로 SHA-256 앞 12자리>.sock`이며 Windows는 같은 식별자의 Named Pipe이다. 자동 기본값은 daemon과 CLI가 같은 코드로 계산한다. 실제 배포에서는 짧은 명시적 endpoint와 절대 models-root를 사용하는 편이 경로 확인에 유리하다. HTTP만 필요하면 daemon에 `--http-port`만 지정한다. 아무 전송 옵션도 없으면 기본 IPC로 시작한다.

## 서비스 registry

최소 시작 모델은 `qwen2.5:0.5b → model://qwen2.5-0.5b-instruct-q4`이다. 공식 `Qwen/Qwen2.5-0.5B-Instruct-GGUF`, revision `9217f5db79a29953eb74d5343926648285ec7e67`의 `qwen2.5-0.5b-instruct-q4_k_m.gguf`를 사용한다. 크기는 491,400,032 bytes, SHA-256은 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`이다. 2026-09-13 [고정 revision의 공식 API](https://huggingface.co/api/models/Qwen/Qwen2.5-0.5B-Instruct-GGUF/revision/9217f5db79a29953eb74d5343926648285ec7e67?blobs=true)로 확인했다. 모델의 내장 chat template을 사용하며 별도 override가 필요 없다. 다운로드 후 대화에는 외부 서비스나 네트워크가 필요하지 않다.

기본 `catalog/registry.json`은 `qwen3:8b → model://qwen3-8b-q4`를 정의한다. 원본은 Qwen의 공식 `Qwen/Qwen3-8B-GGUF`, revision `7c41481f57cb95916b40956ab2f0b139b296d974`, 파일 `Qwen3-8B-Q4_K_M.gguf`이다. 다운로드 크기는 5,027,783,488 bytes, SHA-256은 `d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785`이다. 이 가중치는 SDK에 포함하지 않으며 사용자의 pull 명령으로만 내려받는다. 원본 메타데이터는 2026-09-07 [공식 Hub API](https://huggingface.co/api/models/Qwen/Qwen3-8B-GGUF?blobs=true)에서 확인했다.

`iiLocalLLMD --registry /absolute/path/registry.json`으로 호스트가 다른 원본을 구성할 수 있다. 파일을 지정하면 기본 registry를 대체한다. 별칭은 registry에만 있고 ModelCatalog/런타임은 정규 id를 사용한다. CLI는 URL이나 실제 설치 경로를 결정하지 않는다.

```json
{
  "models": [{
    "aliases": ["example:small"],
    "manifest": {
      "id": "example-small", "architecture": "llama", "format": "gguf", "quantization": "Q4_K_M",
      "context_length": 32768, "capabilities": ["text-generation", "chat"], "entry_point": "model.gguf",
      "files": [{"path": "model.gguf", "size": 12345, "sha256": "REPLACE_WITH_64_HEX_DIGITS"}]
    },
    "sources": {"model.gguf": "https://example.com/PINNED_REVISION/model.gguf"}
  }]
}
```

예시의 size/sha256/URL은 실제 원본 값으로 교체해야 한다. sources는 manifest의 모든 파일에 일대일로 대응해야 한다. 별칭 중복·잘못된 manifest·파일 경로 이탈·해시 누락은 부팅 때 거부한다. HTTPS만 허용하며 로컬 미러/테스트용으로 숫자 loopback 주소의 HTTP를 허용한다. 외부 평문 HTTP, URL 사용자 정보, fragment, HTTPS→HTTP 리다이렉트는 거부한다. 리다이렉트 상한은 8회, 전송 무응답 timeout은 30초이다.

다운로드는 `Models/.pull-*`의 임시 파일로 스트리밍한다. 수신 크기가 고정 크기를 넘으면 중단하고, 완료 후 크기와 SHA-256을 확인한다. 모든 파일이 통과한 뒤 기존 ModelCatalog의 원자적 설치 경로를 호출한다. 취소·실패 시 staging을 제거하며 완료하지 않은 모델은 목록에 게시하지 않는다. 다운로드와 설치 복사본이 잠시 공존하므로 파일 크기 합계의 2배 가용 디스크를 사전 검사한다. 중단한 다운로드의 resume과 임의 Hub 저장소 검색은 제공하지 않는다.

pull은 현재 모델 설치/생성과 같은 FIFO에서 실행되므로 긴 다운로드 중 새 생성·상태 변경은 대기한다. ps와 hardware 조회, 해당 연결의 취소는 계속 응답한다. CLI의 pull 대기 상한은 24시간, 생성은 5분이다. 종료 코드 0은 성공, 1은 오류, 130은 처리된 SIGINT/SIGTERM이다.

## 상세 설정

`iillm parameters`는 그룹을, `iillm parameters GROUP`은 상세 타입·기본값·제약·소스를 출력한다. `iillm parameters GROUP FILE`은 JSON 파일을 검증하여 원본 형식으로 출력한다. `--defaults`와 `--redact`를 지원한다. 데몬에 연결하되 모델은 필요 없다.

`run --options FILE`은 공통 생성 옵션 JSON을 읽는다. 명시한 `--temperature`와 `--max-tokens`만 파일 값을 덮어쓰며, 파일 크기는 최대 1 MiB이다. [Parameters.md](Parameters.md)에 추론·학습·파인튜닝 객체 및 실행 가능한 필드가 정리되어 있다.

## 0.9.0 MCP 연결 관리와 도구 검색

호스트가 지정한 MCP 설정 파일의 연결·복구, 대화별 `ToolSearch`, 인증된 `agent.mcp.status` 및 CLI 옵션을 추가했다. 설정과 실행 권한, 수명 및 미지원 범위는 [ToolDiscovery.md](ToolDiscovery.md)를 참조한다.

## 작업·Todo 명령 (0.11.0)

인증된 iillm에서 `agent tasks ACTION SESSION [PARAMS_JSON_FILE]` 및 `agent todos ACTION SESSION [PARAMS_JSON_FILE]`을 제공한다. tasks는 create/get/list/update/claim, todos는 get/write를 지원한다. JSON 응답의 is_error가 true이면 종료 코드 1이다. daemon의 기본 활성화·옵트아웃과 예시는 [Tasks.md](Tasks.md)를 참조한다. CLI는 여전히 Core/Network만 링크한다.

## 백그라운드 셸 명령 (0.12.0)

`iillm --auth-file TOKEN agent shell ACTION SESSION [PARAMS_JSON_FILE]`에서 ACTION은 start/output/stop/list이다. start JSON에는 `command`, output/stop JSON에는 `task_id`를 지정한다. 목록은 JSON 파일 없이 조회할 수 있다. CLI는 native IPC를 사용하고 도구의 `is_error=true`에 종료 코드 1을 반환한다. 실제 실행 수명과 대기 취소의 차이, 출력 페이지와 daemon 비활성화 옵션은 [BackgroundTasks.md](BackgroundTasks.md)에 기록한다.

## 입력 큐 명령 (0.13.0)

`iillm --auth-file TOKEN agent inputs ACTION SESSION [PARAMS_JSON_FILE]`은 enqueue/list/remove/run을 지원한다. enqueue에는 text와 선택 kind·priority·context_paths, remove에는 input_id를 넣는다. run은 prompt 없이 남은 큐에서 실행하고 생성 옵션·max_turns·context_paths를 받는다. 최종 JSON은 기존 native IPC 경로를 사용하며 완료하지 못한 run은 종료 코드 1이다. 진행 이벤트가 필요하면 `--json rpc agent.inputs.run PARAMS_JSON_FILE`을 사용하고 파일에 session_id도 지정한다. [InputQueue.md](InputQueue.md)에 우선순위와 복구 계약을 기록한다.

`iillm --auth-file FILE agent skills list SESSION`으로 스킬 목록을 조회한다. `agent skills run SESSION PARAMS_JSON_FILE`은 `skill`과 선택적 `skill_arguments`를 인증 API로 전달하며 실패 시 종료 코드 1을 반환한다. 스킬을 로딩하는 데몬은 `--agent-skills-dir`/`--agent-no-skills`, MCP 서버는 `--skills-dir`/`--no-skills`를 지원한다. [스킬 계약](Skills.md)을 참조한다.

0.15.0은 C++ 서브에이전트와 `agent.agents.run/output/stop/list`, 대응 MCP·CLI 경로를 추가한다. 소유권·취소·분기·재개와 설정 계약은 [Subagents.md](Subagents.md)를 따른다.

0.16.0의 `agent.agents.profiles`(MCP: `iiLocalLLM.agent.agents.profiles`, CLI: `agent agents profiles SESSION`)는 프로파일 메타데이터·출처·가려진 정의·오류를 반환한다. C++ 호스트는 `Subagents::attach`로 현재 프로파일을 각 턴에 연결한다. 독립 데몬과 MCP 서버의 `--agent-profiles FILE`·`--no-agent-profiles`, 모델 사용 범위와 훅 계약은 [AgentProfiles.md](AgentProfiles.md)를 따른다.

0.17.0에서는 `context: fork` 스킬을 같은 API·MCP·CLI 호출로 별도 자식에서 실행한다. 직접 호출은 자식 결과를 반환하고 모델의 `Skill` 호출은 후속 부모 턴에 결과를 전달한다. 본문 분리·권한·모델·사용량·큐 입력과 참조 차이는 [Skills.md](Skills.md)의 별도 자식 실행 계약을 따른다.
