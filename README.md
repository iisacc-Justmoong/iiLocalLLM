# iiLocalLLM

C++20 및 Qt 6.8.3 Core를 사용하는 버전 0.1.0 동적 placeholder 라이브러리이다. 실제 도메인 기능은 제공하지 않으며, `iiLocalLLM::helloWorld()`가 정확히 `Hello world!`를 반환하는 최소 SDK이다. 외부 의존성은 기존 Qt Core뿐이다.

## 공개 API

```cpp
#include <iiLocalLLM.h>

const QString message = iiLocalLLM::helloWorld(); // Hello world!
```

공개 헤더와 소스는 프로젝트 루트에 함께 둔다. 공개 함수는 플랫폼별 export/import 매크로를 사용한다. CMake 타깃은 `iiLocalLLM::iiLocalLLM`이며 C++20 요구와 Qt Core 의존성을 소비자에게 전달한다.

## 빌드, 테스트, 설치

CMake 3.24 이상, C++20 컴파일러, Qt **6.8.3** Core 개발 파일이 필요하다. macOS에서는 기본으로 `$HOME/Qt/6.8.3/macos`를 탐색한다.

```sh
./install.sh
```

단독 빌드에서 기본 설치 경로를 적용하며, 상위 CMake 프로젝트에 포함할 때에는 상위 프로젝트의 설치 경로를 유지한다. 스크립트는 `build/`에서 Release 구성·빌드·CTest를 실행하고 `$HOME/.local/SDK/iiLocalLLM`에 설치한다. 이어 설치된 CMake 패키지만 소비하는 별도 프로젝트를 `build/consumer/build/`에 구성하고 빌드·CTest를 실행한다. 두 테스트 모두 C++20 및 Qt 헤더 버전, 실제 Qt 런타임 버전, 반환 문자열을 검증한다.

설정은 명령행 인자 대신 환경변수로 지정한다. `CMAKE_PREFIX_PATH`는 세미콜론으로 구분하는 추가 CMake 검색 경로이다.

```sh
INSTALL_PREFIX="$HOME/.local/SDK/iiLocalLLM" \
QT_PREFIX_PATH="$HOME/Qt/6.8.3/macos" \
CMAKE_PREFIX_PATH="/additional/prefix" \
./install.sh
```

수동 실행도 가능하다. 모든 빌드 산출물은 `build/` 아래에 둔다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos" \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/SDK/iiLocalLLM"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release
cmake -S tests/consumer -B build/consumer/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiLocalLLM;$HOME/Qt/6.8.3/macos"
cmake --build build/consumer/build --config Release --parallel
ctest --test-dir build/consumer/build -C Release --output-on-failure
```

## 설치 패키지 사용

```cmake
find_package(iiLocalLLM 0.1.0 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE iiLocalLLM::iiLocalLLM)
```

소비자를 구성할 때 SDK 설치 경로와 Qt 경로를 `CMAKE_PREFIX_PATH`에 추가한다. 기본 설치 구성은 다음과 같다.

- `include/iiLocalLLM.h`: 공개 헤더
- `lib/`: 버전이 있는 공유 라이브러리; Windows 런타임 DLL은 `bin/`
- `lib/cmake/iiLocalLLM/`: Config, ConfigVersion 및 Targets 패키지
- `share/iiLocalLLM/README.md`: 이 문서

Qt 자체는 재설치하거나 번들링하지 않는다. 실행 환경에도 Qt 6.8.3 Core가 있어야 한다. 설치 RPATH에 링크 의존 경로를 반영하며, 운영체제별 Qt 배포가 필요한 제품화 작업은 이 placeholder의 범위 밖이다. Qt 사용 조건은 기존 Qt 설치의 라이선스를 따른다.

## License

SPDX-License-Identifier: AGPL-3.0-only

iiLocalLLM의 자체 작성 코드와 문서는 GNU Affero General Public License v3.0 전용으로
배포한다. 전체 조건은 [LICENSE](LICENSE)를 따른다.

Qt를 포함한 외부 라이브러리와 별도 고지가 있는 서드파티 코드는 각자의 라이선스를
유지한다. 이 프로젝트의 라이선스 선언은 해당 서드파티 라이선스를 대체하지 않는다.
