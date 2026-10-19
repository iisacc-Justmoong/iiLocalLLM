# iiLocalLLM: Windows 빌드 및 실행

llama.cpp 기반 로컬 언어 모델, 세션·메모리·shell·MCP 기능을 제공하는 Qt Core/Network SDK이다.

검증 기록 갱신: `2026-10-20T02:45:37+09:00`. 변경 전 소스 기준 commit: `ac3ca099b8a9ab460c380784a41e2e34ea9031ae`. 아래 결과는 이번 변경을 포함한 작업 트리의 검증이다.

## 현재 검증 상태

Release 빌드·실행 검사·설치를 완료하였다. 등록 검사 73개 중 73개 통과, 0개 실패, 0개 skip이다.

## 이 프로젝트의 의존성과 실행 범위

선행 workspace SDK는 다음과 같다. 각각의 Windows 지침에 따라 실행 검증과 설치를 완료한 뒤 진행한다.

- [iiDecision](../../../SDK/iiDecision/docs/WindowsBuildAndRun.md)
- [iiFileProvider](../../../SDK/iiFileProvider/docs/WindowsBuildAndRun.md)

- iiFileProvider와 Native, iiDecision 및 Qt Core/Network/Test 6.8.3 EXACT가 필요하다.
- 기본 빌드는 고정된 llama.cpp를 준비한다. Git for Windows의 `sh`를 PATH에 포함한다. 실제 토큰 생성에는 호환 로컬 모델이 별도로 필요하다.

프로젝트별 Windows 조건은 다음과 같다.

- 테스트 C++ 정의에 들어가는 Python 실행 경로는 CMake에서 `/` 구분자로 정규화한다. Windows의 `\t`, `\U`가 C++ escape로 해석되어 MCP peer를 시작하지 못하는 문제를 방지하며 HTTP MCP 테스트의 초기 검사로 실행 파일 경로를 확인한다.
- 세션 검색 cursor는 Windows에서 iiFileProvider가 정규화한 native 변경 토큰을 사용한다. 미리보기 발행 뒤에도 cursor가 유효하며, 같은 크기의 본문을 바꾸고 mtime을 복원한 경우에는 cursor를 거부하는 회귀 검사를 포함한다.
- background shell 검사는 Git for Windows의 MSYS PID를 해당 배포본의 `usr/bin/ps.exe`로 확인한다. 빌드 도구와 이름이 충돌할 수 있는 `usr/bin` 전체를 PATH 앞에 추가하지 않는다.
- 현재 재현 구성은 CPU backend이다. SIMD 및 GPU 옵션을 변경하면 다시 빌드·실행 검증한다.
- 선택적 LVRS 통합 테스트용 `IILOCALLLM_TEST_LVRS_DIR`은 이 구성에서 지정하지 않는다.
- MSVC의 대소문자 비구분 header 탐색에서 `Memory.h`와 C runtime `memory.h`가 만나는 경로는 현재 extern C++ 회귀 검사로 보호한다.

## 환경 준비

PowerShell에서 이 프로젝트 루트로 이동한 뒤 순서대로 실행한다. workspace 배치는 `Workspace/SDK/<이름>`을 전제로 한다.

검증 도구는 MSVC 19.51 x64, Windows SDK 10.0.28000, CMake 4.4.4, Ninja 1.13.2이다. 개발 shell은 빌드 재현을 위해 Python 3.14.8을 선택하며 시스템 기본 Python 3.15.0과 구분한다.

```powershell
$ErrorActionPreference = 'Stop'
$project = (Get-Location).Path
$workspace = (Resolve-Path -LiteralPath '../..').Path
. (Join-Path $workspace 'SDK/Windows/Enter-DevEnvironment.ps1')
Set-Location -LiteralPath $project
$python = (Join-Path $workspace 'Tools/Python314/tools/python.exe').Replace('\', '/')
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source.Replace('\', '/')
$toolchain = (Join-Path $workspace 'Tools/vcpkg/scripts/buildsystems/vcpkg.cmake').Replace('\', '/')
$msvcInclude = (Join-Path $workspace 'SDK/Windows/WindowsMsvcPrefix.cmake').Replace('\', '/')
$prefix = (Join-Path $env:USERPROFILE '.local/SDK/iiLocalLLM').Replace('\', '/')
$env:ONNXRuntime_ROOT = Join-Path $workspace 'Tools/ONNXRuntime/onnxruntime-win-x64-1.31.0'
```

`Enter-DevEnvironment.ps1`은 Qt MSVC kit, vcpkg, 설치된 SDK의 검색 경로와 UTF-8 설정을 준비한다. `CMAKE_PREFIX_PATH`의 디렉터리 존재만으로 의존성 검증을 대신하지 않는다. 선행 SDK의 실행 검사와 설치를 먼저 완료해야 한다.

## 구성과 빌드

