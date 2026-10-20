cmake_minimum_required(VERSION 3.24)
project(iiLocalLLM VERSION 0.56.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# File creation is owned by iiFileProvider.
find_package(iiFileProvider 0.6 CONFIG REQUIRED HINTS "$ENV{IIFILEPROVIDER_PREFIX}" "$ENV{HOME}/.local/SDK/iiFileProvider")
find_package(iiFileProviderNative 0.8.2 CONFIG REQUIRED HINTS "$ENV{IIFILEPROVIDER_PREFIX}" "$ENV{HOME}/.local/SDK/iiFileProvider")





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
# Qt and cpp-httplib are included in both library and executable targets.
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(BUILD_TESTING AND WIN32 AND MINGW)
    add_executable(iiLocalLLMWindowsThreadPowerTests tests/windows_thread_power.cpp)
    target_compile_features(iiLocalLLMWindowsThreadPowerTests PRIVATE cxx_std_23)
    add_test(NAME iiLocalLLM.windows_thread_power COMMAND iiLocalLLMWindowsThreadPowerTests)
endif()
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib/httplib.h" _httplib_sha256)
if(NOT _httplib_sha256 STREQUAL "5933c14b2d0f45212925ed18ca579841f5fce717f431fc20cec712423e905b10")
    message(FATAL_ERROR "Vendored cpp-httplib v0.54.1 checksum mismatch")
endif()

option(IILOCALLLM_WITH_LLAMA "Build the native llama.cpp runtime" ON)
set(IILOCALLLM_LLAMA_SOURCE_DIR "" CACHE PATH "Existing llama.cpp source checkout (optional)")
set(IILOCALLLM_TEST_GGUF "" CACHE FILEPATH "Local GGUF for optional real inference test")
set(IILOCALLLM_TEST_CHAT_GGUF "" CACHE FILEPATH "Pinned Qwen2.5 0.5B Q4_K_M GGUF for optional offline chat acceptance test")
set(IILOCALLLM_TEST_MLX_MODEL "" CACHE PATH "Local MLX model for optional real inference test")
set(IILOCALLLM_MLX_PYTHON "python3" CACHE STRING "Python interpreter containing mlx-lm")

# Official mlx-lm sampling helpers: dcbcf786c0cf56f9a12fabe9468c887781431ae2 (MIT).
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/runtimes/mlx_sample_utils.py" _mlx_sampler_sha)
if(NOT _mlx_sampler_sha STREQUAL "c93c1eef794725f9f7ce77b6212f61eb6d0fe17b9cd87c06cec6470ee12b07f2")
    message(FATAL_ERROR "Pinned MLX sampler checksum mismatch")
endif()

add_library(iiLocalLLM SHARED src/iiLocalLLM.h src/iiLocalLLM.cpp
    src/Types.h src/Types.cpp src/Parameters.h src/Parameters.cpp src/Hardware.h src/Hardware.cpp src/Memory.h src/Memory.cpp src/Runtime.h src/Service.h src/Service.cpp src/LocalIpcServer.h src/LocalIpcServer.cpp
    src/ModelManifest.h src/ModelManifest.cpp src/ModelCatalog.h src/ModelCatalog.cpp src/ModelInfo.h src/HttpApiServer.h src/HttpApiServer.cpp
    src/service/Scheduler.h src/service/Scheduler.cpp
    src/service/Conversation.h src/service/Conversation.cpp
    src/service/Managers.h src/service/Managers.cpp
    src/service/ModelManager.h src/service/ModelManager.cpp
    src/service/ModelResidencyManager.h src/service/ModelResidencyManager.cpp
    src/service/ModelRegistry.h src/service/ModelRegistry.cpp src/ModelPull.h catalog/catalog.qrc
    src/runtimes/LlamaRuntime.cpp src/runtimes/MlxRuntime.cpp)
target_sources(iiLocalLLM PRIVATE src/MediaContent.cpp src/MediaContent.h src/Multimodal.h)
add_library(iiLocalLLM::iiLocalLLM ALIAS iiLocalLLM)
target_compile_features(iiLocalLLM PUBLIC cxx_std_23)
target_compile_definitions(iiLocalLLM PRIVATE IILOCALLLM_BUILDING_LIBRARY)
if(WIN32)
    target_compile_definitions(iiLocalLLM PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
target_link_libraries(iiLocalLLM PUBLIC iiFileProvider::Native iiFileProvider::iiFileProvider Qt6::Core Qt6::Network PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(iiLocalLLM PRIVATE ws2_32)
endif()
if(APPLE)
    enable_language(OBJC OBJCXX)
    target_sources(iiLocalLLM PRIVATE src/hardware/Apple.mm)
    set_source_files_properties(src/hardware/Apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(iiLocalLLM PROPERTIES OBJCXX_STANDARD 23 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(iiLocalLLM PRIVATE "-framework Metal" "-framework Foundation")
else()
    target_sources(iiLocalLLM PRIVATE src/hardware/Platform.cpp)
    if(MINGW)
        # On case-insensitive filesystems our public Rpc.h shadows Windows rpc.h.
        find_file(_iilocal_windows_rpc_header rpc.h
            PATHS ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES} NO_DEFAULT_PATH REQUIRED)
        set_source_files_properties(src/hardware/Platform.cpp tests/hardware_tests.cpp
            PROPERTIES COMPILE_OPTIONS "-include;${_iilocal_windows_rpc_header}")
    endif()
    if(WIN32)
        target_link_libraries(iiLocalLLM PRIVATE dxgi)
    endif()
endif()
target_include_directories(iiLocalLLM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(iiLocalLLM PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(IILOCALLLM_WITH_LLAMA)
    include(cmake/Llama.cmake)
    target_compile_definitions(iiLocalLLM PRIVATE IILOCALLLM_WITH_LLAMA)
    target_link_libraries(iiLocalLLM PRIVATE llama-common mtmd vendor::stb)
endif()

add_executable(iiLocalLLMService tools/service_main.cpp)
add_executable(iiLocalLLMD tools/service_main.cpp)
add_executable(iillm tools/client_main.cpp tools/IpcClient.h tools/IpcClient.cpp tools/IpcEndpoint.h)
set(_versioned_entrypoints iillm iiLocalLLMD iiLocalLLMService)
target_compile_features(iillm PRIVATE cxx_std_23)
target_link_libraries(iillm PRIVATE iiFileProvider::Native iiFileProvider::iiFileProvider Qt6::Core Qt6::Network)
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
foreach(_entrypoint IN LISTS _versioned_entrypoints)
    target_compile_definitions(${_entrypoint} PRIVATE IILOCALLLM_APP_VERSION="${PROJECT_VERSION}")
endforeach()
if(BUILD_TESTING)
    add_executable(iiLocalLLMMemoryHeaderInteropTests tests/memory_header_interop.cpp)
    target_link_libraries(iiLocalLLMMemoryHeaderInteropTests PRIVATE iiLocalLLM::iiLocalLLM)
    add_test(NAME iiLocalLLM.MemoryHeaderInterop COMMAND iiLocalLLMMemoryHeaderInteropTests)
    if(IILOCALLLM_WITH_LLAMA)
        get_target_property(_llama_common_source_dir llama-common SOURCE_DIR)
        add_executable(iiLocalLLMNativeGrammarTests tests/native_grammar_tests.cpp)
        target_link_libraries(iiLocalLLMNativeGrammarTests PRIVATE iiFileProvider::Native iiFileProvider::iiFileProvider llama-common Qt6::Core Qt6::Test)
        target_include_directories(iiLocalLLMNativeGrammarTests PRIVATE "${_llama_common_source_dir}/../src")
        target_compile_definitions(iiLocalLLMNativeGrammarTests PRIVATE
            IILOCALLLM_LLAMA_TEMPLATES="${_llama_common_source_dir}/../models/templates")
        set_target_properties(iiLocalLLMNativeGrammarTests PROPERTIES AUTOMOC ON)
        add_test(NAME iiLocalLLM.native_grammar COMMAND iiLocalLLMNativeGrammarTests)
        set_tests_properties(iiLocalLLM.native_grammar PROPERTIES TIMEOUT 60)
    endif()
    add_executable(iiLocalLLMHelloWorld tests/hello_world.cpp)
    target_link_libraries(iiLocalLLMHelloWorld PRIVATE iiLocalLLM::iiLocalLLM)
    set_target_properties(iiLocalLLMHelloWorld PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiLocalLLM.hello_world COMMAND iiLocalLLMHelloWorld)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Test)
    if(IILOCALLLM_WITH_LLAMA)
        foreach(_kind VISION AUDIO)
            if(_kind STREQUAL "VISION")
                set(_mode vision)
                set(_media "${IILOCALLLM_TEST_IMAGE}")
            else()
                set(_mode audio)
                set(_media "${IILOCALLLM_TEST_AUDIO}")
            endif()
        endforeach()
    endif()
    add_executable(iiLocalLLMParameterTests tests/parameter_tests.cpp)
    target_link_libraries(iiLocalLLMParameterTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMParameterTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.parameters COMMAND iiLocalLLMParameterTests)
    add_executable(iiLocalLLMResidencyTests tests/residency_tests.cpp src/service/ModelResidencyManager.cpp)
    target_link_libraries(iiLocalLLMResidencyTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMResidencyTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.residency COMMAND iiLocalLLMResidencyTests)
    add_executable(iiLocalLLMServiceTests tests/service_tests.cpp)
    target_link_libraries(iiLocalLLMServiceTests PRIVATE iiLocalLLM::iiLocalLLM Qt6::Test)
    set_target_properties(iiLocalLLMServiceTests PROPERTIES AUTOMOC ON)
    add_test(NAME iiLocalLLM.service COMMAND iiLocalLLMServiceTests)
    set_tests_properties(iiLocalLLM.service PROPERTIES TIMEOUT 60)
    add_executable(iiLocalLLMHardwareTests tests/hardware_tests.cpp src/service/Managers.cpp)
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
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" Python3_EXECUTABLE)
    set(_versioned_executables)
    foreach(_entrypoint IN LISTS _versioned_entrypoints)
        list(APPEND _versioned_executables $<TARGET_FILE:${_entrypoint}>)
    endforeach()
    add_test(NAME iiLocalLLM.parameter_catalog COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/parameter_catalog_tests.py)
    if(UNIX)
        set(_cli_fixture)
        if(IILOCALLLM_WITH_LLAMA AND IILOCALLLM_TEST_GGUF)
            set(_cli_fixture "${IILOCALLLM_TEST_GGUF}")
        endif()
        add_test(NAME iiLocalLLM.cli COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.py
            $<TARGET_FILE:iiLocalLLMD> $<TARGET_FILE:iillm> ${_cli_fixture})
        set_tests_properties(iiLocalLLM.cli PROPERTIES TIMEOUT 150)
        if(IILOCALLLM_WITH_LLAMA AND IILOCALLLM_TEST_CHAT_GGUF)
            add_executable(iiLocalLLMNativeSchemaSmoke tests/native_schema_smoke.cpp)
            target_link_libraries(iiLocalLLMNativeSchemaSmoke PRIVATE iiLocalLLM::iiLocalLLM)
            add_test(NAME iiLocalLLM.native_schema COMMAND iiLocalLLMNativeSchemaSmoke "${IILOCALLLM_TEST_CHAT_GGUF}")
            set_tests_properties(iiLocalLLM.native_schema PROPERTIES TIMEOUT 90)
            add_executable(iiLocalLLMReasoningRuntimeSmoke tests/reasoning_runtime_smoke.cpp)
            target_link_libraries(iiLocalLLMReasoningRuntimeSmoke PRIVATE iiLocalLLM::iiLocalLLM)
            add_test(NAME iiLocalLLM.reasoning_template COMMAND iiLocalLLMReasoningRuntimeSmoke "${IILOCALLLM_TEST_CHAT_GGUF}")
            set_tests_properties(iiLocalLLM.reasoning_template PROPERTIES TIMEOUT 90)
            add_test(NAME iiLocalLLM.chat COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/chat_smoke.py
                $<TARGET_FILE:iiLocalLLMD> $<TARGET_FILE:iillm> "${IILOCALLLM_TEST_CHAT_GGUF}")
            set_tests_properties(iiLocalLLM.chat PROPERTIES TIMEOUT 300 LABELS "inference")
        endif()
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
        add_test(NAME iiLocalLLM.mlx_sampling COMMAND "${IILOCALLLM_MLX_PYTHON}" -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/mlx_sampler_tests.py)
        add_test(NAME iiLocalLLM.mlx_inference COMMAND iiLocalLLMRuntimeSmoke mlx "${IILOCALLLM_TEST_MLX_MODEL}"
            "${IILOCALLLM_MLX_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/src/runtimes/mlx_worker.py")
        set_tests_properties(iiLocalLLM.mlx_inference PROPERTIES TIMEOUT 180 LABELS "inference")
        add_test(NAME iiLocalLLM.mlx_cpu_inference COMMAND iiLocalLLMCpuRuntimeSmoke mlx "${IILOCALLLM_TEST_MLX_MODEL}"
            "${IILOCALLLM_MLX_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/src/runtimes/mlx_worker.py")
        set_tests_properties(iiLocalLLM.mlx_cpu_inference PROPERTIES TIMEOUT 180 LABELS "inference")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
    get_property(_iilocal_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_iilocal_tests} PROPERTIES ENVIRONMENT_MODIFICATION
        "TMPDIR=set:${CMAKE_CURRENT_BINARY_DIR}/tmp;TMP=set:${CMAKE_CURRENT_BINARY_DIR}/tmp;TEMP=set:${CMAKE_CURRENT_BINARY_DIR}/tmp;IILOCALLLM_APP_ENDPOINTS=set:${CMAKE_CURRENT_BINARY_DIR}/tmp/app-endpoints")
    if(APPLE)
        set_property(TEST ${_iilocal_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "DYLD_LIBRARY_PATH=unset:" "DYLD_FRAMEWORK_PATH=unset:"
            "DYLD_FALLBACK_LIBRARY_PATH=unset:" "DYLD_VERSIONED_LIBRARY_PATH=unset:"
            "DYLD_VERSIONED_FRAMEWORK_PATH=unset:")
    endif()
endif()
install(TARGETS iiLocalLLM EXPORT iiLocalLLMTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS iiLocalLLMService iiLocalLLMD iillm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/iiLocalLLM.h src/Types.h src/Parameters.h src/Hardware.h src/Memory.h src/ModelManifest.h src/ModelCatalog.h src/ModelInfo.h src/ModelPull.h src/Runtime.h src/Service.h src/Rpc.h src/LocalIpcServer.h src/HttpApiServer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/Multimodal.h src/MediaContent.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES src/Export.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES catalog/parameters.json catalog/parameter-sources.json catalog/parameter-coverage.json DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM)
install(FILES catalog/registry.json DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/docs)
install(FILES src/runtimes/mlx_worker.py src/runtimes/mlx_sample_utils.py src/runtimes/mlx-requirements.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/runtimes)
install(FILES third_party/cpp-httplib/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME cpp-httplib.LICENSE)
if(IILOCALLLM_WITH_LLAMA)
    get_target_property(_llama_source_dir llama SOURCE_DIR)
    install(FILES "${_llama_source_dir}/../LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME llama.cpp.LICENSE)
    install(DIRECTORY third_party/llama-common/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses/llama-common)
    install(DIRECTORY "${_llama_source_dir}/../vendor/miniaudio" "${_llama_source_dir}/../vendor/stb" "${_llama_source_dir}/../vendor/hash"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses/llama-mtmd PATTERN "CMakeLists.txt" EXCLUDE)
endif()
install(EXPORT iiLocalLLMTargets
    FILE iiLocalLLMTargets.cmake
    NAMESPACE iiLocalLLM::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiLocalLLM
)

if(WIN32 AND COMMAND onnxruntime_deploy)
    get_property(_iillm_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_iillm_target IN LISTS _iillm_targets)
        get_target_property(_iillm_type "${_iillm_target}" TYPE)
        if(_iillm_type STREQUAL "EXECUTABLE")
            onnxruntime_deploy("${_iillm_target}")
        endif()
    endforeach()
    install(FILES "$<TARGET_FILE:ONNXRuntime::ONNXRuntime>" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
configure_package_config_file(cmake/iiLocalLLMConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiLocalLLM
)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/iiLocalLLMConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiLocalLLM
)
