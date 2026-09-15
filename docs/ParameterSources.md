# 제어 파라미터 출처

확인일: 2026-09-13. 설정 이름·주석·기본값을 공식 소스에서 추출하고 iiLocalLLM 스키마로 변환했다. 원본 코드를 실행하지 않는다. 카탈로그에 표현된 주석·선언의 원본 저작권 및 라이선스는 유지되며 아래 고지 파일도 설치한다.

| 제공자 | 고정 revision | 필드 수 (상속 포함) | 라이선스 |
| --- | --- | --- | --- |
| accelerate | [f13f7c13b64c](https://github.com/huggingface/accelerate/tree/f13f7c13b64c10b6eb7e2d73171ea1b94c748701) | 177 | [Apache-2.0](parameter-licenses/accelerate-LICENSE) |
| deepspeed | [b5e000c4cd79](https://github.com/deepspeedai/DeepSpeed/tree/b5e000c4cd7952b351026c73a9e7bfd8e52ff0ba) | 198 | [Apache-2.0](parameter-licenses/deepspeed-LICENSE) |
| llama | [5202104b59ad](https://github.com/ggml-org/llama.cpp/tree/5202104b59ada9005db079eea43882a2b7bf5802) | 395 | [MIT](parameter-licenses/llama-LICENSE) |
| lmstudio | [c47dce0d37a3](https://github.com/lmstudio-ai/lmstudio-js/tree/c47dce0d37a3008d3e4c393e40452825a2a9790b) | 228 | [MIT](parameter-licenses/lmstudio-LICENSE) |
| mlx | [229f5b430df7](https://github.com/ml-explore/mlx/tree/229f5b430df7926743c5b6ac62068cae2ebc8978) | 57 | [MIT](parameter-licenses/mlx-LICENSE) |
| mlx-lm | [dcbcf786c0cf](https://github.com/ml-explore/mlx-lm/tree/dcbcf786c0cf56f9a12fabe9468c887781431ae2) | 164 | [MIT](parameter-licenses/mlx-lm-LICENSE) |
| ollama | [53fed2611281](https://github.com/ollama/ollama/tree/53fed26112817f7c55f664efb9e3f65f06cab7db) | 69 | [MIT](parameter-licenses/ollama-LICENSE) |
| peft | [0e8d0ae8ab94](https://github.com/huggingface/peft/tree/0e8d0ae8ab94f189f28b845e293d7452d7892d91) | 815 | [Apache-2.0](parameter-licenses/peft-LICENSE) |
| sglang | [14b647cf27d7](https://github.com/sgl-project/sglang/tree/14b647cf27d7f2c1a3764841f7d3770ff9f9e7d6) | 1033 | [Apache-2.0](parameter-licenses/sglang-LICENSE) |
| torch | [4d2bd99cd03c](https://github.com/pytorch/pytorch/tree/4d2bd99cd03cda454d98ff8c060c1353743c266f) | 234 | [BSD-style and included notices](parameter-licenses/torch-LICENSE) |
| transformers | [415e6d2f596e](https://github.com/huggingface/transformers/tree/415e6d2f596ef2bd44fdee4261799200a7fc02bf) | 609 | [Apache-2.0](parameter-licenses/transformers-LICENSE) |
| trl | [cd2c52876d99](https://github.com/huggingface/trl/tree/cd2c52876d99b00baa5660328310e940b8360c6c) | 4081 | [Apache-2.0](parameter-licenses/trl-LICENSE) |
| vllm | [fa1b3b1922cc](https://github.com/vllm-project/vllm/tree/fa1b3b1922ccb2d7a597925765c4bec7f358d748) | 1107 | [Apache-2.0](parameter-licenses/vllm-LICENSE) |

공식 파일 214개를 잠갔다. iiLocalLLM 공통 생성 필드 16개는 별도 포함한다. 설정 그룹 수 391, 상속 포함 필드 수 9183, 고유 선언 수 4610이다.

이 목록은 유지보수 중인 대표 런타임·학습 도구의 고정 스냅샷이다. 각 파일의 추출 그룹과 opaque 필드는 `catalog/parameter-coverage.json`에서 확인한다. 특정 하드웨어에서 실행 가능한 설정의 목록은 필드별 nativeBindings 및 실제 런타임 검증으로 구분한다.

새 런타임 의존성을 추가하지 않았다. Python 표준 AST와 Qt JSON을 재사용하고, C++/Go/TypeScript 선언의 제한된 정적 추출기를 테스트한다. 대규모 학습 프레임워크 전체를 설치·링크하는 비용을 피하면서도 원본 제공자의 객체 이름과 타입을 유지한다. upstream 기본값 표현식·post-init·동적 플러그인 검증은 평가하지 않는다.

MLX 샘플링 파일은 위 mlx-lm 커밋의 `mlx_lm/sample_utils.py`를 수정 없이 `runtimes/mlx_sample_utils.py`로 포함했다. SHA-256은 `c93c1eef794725f9f7ce77b6212f61eb6d0fe17b9cd87c06cec6470ee12b07f2`이며 Apple의 MIT 고지를 보존한다. 이 파일은 min-p에서 bool 대신 MLX array를 전달하는 공식 수정을 포함한다.

0.30.0에서 `Types.h`에 구조화 요청 필드가 추가되어 로컬 `iiLocalLLM.GenerationOptions` 16개 항목의 선언 행과 전체 헤더 SHA-256을 생성기로 다시 산출했다. 생성 제어 타입·기본값·검증 제약·백엔드 바인딩은 변경하지 않았다. 외부 공급자 고정 소스는 유지한다.