```powershell
$configure = @(
    '-S', '.', '-B', 'build', '-G', 'Ninja'
    '-DCMAKE_BUILD_TYPE=Release'
    "-DCMAKE_C_COMPILER=$compiler"
    "-DCMAKE_CXX_COMPILER=$compiler"
    "-DCMAKE_INSTALL_PREFIX=$prefix"
    "-DCMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH"
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    '-DVCPKG_TARGET_TRIPLET=x64-windows'
    "-DCMAKE_PROJECT_INCLUDE=$msvcInclude"
    "-DPython3_EXECUTABLE=$python"
    "-DPython_EXECUTABLE=$python"
    '-DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6'
    '-DBUILD_TESTING=ON'
    "-DONNXRuntime_ROOT=$($env:ONNXRuntime_ROOT.Replace('\', '/'))"
    '-DIILOCALLLM_WITH_LLAMA=ON'
    '-DGGML_CUDA=OFF'
    '-DGGML_VULKAN=OFF'
    '-DGGML_NATIVE=ON'
    '-DGGML_AVX=OFF'
    '-DGGML_AVX2=OFF'
    '-DGGML_BMI2=OFF'
    '-DGGML_SSE42=OFF'
)
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake 구성 실패' }
$cache = Get-Content -LiteralPath 'build/CMakeCache.txt' -Raw
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') {
    throw 'Release 구성이 유실되었음. 같은 구성 인수에 --fresh를 추가하여 다시 구성할 것'
}
& $python (Join-Path $workspace 'SDK/Windows/Configure-WindowsNinja.py') (Join-Path $project 'build')
if ($LASTEXITCODE -ne 0) { throw 'Ninja MSVC 의존성 설정 실패' }
cmake --build build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw '빌드 실패' }
```

모든 생성 파일은 이 프로젝트의 `build/` 아래에 둔다. 컴파일러는 위에서 확인한 절대 경로를 사용한다. 기존 캐시가 다른 compiler 또는 generator를 사용한다면 같은 구성 인수에 `--fresh`를 추가하여 다시 구성한다. MSVC 키트와 MinGW 또는 Debug/Release 라이브러리를 혼합하지 않는다.

`WindowsMsvcPrefix.cmake`와 `Configure-WindowsNinja.py`는 MSVC include 출력 언어를 Ninja에 맞추고 UTF-8 실행 manifest를 적용한다. 빌드 병렬도는 4로 제한하며 실행 검사는 직렬로 수행한다.

## 실행과 확인

입력 대기열·compaction·agent profile·task 검사는 QtTest의 stdout logger와 파일 logger를 함께 사용한다. Windows launcher가 stdout을 연결하지 않는 경우에도 build/input-queue-tests.txt, build/compaction-tests.txt, build/agent-profile-tests.txt, build/task-tests.txt에 실제 assertion과 case 이름을 보존한다. 간헐 실패는 동일한 TEMP·작업 디렉터리·app endpoint 환경에서 반복 재현하며, 단독 재실행 성공만으로 전체 통과를 대신하지 않는다.

Windows의 다른 프로세스가 읽기 핸들을 닫는 동안 원자적 이름 교체가 Win32 5·32·33·303 계열 오류를 반환할 수 있다. 대기열은 기존 `queue.lock`을 유지하며 `InputQueueOptions::lockTimeoutMs` 안에서 새 임시 파일의 원자적 게시만 재시도한다. 직접 덮어쓰기 fallback은 사용하지 않으며, 각 재시도에서 취소와 symlink 여부를 확인한다. 지속적인 공유·권한 오류는 `StorageFailure`로 반환하고 이전 JSON과 revision을 유지한다. 잠금 파일의 삭제 지연도 같은 제한 시간 안에서 처리한다. 회귀 검사는 잠금 삭제 대기, 짧은 읽기 핸들, 지속적인 공유 오류의 데이터 보존 및 게시 대기 중 취소를 실제 Windows 핸들로 검증한다.

대기열 fixture의 임시 디렉터리는 iiFileProvider의 정리 adapter를 사용하여 preview 작업을 취소한 뒤 제거한다. 다중 프로세스 검사는 모든 producer를 회수한 후 각 오류를 검사하며, producer의 실제 오류 문자열을 보존한다.

경합 중 다른 작성자가 잠금 파일을 제거하여 Qt 잠금 취득이 Win32 2를 반환하는 경우에는 부모 디렉터리가 여전히 디렉터리이고 symlink가 아니며 canonical 경로가 같은지 재확인한 뒤 같은 잠금 제한 시간 안에서 다시 시도한다. 부모 경로가 사라지거나 바뀐 경우에는 저장 실패로 반환한다.

iiDecision의 ONNX/Laya 구성을 사용하는 Windows 빌드는 모든 CLI와 검사 실행 파일 옆에 일치하는 ONNX Runtime DLL을 배치하고 설치 `bin`에도 포함한다. 시스템 폴더의 이전 Windows ML DLL이 먼저 로드되는 일을 막는다.

