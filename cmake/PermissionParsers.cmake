# Private C syntax parsers; no parser ABI or extra runtime process is exposed.
include(FetchContent)
FetchContent_Declare(iilocal_tree_sitter
    URL https://codeload.github.com/tree-sitter/tree-sitter/tar.gz/refs/tags/v0.27.0
    URL_HASH SHA256=d35c96e68736bd9569d2757c3cc71052485f33082c3825f1aed9d0e86013a159
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/tree-sitter"
    SOURCE_SUBDIR unused-no-cmake-project)
FetchContent_Declare(iilocal_tree_sitter_bash
    URL https://codeload.github.com/tree-sitter/tree-sitter-bash/tar.gz/refs/tags/v0.25.1
    URL_HASH SHA256=2e785a761225b6c433410ef9c7b63cfb0a4e83a35a19e0f2aec140b42c06b52d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/tree-sitter-bash"
    SOURCE_SUBDIR unused-no-cmake-project)
FetchContent_MakeAvailable(iilocal_tree_sitter iilocal_tree_sitter_bash)
add_library(iilocal_permission_parsers OBJECT
    "${iilocal_tree_sitter_SOURCE_DIR}/lib/src/lib.c"
    "${iilocal_tree_sitter_bash_SOURCE_DIR}/src/parser.c"
    "${iilocal_tree_sitter_bash_SOURCE_DIR}/src/scanner.c")
target_include_directories(iilocal_permission_parsers PRIVATE
    "${iilocal_tree_sitter_SOURCE_DIR}/lib/include"
    "${iilocal_tree_sitter_SOURCE_DIR}/lib/src"
    "${iilocal_tree_sitter_bash_SOURCE_DIR}/src")
set_target_properties(iilocal_permission_parsers PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 11)
target_compile_definitions(iilocal_permission_parsers PRIVATE TREE_SITTER_HIDE_SYMBOLS)
set_property(SOURCE agent/PermissionRules.cpp APPEND PROPERTY COMPILE_DEFINITIONS TREE_SITTER_HIDE_SYMBOLS)
target_sources(iiLocalLLM PRIVATE $<TARGET_OBJECTS:iilocal_permission_parsers>)
target_include_directories(iiLocalLLM SYSTEM PRIVATE "${iilocal_tree_sitter_SOURCE_DIR}/lib/include")
add_library(iilocal_wildmatch OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wildmatch/wildmatch.c")
set_target_properties(iilocal_wildmatch PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 11)
target_sources(iiLocalLLM PRIVATE $<TARGET_OBJECTS:iilocal_wildmatch>)
target_include_directories(iiLocalLLM SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wildmatch")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wildmatch/COPYING" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME libgit2-wildmatch.COPYING)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wildmatch/" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses/libgit2-wildmatch-source)
install(FILES "${iilocal_tree_sitter_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME tree-sitter.LICENSE)
install(FILES "${iilocal_tree_sitter_bash_SOURCE_DIR}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME tree-sitter-bash.LICENSE)
