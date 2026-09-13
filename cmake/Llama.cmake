# Pin the C API we build and test. No network access occurs when the backend is disabled.
function(iilocal_add_llama)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
    set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
    set(LLAMA_LLGUIDANCE OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
    # Package all detected toolchains. Runtime policy, never the client, chooses the device.
    # Explicit packaging flags still produce a normal configure error when a required SDK is missing.
    find_package(CUDAToolkit QUIET)
    set(_cuda_default OFF)
    if(CUDAToolkit_FOUND AND CUDAToolkit_NVCC_EXECUTABLE)
        set(_cuda_default ON)
    endif()
    set(GGML_CUDA ${_cuda_default} CACHE BOOL "Compile CUDA when its toolkit and compiler are installed")
    find_package(Vulkan QUIET COMPONENTS glslc)
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
    endif()
    find_package(SPIRV-Headers CONFIG QUIET)
    set(_vulkan_default OFF)
    if(Vulkan_FOUND AND TARGET SPIRV-Headers::SPIRV-Headers)
        set(_vulkan_default ON)
    endif()
    set(GGML_VULKAN ${_vulkan_default} CACHE BOOL "Compile Vulkan when its SDK and shader tools are installed")
    if(IILOCALLLM_LLAMA_SOURCE_DIR)
        add_subdirectory("${IILOCALLLM_LLAMA_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/llama/build" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(iilocal_llama
            URL https://codeload.github.com/ggml-org/llama.cpp/tar.gz/5202104b59ada9005db079eea43882a2b7bf5802
            URL_HASH SHA256=58345c999af65b5dec07b71601eabbb0a30cc1f8842d4f00190b87b5a776fc7e
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/llama.cpp"
            BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/llama/build"
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(iilocal_llama)
    endif()
    # The upstream common library includes its own compiled HTTP implementation.
    # Isolate its symbols from our independently pinned header-only HTTP server.
    target_compile_definitions(llama-common PRIVATE httplib=iillm_llama_httplib)
    target_compile_definitions(cpp-httplib PRIVATE httplib=iillm_llama_httplib)
endfunction()
iilocal_add_llama()
