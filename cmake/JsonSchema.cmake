# Private header-only schema engine; no JSON types enter the installed C++ ABI.
include(FetchContent)
FetchContent_Declare(iilocal_jsoncons
    URL https://codeload.github.com/danielaparker/jsoncons/tar.gz/refs/tags/v1.9.0
    URL_HASH SHA256=f1017b36e4e034acd5c0f5f616bacf5d7a161d6d3a43ff9ddb73fd8dca4d3cd9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/jsoncons"
    SOURCE_SUBDIR unused-no-cmake-project)
FetchContent_MakeAvailable(iilocal_jsoncons)
target_include_directories(iiLocalLLM SYSTEM PRIVATE "${iilocal_jsoncons_SOURCE_DIR}/include")
