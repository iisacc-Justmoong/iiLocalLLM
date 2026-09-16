# Private HTML5/encoding parser; no Lexbor types, symbols or install dependencies
# enter the SDK ABI. v3.0.0 uses Apache-2.0. Only required modules are compiled.
include(FetchContent)
FetchContent_Declare(iilocal_lexbor
    URL https://codeload.github.com/lexbor/lexbor/tar.gz/refs/tags/v3.0.0
    URL_HASH SHA256=eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/lexbor"
    SOURCE_SUBDIR unused-no-cmake-project)
FetchContent_MakeAvailable(iilocal_lexbor)
set(_web_sources)
foreach(_module core dom ns tag html encoding)
    file(GLOB_RECURSE _module_sources CONFIGURE_DEPENDS "${iilocal_lexbor_SOURCE_DIR}/source/lexbor/${_module}/*.c")
    list(APPEND _web_sources ${_module_sources})
endforeach()
if(WIN32)
    set(_web_port windows_nt)
else()
    set(_web_port posix)
endif()
file(GLOB _port_sources CONFIGURE_DEPENDS "${iilocal_lexbor_SOURCE_DIR}/source/lexbor/ports/${_web_port}/lexbor/core/*.c")
add_library(iilocal_web_parser OBJECT ${_web_sources} ${_port_sources})
target_include_directories(iilocal_web_parser PRIVATE "${iilocal_lexbor_SOURCE_DIR}/source")
target_compile_definitions(iilocal_web_parser PRIVATE LEXBOR_STATIC)
set_target_properties(iilocal_web_parser PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 99)
target_sources(iiLocalLLM PRIVATE $<TARGET_OBJECTS:iilocal_web_parser>)
target_include_directories(iiLocalLLM SYSTEM PRIVATE "${iilocal_lexbor_SOURCE_DIR}/source")
target_compile_definitions(iiLocalLLM PRIVATE LEXBOR_STATIC)
install(FILES "${iilocal_lexbor_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME lexbor.LICENSE)

install(FILES "${iilocal_lexbor_SOURCE_DIR}/NOTICE" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME lexbor.NOTICE)
