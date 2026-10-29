# Preserve flat public include names while grouping authored source by responsibility.
foreach(_area IN ITEMS core hardware models runtimes service media)
    file(GLOB _headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${_area}/*.h")
    foreach(_header IN LISTS _headers)
        get_filename_component(_name "${_header}" NAME)
        if(DEFINED _iillm_header_${_name})
            message(FATAL_ERROR "Duplicate iiLocalLLM header basename: ${_name}")
        endif()
        set("_iillm_header_${_name}" "${_header}")
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/${_name}"
            CONTENT "#pragma once\n#include \"${_header}\"\n")
    endforeach()
endforeach()
