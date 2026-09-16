# Jupyter 노트북 편집

0.45의 C++ `NotebookEdit`는 기존 작업 공간의 `Read` 관찰·권한·내용 해시·원자적 저장·백업을 사용하는 셀 편집 도구이다. 코드를 실행하거나 kernel을 시작하지 않는다.

## 입력과 편집

`notebook_path`와 `new_source`가 필수이다. `new_source`는 삭제에서도 문자열로 전달한다. `edit_mode`는 `replace`(기본), `insert`, `delete`이며 `cell_type`은 `code` 또는 `markdown`이다. 삽입에는 `cell_type`이 필요하다.

`new_source`는 노트북 JSON의 문자열 리터럴이 아니라 JSON을 해석한 실제 셀 소스이다. 노트북에 저장된 JSON 이스케이프를 코드 문자로 복사하지 않도록 도구의 인자 설명에도 이 계약을 제공한다. 도구는 전달받은 소스를 그대로 저장하며 코드 구문을 추측해서 보정하지 않는다.

- `replace`·`delete`는 `cell_id`가 필요하다. 실제 ID를 먼저 찾고, 없을 때만 `cell-N`을 0부터 시작하는 인덱스로 해석한다. 범위 밖 인덱스는 거절한다.
- `insert`는 지정 셀 뒤에 삽입한다. ID를 생략하면 맨 앞이다. 4.5 이상에는 충돌하지 않는 UUID 형식 ID를 부여한다. 이전 minor의 파일에 ID를 일괄 추가하지 않는다.
- 코드 셀 변경은 실행 횟수와 결과를 초기화한다. Markdown/raw로 바뀌면 코드 전용 필드를 제거하고, 코드로 바뀌면 attachments를 제거한다. raw 셀은 `cell_type`을 생략한 소스 수정과 삭제가 가능하다.
- 다른 셀과 notebook/cell metadata, 첨부·출력·알 수 없는 확장 필드는 보존한다. 알 수 없는 미래 셀 형식은 그대로 보존하지만 해당 셀의 직접 편집은 거절한다.

경로는 현재 작업 공간 기준으로 해석한다. 기존 파일을 완전히 읽어야 하며, 다른 소유자·압축 revision·작업 트리 revision의 관찰을 재사용하지 않는다. 읽기 후 파일 내용 변경, 권한 미리보기 후 경로 변경, private 경로와 허용 경로 밖의 파일을 거절한다. 파일 기반 설정의 `Edit(...)` 규칙은 `Write`와 `NotebookEdit`에도 적용된다. 명시적인 `NotebookEdit(...)` 경로 규칙도 지원한다.

## 저장·결과·한도

nbformat major 4, UTF-8, 1 MiB 이하를 처리한다. 입력과 출력 파일 모두 이 한도를 적용하고 셀은 최대 10,000개이다. 1 MiB는 현재 일반 텍스트 `Read`의 한도와 같다. 알려진 셀의 기본 구조·중복 ID·4.5 ID 규칙을 검사하지만 완전한 Jupyter JSON Schema 검증기를 생산 런타임에 포함하는 것은 아니다.

하위 `Read`의 전체 읽기 한도인 20,000줄도 적용된다. 이보다 긴 JSON 파일은 1 MiB 이하여도 완전한 읽기 관찰을 만들 수 없어 파일 도구를 통한 편집이 거절된다.

JSON을 Qt의 들여쓰기·LF·UTF-8 형식으로 다시 직렬화하므로 원래 공백·키 순서·BOM/EOL 형식은 보존하지 않는다. 원문은 호스트가 artifact 디렉터리를 제공했을 때 기존 파일 편집과 같은 방식으로 백업한다.

결과는 실제 셀 ID/인덱스/형식, 언어, 작업 종류, 새 소스, 셀 수, notebook 경로와 전후 SHA-256·백업 경로를 포함한다. 두 파일의 합계가 32 KiB 이하면 `original_file`·`updated_file`을 인라인으로 제공한다. 더 크면 중복 본문을 넣지 않고, artifact 디렉터리가 있을 때 별도의 불변 `updated_file_path`와 `backup_path`로 전후 내용을 보존한다. `files_inlined`가 이 구분을 표시한다.

## 참조와 의존성

