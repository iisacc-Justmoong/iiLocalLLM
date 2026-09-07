cmake_minimum_required(VERSION 3.24)
project(iiLocalLLM VERSION 0.2.0 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    if(DEFINED ENV{HOME})
        set(_user_home "$ENV{HOME}")
    elseif(DEFINED ENV{USERPROFILE})
        set(_user_home "$ENV{USERPROFILE}")
    else()
        message(FATAL_ERROR "Set CMAKE_INSTALL_PREFIX explicitly: no user home is available")
    endif()
    set(CMAKE_INSTALL_PREFIX "${_user_home}/.local/SDK/iiLocalLLM" CACHE PATH "Install prefix" FORCE)
endif()

find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Network)
find_package(Threads REQUIRED)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib/httplib.h" _httplib_sha256)
if(NOT _httplib_sha256 STREQUAL "5933c14b2d0f45212925ed18ca579841f5fce717f431fc20cec712423e905b10")
    message(FATAL_ERROR "Vendored cpp-httplib v0.54.1 checksum mismatch")
endif()

option(IILOCALLLM_WITH_LLAMA "Build the native llama.cpp runtime" OFF)
set(IILOCALLLM_LLAMA_SOURCE_DIR "" CACHE PATH "Existing llama.cpp source checkout (optional)")
set(IILOCALLLM_TEST_GGUF "" CACHE FILEPATH "Local GGUF for optional real inference test")
set(IILOCALLLM_TEST_MLX_MODEL "" CACHE PATH "Local MLX model for optional real inference test")
set(IILOCALLLM_MLX_PYTHON "python3" CACHE STRING "Python interpreter containing mlx-lm")

add_library(iiLocalLLM SHARED iiLocalLLM.h iiLocalLLM.cpp
    Types.h Types.cpp Hardware.h Hardware.cpp Memory.h Memory.cpp Runtime.h Service.h Service.cpp LocalIpcServer.h LocalIpcServer.cpp
    ModelManifest.h ModelManifest.cpp ModelCatalog.h ModelCatalog.cpp ModelInfo.h HttpApiServer.h HttpApiServer.cpp
    service/Scheduler.h service/Scheduler.cpp
    service/Managers.h service/Managers.cpp
    service/ModelManager.h service/ModelManager.cpp
    service/ModelResidencyManager.h service/ModelResidencyManager.cpp
    service/ModelRegistry.h service/ModelRegistry.cpp ModelPull.h catalog/catalog.qrc
    runtimes/LlamaRuntime.cpp runtimes/MlxRuntime.cpp)