실제 네이티브 테스트 프로세스로 세션·메모리·shell·discovery·MCP·문법과 DLL header 호환성을 검증한다.

```powershell
$dllDirectories = @(Get-ChildItem -LiteralPath build -Recurse -Filter '*.dll' -File |
    Select-Object -ExpandProperty DirectoryName | Sort-Object -Unique)
$env:PATH = (@($dllDirectories) + @($env:PATH)) -join ';'
$env:QSG_RENDER_LOOP = 'basic'
ctest --test-dir build -C Release --output-on-failure --parallel 1 --no-tests=error
if ($LASTEXITCODE -ne 0) { throw '전체 실행 검사 실패' }
```

현재 빌드에 실제 등록된 실행 경로의 예는 `iiLocalLLM.MemoryHeaderInterop`, `iiLocalLLM.native_grammar`, `iiLocalLLM.teams`이다. 전체 목록은 `ctest --test-dir build -N`으로 확인한다.

## 설치와 배포 범위

전체 테스트가 통과한 구성만 설치한다. skip된 선택 기능은 지원·실행 범위를 별도 기록한다.

```powershell
cmake --install build --config Release --prefix "$prefix"
if ($LASTEXITCODE -ne 0) { throw '설치 실패' }
```

설치 파일은 `build/install_manifest.txt`와 실제 prefix의 파일을 대조한다. 소비자 실행 시 prefix의 `bin`과 필요한 vcpkg DLL을 검색할 수 있어야 한다.

## 검증 근거

- 빌드 정의: [CMakeLists.txt](../CMakeLists.txt)와 프로젝트 테스트 등록이다.
- 이번 실행 결과: [Windows SDK 전체 검증 기록](https://github.com/iisacc-Justmoong/SDK/blob/main/Windows/Verification-20261020.md)이다.
- 프로젝트별 실행 로그와 JUnit은 같은 검증 디렉터리에 보관한다. 로그의 종료 코드, 실패·skip 목록과 실제 실행 범위를 함께 확인한다.

이전 세션 기록의 v1→v2 변환은 lease의 쓰기 파일을 닫은 뒤 해당 파일의 iiFileProvider preview를 취소하고 원자적으로 교체한다. 닫기 전에 preview를 취소하면 닫기 동작이 새 읽기 작업을 생성하여 Windows 이름 교체와 경합할 수 있다. 회귀 검사는 원본 메시지 전체 보존, 잘린 마지막 checkpoint 복구 및 완전하지만 손상된 기록의 거부를 확인한다. compaction fixture도 provider 임시 디렉터리 adapter로 preview 종료 후 정리한다.

백그라운드 agent 작업은 실행 시작 시 mutex 안에서 JSON 상태 사본을 확보한 뒤 const 사본에서 실행 설정을 읽는다. 공유 QJsonObject의 비상수 subscript는 읽기 용도로 사용해도 누락된 key를 삽입할 수 있으므로 잠금 밖에서 사용하지 않는다. 회귀 검사는 32개 background 작업을 연속으로 시작하면서 즉시 상태·완료·소유권을 조회하고, 잘못된 profile catalog가 생긴 후에도 정리와 조회가 유지되는지 검사한다.

TaskStore의 잠금도 Windows 삭제 지연과 잠금 파일 경로 소멸을 부모 canonical 경로 재검사 및 lockTimeoutMs 안에서 처리한다. 취소·영속 저장 오류·실제 소유권 경합의 Timeout 구분을 유지한다. task worker는 JSON 응답을 명시적으로 flush하며, 다중 프로세스 fixture는 생성 응답을 소비한 후 claim 단계로 넘어가고 JSON·정상 종료·단일 claim 성공을 모두 확인한다. Windows 핸들로 잠금 삭제 지연과 대기 중 취소를 회귀 검사한다.

TaskStore의 board.json 원자적 게시도 다른 프로세스의 닫히는 읽기 핸들로 인한 Win32 공유 오류만 board.lock을 유지한 채 lockTimeoutMs 안에서 재시도한다. 직접 덮어쓰기 fallback은 사용하지 않으며 취소·symlink·영속 오류를 계속 검사한다. 짧은 읽기 핸들 해제, 영속 오류의 원본 board 보존, 게시 대기 중 취소를 Windows 회귀 검사로 확인한다.

전체 SDK의 실행·설치와 커밋 조건은 [공통 Windows 검증 도구](https://github.com/iisacc-Justmoong/SDK/blob/main/Windows/README.md)에서 재현한다. 원본 JSON·JUnit·상세 로그는 로컬 `Workspace/build/maintenance/20261019-221920-sdk-recovery/final/`에 보관하며 Git에는 포함하지 않는다. 이 프로젝트의 성공은 제품·다른 플랫폼·별도 계정 및 물리 장치의 실연동을 대신하지 않는다.
