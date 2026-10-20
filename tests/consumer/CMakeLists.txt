cmake_minimum_required(VERSION 3.24)
project(iiLocalLLMConsumer LANGUAGES CXX)
find_package(iiLocalLLM 0.56 CONFIG REQUIRED)
enable_testing()
add_executable(iiLocalLLMConsumer ../hello_world.cpp)
target_link_libraries(iiLocalLLMConsumer PRIVATE iiLocalLLM::iiLocalLLM)
add_test(NAME iiLocalLLM.installed_consumer COMMAND iiLocalLLMConsumer)
add_executable(iiLocalLLMServiceConsumer consumer.cpp)
target_link_libraries(iiLocalLLMServiceConsumer PRIVATE iiLocalLLM::iiLocalLLM)
add_test(NAME iiLocalLLM.installed_service_consumer COMMAND iiLocalLLMServiceConsumer)
if(TARGET iiDecision::iiDecision OR TARGET iiMultimodalAgent::iiMultimodalAgent)
    message(FATAL_ERROR "Inference package leaked orchestration dependencies")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
get_property(_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_tests} PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION
    "TMPDIR=set:${CMAKE_CURRENT_BINARY_DIR}/tmp;TMP=set:${CMAKE_CURRENT_BINARY_DIR}/tmp;TEMP=set:${CMAKE_CURRENT_BINARY_DIR}/tmp;IILOCALLLM_APP_ENDPOINTS=set:${CMAKE_CURRENT_BINARY_DIR}/tmp/app-endpoints")
if(APPLE)
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:" "DYLD_FRAMEWORK_PATH=unset:"
        "DYLD_FALLBACK_LIBRARY_PATH=unset:" "DYLD_VERSIONED_LIBRARY_PATH=unset:"
        "DYLD_VERSIONED_FRAMEWORK_PATH=unset:")
endif()