고정 참조 `c8cd253554319f32ff64ff7000636199f720c9bc`의 NotebookEditTool, notebook 유틸리티와 [공식 nbformat 형식](https://nbformat.readthedocs.io/en/latest/format_description.html)을 대조했다. 참조 프롬프트의 `cell_number`는 실제 입력 스키마에 없으므로 구현된 `cell_id` 계약을 따른다. 참조의 validation은 끝 다음 셀을 거절하므로 내부의 replace→insert 분기를 공개 동작으로 취급하지 않는다. 셀 형식 전환에서 유효하지 않은 필드를 남기거나 Markdown 결과를 code로 보고하는 문제도 옮기지 않는다.

생산 코드는 기존 Qt Core JSON·파일 처리만 사용한다. 새 Python/TypeScript 런타임을 추가하지 않는다. 독립된 결과 검증에는 [Jupyter nbformat 5.11.1](https://pypi.org/project/nbformat/5.11.1/)을 사용한다. 이 라이브러리는 BSD-3-Clause이며 Python 3.10 이상과 fastjsonschema/jsonschema/jupyter-core/traitlets에 의존하므로 검증용 가상 환경에만 설치한다.

셀 단위 노트북 읽기·이미지 출력 렌더링, 큰 노트북의 범위 조회, kernel 실행, PDF·이미지·음성 모델 입력과 모든 앱 UI는 별도 구현·검증 대상이다. 검증 결과는 [Verification.md](Verification.md)에 기록한다.

## C++·앱 API·MCP

`Engine::notebookToolsEnabled()`는 native Read와 NotebookEdit의 등록 여부를 반환한다. `runNotebookTool(sessionId, "Read", arguments)`와 `runNotebookTool(sessionId, "NotebookEdit", arguments)`는 같은 저장소·정책·훅을 사용한다. 직접 호출은 transcript에 메시지를 추가하지 않으며 세션 lease를 유지한다. 활성 모델 실행과 파일 수정이 경합하면 busy 오류를 반환한다. Worktree가 비활성이어도 압축 revision을 읽고 기존 관찰의 재사용을 제한한다.

인증 HTTP/IPC에는 `agent.notebooks.read`·`agent.notebooks.edit`와 `agent.info.notebooks_enabled`가 있다. 두 호출은 `session_id`를 받고 앱 토큰의 세션 소유권을 확인한다. read는 `notebook_path`와 선택적인 `offset`·`limit`를 받으며 native Read로 전달한다. 기본 limit는 20,000줄이다. 결과의 `complete`가 false이면 편집할 수 없다. edit는 위 NotebookEdit 인자를 그대로 사용한다. 결과는 `{text,result,is_error}`이다.

CLI는 `iillm --auth-file FILE agent notebooks read|edit SESSION PARAMS_JSON_FILE`이다. MCP는 `Read(path)`·`NotebookEdit(notebook_path,...)`를 native 도구로 공개하고 `experimental.iisacc/notebooks`에서 major 4·1 MiB·UTF-8 JSON 읽기 계약을 알린다. 엔진이 없는 standalone MCP도 native workspace 도구가 있으면 해당 capability를 제공한다. 엔진에 연결된 MCP의 workspace 도구는 세션 lease와 압축 revision을 사용한다. `iisacc/projectMemory`의 Markdown fileTools 목록은 바꾸지 않는다.

대규모 결과의 불변 파일 경로는 로컬 호스트가 읽는 artifact이다. 모든 MCP 연결에서 이 경로를 원격 resource로 읽을 수 있다는 보장은 포함하지 않는다. NotebookEdit의 셀 편집 결과와 MCP 구조화 응답 자체는 별도로 검증한다.

실제 모델 fixture는 4,096-token 컨텍스트 한 개를 사용하는 단일 대화이다. 이에 맞춰 캐시도 한 개로 제한하며 서비스의 RAM 보호 조건은 유지한다. 모델 질문에 없는 임의 문자열을 Read에서 관찰하고 NotebookEdit에 사용해야 성공한다.

실제 모델 결과는 정확한 셀 소스·최종 답변, 도구 순서, 다른 셀·metadata·백업 보존을 각각 검사한다. 모델이 유효한 도구 인수 안에 잘못된 코드를 넣더라도 편집 성공으로 집계하지 않는다. JSON 보고서의 `tools`는 모델 호출만 담으며, 뒤따르는 호스트 직접 읽기와 transcript 검사는 별도로 기록한다.

`iiLocalLLMNotebookRuntimeSmoke CATALOG MODEL_URI`는 기존 native tool grammar를 사용한다. 선택 인자 `--no-tool-grammar`로 서비스의 기존 자연 출력 설정도 비교할 수 있다. 두 설정 모두 같은 도구 파서·인수 검증·성공 조건을 사용하며 모델 출력을 고치거나 예상 코드로 대체하지 않는다.

기본 fixture는 greedy이며, `--qwen-sampling`은 [Qwen3 공식 비추론 모드 권장값](https://huggingface.co/Qwen/Qwen3-8B#best-practices)인 temperature 0.7, top-p 0.8, top-k 20, min-p 0을 사용한다. seed는 0으로 유지하고 실제 선택값을 결과에 기록한다. 샘플링 설정에 따른 관측값을 모든 모델의 신뢰성으로 일반화하지 않는다.

`--thinking`은 추론 모드와 그 모드의 공식 샘플링 값인 temperature 0.6, top-p 0.95, top-k 20을 함께 사용한다. 모델 턴당 출력 한도는 다른 조건과 동일한 1,024 토큰이며, 잘린 추론이나 미완성 도구 호출을 성공으로 집계하지 않는다.
