cmake_minimum_required(VERSION 3.24)
project(iiLocalLLM VERSION 0.1.0 LANGUAGES CXX)

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

find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

add_library(iiLocalLLM SHARED iiLocalLLM.h iiLocalLLM.cpp)
add_library(iiLocalLLM::iiLocalLLM ALIAS iiLocalLLM)
target_compile_features(iiLocalLLM PUBLIC cxx_std_20)
target_compile_definitions(iiLocalLLM PRIVATE IILOCALLLM_BUILDING_LIBRARY)
target_link_libraries(iiLocalLLM PUBLIC Qt6::Core)
target_include_directories(iiLocalLLM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(iiLocalLLM PROPERTIES
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

if(BUILD_TESTING)
    add_executable(iiLocalLLMHelloWorld tests/hello_world.cpp)
    target_link_libraries(iiLocalLLMHelloWorld PRIVATE iiLocalLLM::iiLocalLLM)
    set_target_properties(iiLocalLLMHelloWorld PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME iiLocalLLM.hello_world COMMAND iiLocalLLMHelloWorld)
endif()

install(TARGETS iiLocalLLM EXPORT iiLocalLLMTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES iiLocalLLM.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiLocalLLM)
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
