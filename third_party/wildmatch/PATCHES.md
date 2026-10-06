<a id="libgit2-wildmatch-adaptation"></a>

# libgit2 와일드매치 적응

소스는 libgit2 v1.9.7에서 왔습니다. `source.json`는 **상위 공급 측**와 수정되지 않은 파일 해시 및 URL을 기록합니다. `COPYING`는 변경되지 않았습니다: GNU GPL v2와 libgit2 연결 예외가 있습니다. 이것은 소스 MIT를 라이선스로 만들지 않습니다.

`wildmatch.c` 와 `wildmatch.h` 로의 로컬 변경은 개인 엔트리 포인트를 `iilocal_wildmatch` 로 이름 변경하고, 공유 작업 예산과 취소 콜백을 허용하며, 재귀를 128로 제한하고, `WM_ABORT_LIMIT` 로 소진을 전파합니다. 이는 선택 사항인 `**/` 분기에서 비롯된 것까지 포함됩니다. C++ 호출자는 소진에 대한 결정을 거부하며, 소진을 불일치 거부로 해석하지 않습니다. 표준 헤더 어댑터 `git2_util.h` 는 libgit2 와 연결을 피하거나 환경 리포지토리 무시 파일을 참조하지 않습니다.

C++ 어댑터는 UTF-16 입력을 소문자로 바꾸고 최대 128개의 서로 다른 비ASCII 코드 단위를 정렬된 바이트 알파벳에 매핑한다. 이는 wildmatch의 바이트 파서를 바꾸지 않고 와일드카드 문자 단위/범위를 보존한다. 더 큰 알파벳을 가진 입력은 명시적으로 실패한다. Unicode 소문자화 동작은 JavaScript RegExp의 정확한 Unicode 대소문자 접기 규칙이 아니라 Qt를 따른다. 이 차이는 설정 문서에 명시한다.

Unix에서 C11 컴파일러와 `-fPIC -fvisibility=hidden`를 사용하여 이 개인 객체를 다시 빌드하세요. `git2_util.h`는 표준 C 헤더만 제공합니다. 정확한 통합 및 설치 명령은 `cmake/PermissionParsers.cmake`에 있습니다.