add_library(iiLocalLLM::iiLocalLLM ALIAS iiLocalLLM)
target_compile_features(iiLocalLLM PUBLIC cxx_std_20)
target_compile_definitions(iiLocalLLM PRIVATE IILOCALLLM_BUILDING_LIBRARY)
target_link_libraries(iiLocalLLM PUBLIC Qt6::Core Qt6::Network PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(iiLocalLLM PRIVATE ws2_32)
endif()
if(APPLE)
    enable_language(OBJCXX)
    target_sources(iiLocalLLM PRIVATE hardware/Apple.mm)
    set_source_files_properties(hardware/Apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(iiLocalLLM PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(iiLocalLLM PRIVATE "-framework Metal" "-framework Foundation")
else()
    target_sources(iiLocalLLM PRIVATE hardware/Platform.cpp)
    if(WIN32)
        target_link_libraries(iiLocalLLM PRIVATE dxgi)
    endif()
endif()
target_include_directories(iiLocalLLM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(iiLocalLLM PROPERTIES
    AUTORCC ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(IILOCALLLM_WITH_LLAMA)
    include(cmake/Llama.cmake)
    target_compile_definitions(iiLocalLLM PRIVATE IILOCALLLM_WITH_LLAMA)
    target_link_libraries(iiLocalLLM PRIVATE llama)
endif()

add_executable(iiLocalLLMService tools/service_main.cpp)
add_executable(iiLocalLLMD tools/service_main.cpp)
add_executable(iillm tools/client_main.cpp tools/IpcClient.h tools/IpcClient.cpp tools/IpcEndpoint.h)
target_compile_features(iillm PRIVATE cxx_std_20)
target_link_libraries(iillm PRIVATE Qt6::Core Qt6::Network)
set_target_properties(iillm PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
set_target_properties(iiLocalLLMService PROPERTIES OUTPUT_NAME iilocal-llm-service INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set_target_properties(iiLocalLLMService PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
    set_target_properties(iiLocalLLMService PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
target_link_libraries(iiLocalLLMService PRIVATE iiLocalLLM::iiLocalLLM)
target_link_libraries(iiLocalLLMD PRIVATE iiLocalLLM::iiLocalLLM)
get_target_property(_daemon_rpath iiLocalLLMService INSTALL_RPATH)
set_target_properties(iiLocalLLMD PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE INSTALL_RPATH "${_daemon_rpath}")

if(BUILD_TESTING)
    add_executable(iiLocalLLMHelloWorld tests/hello_world.cpp)
    target_link_libraries(iiLocalLLMHelloWorld PRIVATE iiLocalLLM::iiLocalLLM)
    set_target_properties(iiLocalLLMHelloWorld PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiLocalLLM.hello_world COMMAND iiLocalLLMHelloWorld)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    add_executable(iiLocalLLMResidencyTests tests/residency_tests.cpp service/ModelResidencyManager.cpp)
    target_link_libraries(iiLocalLLMResidencyTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMResidencyTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.residency COMMAND iiLocalLLMResidencyTests)
    add_executable(iiLocalLLMServiceTests tests/service_tests.cpp)
    target_link_libraries(iiLocalLLMServiceTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMServiceTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.service COMMAND iiLocalLLMServiceTests)
    set_tests_properties(iiLocalLLM.service PROPERTIES TIMEOUT 60)
    add_executable(iiLocalLLMHardwareTests tests/hardware_tests.cpp service/Managers.cpp)
    target_link_libraries(iiLocalLLMHardwareTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMHardwareTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.hardware_policy COMMAND iiLocalLLMHardwareTests)
    set_tests_properties(iiLocalLLM.hardware_policy PROPERTIES TIMEOUT 60)
    add_executable(iiLocalLLMCatalogTests tests/catalog_tests.cpp)
    target_link_libraries(iiLocalLLMCatalogTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMCatalogTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.model_catalog COMMAND iiLocalLLMCatalogTests)
    set_tests_properties(iiLocalLLM.model_catalog PROPERTIES TIMEOUT 60)
    add_executable(iiLocalLLMHttpTests tests/http_tests.cpp)
    target_link_libraries(iiLocalLLMHttpTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMHttpTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.http COMMAND iiLocalLLMHttpTests)
    set_tests_properties(iiLocalLLM.http PROPERTIES TIMEOUT 90)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(UNIX)
        set(_cli_fixture)
        if(IILOCALLLM_WITH_LLAMA AND IILOCALLLM_TEST_GGUF)
            set(_cli_fixture "${IILOCALLLM_TEST_GGUF}")
        endif()
        add_test(NAME iiLocalLLM.cli COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.py
            $<TARGET_FILE:iiLocalLLMD> $<TARGET_FILE:iillm> ${_cli_fixture})
        set_tests_properties(iiLocalLLM.cli PROPERTIES TIMEOUT 150)
    endif()
    add_test(NAME iiLocalLLM.mlx_cache_policy COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/mlx_worker_tests.py)
    add_executable(iiLocalLLMRuntimeSmoke tests/runtime_smoke.cpp)
    target_link_libraries(iiLocalLLMRuntimeSmoke PRIVATE iiLocalLLM::iiLocalLLM)
    add_executable(iiLocalLLMCpuRuntimeSmoke tests/cpu_runtime_smoke.cpp)
    target_link_libraries(iiLocalLLMCpuRuntimeSmoke PRIVATE iiLocalLLM::iiLocalLLM)
    if(IILOCALLLM_WITH_LLAMA AND IILOCALLLM_TEST_GGUF)
        add_test(NAME iiLocalLLM.llama_inference COMMAND iiLocalLLMRuntimeSmoke llama.cpp "${IILOCALLLM_TEST_GGUF}" chatml)
        set_tests_properties(iiLocalLLM.llama_inference PROPERTIES TIMEOUT 180 LABELS "inference")
        add_test(NAME iiLocalLLM.llama_cpu_inference COMMAND iiLocalLLMCpuRuntimeSmoke llama.cpp "${IILOCALLLM_TEST_GGUF}")
        set_tests_properties(iiLocalLLM.llama_cpu_inference PROPERTIES TIMEOUT 180 LABELS "inference")
        if(UNIX)
            add_test(NAME iiLocalLLM.daemon COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon_smoke.py
                $<TARGET_FILE:iiLocalLLMService> "${IILOCALLLM_TEST_GGUF}")
            set_tests_properties(iiLocalLLM.daemon PROPERTIES TIMEOUT 70 LABELS "inference")
        endif()
    endif()
    if(IILOCALLLM_TEST_MLX_MODEL)
        add_test(NAME iiLocalLLM.mlx_inference COMMAND iiLocalLLMRuntimeSmoke mlx "${IILOCALLLM_TEST_MLX_MODEL}"
            "${IILOCALLLM_MLX_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/runtimes/mlx_worker.py")
        set_tests_properties(iiLocalLLM.mlx_inference PROPERTIES TIMEOUT 180 LABELS "inference")
        add_test(NAME iiLocalLLM.mlx_cpu_inference COMMAND iiLocalLLMCpuRuntimeSmoke mlx "${IILOCALLLM_TEST_MLX_MODEL}"
            "${IILOCALLLM_MLX_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/runtimes/mlx_worker.py")
        set_tests_properties(iiLocalLLM.mlx_cpu_inference PROPERTIES TIMEOUT 180 LABELS "inference")
    endif()
endif()

install(TARGETS iiLocalLLM EXPORT iiLocalLLMTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS iiLocalLLMService iiLocalLLMD iillm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES iiLocalLLM.h Types.h Hardware.h Memory.h ModelManifest.h ModelCatalog.h ModelInfo.h ModelPull.h Runtime.h Service.h LocalIpcServer.h HttpApiServer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES catalog/registry.json DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/docs)
install(FILES runtimes/mlx_worker.py runtimes/mlx-requirements.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/runtimes)
install(FILES third_party/cpp-httplib/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME cpp-httplib.LICENSE)
if(IILOCALLLM_WITH_LLAMA)
    get_target_property(_llama_source_dir llama SOURCE_DIR)
    install(FILES "${_llama_source_dir}/../LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME llama.cpp.LICENSE)
endif()
install(EXPORT iiLocalLLMTargets
    FILE iiLocalLLMTargets.cmake
    NAMESPACE iiLocalLLM::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiLocalLLM
)
configure_package_config_file(cmake/iiLocalLLMConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiLocalLLM
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiLocalLLM
)
