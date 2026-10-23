# Backport the per-layer pruned expert handling from ggml-org/llama.cpp
# cd0a5b6a180dff0048e5fd993098b21c0b2c91b8 (MIT). Keep fetched sources intact.
function(iilocal_fix_nemotron_bolt)
    get_target_property(_root llama SOURCE_DIR)
    set(_original "${_root}/models/nemotron-h.cpp")
    file(READ "${_original}" _contents)
    if(_contents MATCHES "n_expert_layer" AND _contents MATCHES "ffn_up_exps->ne\\[2\\]")
        return()
    endif()
    macro(_iilocal_nemotron_replace _before _after)
        string(FIND "${_contents}" "${_before}" _position)
        if(_position EQUAL -1)
            message(FATAL_ERROR "Unsupported Nemotron-H source; review the pruned expert backport")
        endif()
        string(REPLACE "${_before}" "${_after}" _contents "${_contents}")
    endmacro()
    _iilocal_nemotron_replace([=[            if (n_expert != 0) {
                // Use per-layer n_ff_exp;]=] [=[            if (n_expert != 0) {
                const auto * router_meta = ml.get_tensor_meta(tn(LLM_TENSOR_FFN_GATE_INP, "weight", i).str().c_str());
                const int64_t n_expert_layer = router_meta ? router_meta->ne[1] : n_expert;
                if (n_expert_layer < hparams.n_expert_used(i) || n_expert_layer > n_expert) {
                    throw std::runtime_error("Invalid per-layer Nemotron expert count");
                }
                // Use per-layer n_ff_exp;]=])
    _iilocal_nemotron_replace(
        [=[{ n_embd, n_expert}, trunk_flags);]=]
        [=[{ n_embd, n_expert_layer}, trunk_flags);]=])
    _iilocal_nemotron_replace(
        [=[{n_expert         }, trunk_flags);]=]
        [=[{n_expert_layer}, trunk_flags);]=])
    _iilocal_nemotron_replace(
        [=[{n_ff_exp_i,   moe_n_embd, n_expert}, trunk_flags);]=]
        [=[{n_ff_exp_i,   moe_n_embd, n_expert_layer}, trunk_flags);]=])
    _iilocal_nemotron_replace(
        [=[{moe_n_embd, n_ff_exp_i, n_expert}, trunk_flags);]=]
        [=[{moe_n_embd, n_ff_exp_i, n_expert_layer}, trunk_flags);]=])
    _iilocal_nemotron_replace(
        [=[n_expert, (int64_t)hparams.n_expert_used(il),]=]
        [=[model.layers[il].ffn_up_exps->ne[2], (int64_t)hparams.n_expert_used(il),]=])
    set(_copy "${CMAKE_CURRENT_BINARY_DIR}/llama-patches/nemotron-h.cpp")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/llama-patches")
    set(_existing "")
    if(EXISTS "${_copy}")
        file(READ "${_copy}" _existing)
    endif()
    if(NOT _existing STREQUAL _contents)
        file(WRITE "${_copy}" "${_contents}")
    endif()
    get_target_property(_sources llama SOURCES)
    set(_updated "")
    set(_found FALSE)
    foreach(_source IN LISTS _sources)
        get_filename_component(_absolute "${_source}" ABSOLUTE BASE_DIR "${_root}")
        if(_absolute STREQUAL _original)
            list(APPEND _updated "${_copy}")
            set(_found TRUE)
        else()
            list(APPEND _updated "${_source}")
        endif()
    endforeach()
    if(NOT _found)
        message(FATAL_ERROR "Cannot identify the Nemotron-H source for the expert backport")
    endif()
    set_source_files_properties("${_copy}" TARGET_DIRECTORY llama
        PROPERTIES INCLUDE_DIRECTORIES "${_root}/models")
    set_property(TARGET llama PROPERTY SOURCES "${_updated}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_original}")
endfunction()

# Upstream #30699: the batched MoE mapper needs a specialization for top-k=9.
# Only the disposable downloaded build tree is patched; explicit checkouts stay untouched.
function(iilocal_fix_metal_top9)
    if(NOT TARGET ggml-metal)
        return()
    endif()
    get_target_property(_metal_root ggml-metal SOURCE_DIR)
    set(_kernel "${_metal_root}/kernels/mul_mm.metal")
    file(READ "${_kernel}" _contents)
    if(_contents MATCHES "kernel_mul_mm_id_map0<9>")
        return()
    endif()
    file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" _build_root)
    file(REAL_PATH "${_kernel}" _real_kernel)
    cmake_path(IS_PREFIX _build_root "${_real_kernel}" NORMALIZE _inside_build)
    if(NOT _inside_build)
        message(FATAL_ERROR "The external llama.cpp checkout needs upstream Metal top-9 support (#30699)")
    endif()
    set(_anchor [=[template [[host_name("kernel_mul_mm_id_map0_ne20_8" )]] kernel kernel_mul_mm_id_map0_t kernel_mul_mm_id_map0<8>;]=])
    string(FIND "${_contents}" "${_anchor}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "Unsupported Metal MoE source; review upstream #30699")
    endif()
    set(_variant [=[template [[host_name("kernel_mul_mm_id_map0_ne20_9" )]] kernel kernel_mul_mm_id_map0_t kernel_mul_mm_id_map0<9>;]=])
    string(REPLACE "${_anchor}" "${_anchor}\n${_variant}" _patched "${_contents}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/llama-patches")
    configure_file("${_kernel}" "${CMAKE_CURRENT_BINARY_DIR}/llama-patches/mul_mm.original.metal" COPYONLY)
    file(WRITE "${_kernel}" "${_patched}")
endfunction()
