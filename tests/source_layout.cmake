if(IS_DIRECTORY "${SOURCE_ROOT}/src/iiLocalLLM")
    message(FATAL_ERROR "The source directory repeats the project name")
endif()
foreach(_file core/iiLocalLLM.h core/iiLocalLLM.cpp core/Types.h core/Types.cpp
        core/Parameters.h core/Parameters.cpp hardware/Hardware.h hardware/Hardware.cpp
        hardware/Memory.h hardware/Memory.cpp models/ModelCatalog.h models/ModelCatalog.cpp
        service/Service.h service/Service.cpp service/HttpApiServer.h service/HttpApiServer.cpp
        runtimes/Runtime.h runtimes/LlamaRuntime.cpp media/MediaContent.h media/MediaContent.cpp)
    if(NOT EXISTS "${SOURCE_ROOT}/src/${_file}")
        message(FATAL_ERROR "Missing responsibility-grouped source: ${_file}")
    endif()
endforeach()
