# Small private C parsers. No parser types or runtime dependencies enter the SDK ABI.
enable_language(C)
include(FetchContent)
FetchContent_Declare(iilocal_md4c
    URL https://codeload.github.com/mity/md4c/tar.gz/refs/tags/release-0.5.3
    URL_HASH SHA256=353c346f376b87c954a13f3415ede2d51264cc61dc5abcd38ff1d2aa0d059b9e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/md4c"
    SOURCE_SUBDIR unused-no-cmake-project)
FetchContent_Declare(iilocal_yaml
    URL https://codeload.github.com/yaml/libyaml/tar.gz/refs/tags/0.2.5
    URL_HASH SHA256=fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/dependencies/libyaml"
    SOURCE_SUBDIR unused-no-cmake-project)
FetchContent_MakeAvailable(iilocal_md4c iilocal_yaml)
add_library(iilocal_context_parsers OBJECT "${iilocal_md4c_SOURCE_DIR}/src/md4c.c")
foreach(_source api dumper emitter loader parser reader scanner writer)
    target_sources(iilocal_context_parsers PRIVATE "${iilocal_yaml_SOURCE_DIR}/src/${_source}.c")
endforeach()
target_include_directories(iilocal_context_parsers PRIVATE "${iilocal_yaml_SOURCE_DIR}/include")
target_compile_definitions(iilocal_context_parsers PRIVATE YAML_DECLARE_STATIC
    YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5")
set_target_properties(iilocal_context_parsers PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 99)
target_sources(iiLocalLLM PRIVATE $<TARGET_OBJECTS:iilocal_context_parsers>)
target_include_directories(iiLocalLLM SYSTEM PRIVATE "${iilocal_md4c_SOURCE_DIR}/src" "${iilocal_yaml_SOURCE_DIR}/include")
target_compile_definitions(iiLocalLLM PRIVATE YAML_DECLARE_STATIC)
install(FILES "${iilocal_md4c_SOURCE_DIR}/LICENSE.md" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME md4c.LICENSE)
install(FILES "${iilocal_yaml_SOURCE_DIR}/License" DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM/licenses RENAME libyaml.LICENSE)
