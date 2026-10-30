function(sgc_embed_runtime)
    cmake_parse_arguments(ARG "" "OUTPUT;ROOT" "SOURCES" ${ARGN})

    if(NOT ARG_OUTPUT)
        message(FATAL_ERROR "sgc_embed_runtime: OUTPUT is required")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "sgc_embed_runtime: SOURCES is required")
    endif()
    if(NOT ARG_ROOT)
        set(ARG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    foreach(src IN LISTS ARG_SOURCES)
        if(NOT EXISTS "${src}")
            message(FATAL_ERROR "sgc_embed_runtime: runtime source not found: ${src}")
        endif()
    endforeach()

    set(_combined "")
    set(_rel_list "")
    foreach(src IN LISTS ARG_SOURCES)
        file(READ "${src}" _part)
        string(REPLACE "\r\n" "\n" _part "${_part}")
        string(APPEND _combined "${_part}")

        file(RELATIVE_PATH _rel "${ARG_ROOT}" "${src}")
        if(_rel_list)
            string(APPEND _rel_list ", ")
        endif()
        string(APPEND _rel_list "${_rel}")
    endforeach()

    set(_delimiter ")GALLT_CRT\"")
    string(FIND "${_combined}" "${_delimiter}" _delim_pos)
    if(NOT _delim_pos EQUAL -1)
        message(FATAL_ERROR
            "sgc_embed_runtime: runtime source contains the raw-string delimiter '${_delimiter}'")
    endif()

    string(SHA256 _hash "${_combined}")

    set(_prologue [=[// WARNING: EmbedRuntime.cmake AUTO-GENERATED FILE - DO NOT EDIT BY HAND

#ifndef GALLT_RUNTIME_CRT_EMBEDDED_HPP
#define GALLT_RUNTIME_CRT_EMBEDDED_HPP

namespace gallt {

    inline constexpr const char* kGalltRuntimeCSource = R"GALLT_CRT(
]=])
    string(REPLACE "\r\n" "\n" _prologue "${_prologue}")

    set(_epilogue [=[)GALLT_CRT";

}

#endif
]=])
    string(REPLACE "\r\n" "\n" _epilogue "${_epilogue}")

    set(_new_text_lf "${_prologue}${_combined}${_epilogue}")

    set(_existing "")
    set(_existing_malformed FALSE)
    if(EXISTS "${ARG_OUTPUT}")
        file(READ "${ARG_OUTPUT}" _existing)
        string(REPLACE "\r\n" "\n" _existing "${_existing}")
        file(SHA256 "${ARG_OUTPUT}" _existing_hash)
        string(SHA256 _lf_hash "${_new_text_lf}")
        string(REPLACE "\n" "\r\n" _new_text_crlf "${_new_text_lf}")
        string(SHA256 _crlf_hash "${_new_text_crlf}")
        if(NOT _existing_hash STREQUAL _lf_hash
            AND NOT _existing_hash STREQUAL _crlf_hash)
            set(_existing_malformed TRUE)
        endif()
    endif()

    if(NOT _existing_malformed AND _existing STREQUAL _new_text_lf)
        message(STATUS "runtime already embedded: ${_rel_list} (sha256 ${_hash})")
    else()
        get_filename_component(_out_dir "${ARG_OUTPUT}" DIRECTORY)
        if(NOT EXISTS "${_out_dir}")
            file(MAKE_DIRECTORY "${_out_dir}")
        endif()
        file(WRITE "${ARG_OUTPUT}" "${_new_text_lf}")
        message(STATUS "embedded ${_rel_list} -> crt_embedded.hpp (sha256 ${_hash})")
    endif()
endfunction()
