cmake_minimum_required(VERSION 3.16)
project(SGC LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SGC_ROOT          "${CMAKE_CURRENT_SOURCE_DIR}")
set(SGC_INCLUDE_DIR   "${SGC_ROOT}/include")
set(SGC_RUNTIME_DIR   "${SGC_INCLUDE_DIR}/gallt/runtime")
set(SGC_EMBEDDED_HDR  "${SGC_RUNTIME_DIR}/crt_embedded.hpp")

set(SGC_RUNTIME_SOURCES
    "${SGC_RUNTIME_DIR}/crt_core.c"
    "${SGC_RUNTIME_DIR}/crt_file.c"
    "${SGC_RUNTIME_DIR}/crt_string.c"
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SGC_RUNTIME_SOURCES})

include("${SGC_ROOT}/cmake/EmbedRuntime.cmake")
sgc_embed_runtime(
    ROOT    "${SGC_ROOT}"
    SOURCES ${SGC_RUNTIME_SOURCES}
    OUTPUT  "${SGC_EMBEDDED_HDR}"
)

set(SGC_SOURCES
    SGC.cpp
    include/gallt/codegen/codegen.cpp
    include/gallt/codegen/codegen_constant.cpp
    include/gallt/codegen/codegen_aggregate.cpp
    include/gallt/codegen/codegen_expression.cpp
    include/gallt/codegen/codegen_builtin.cpp
    include/gallt/codegen/codegen_lifetime.cpp
    include/gallt/codegen/codegen_variadic.cpp
    include/gallt/common/diagnostics.cpp
    include/gallt/common/token.cpp
    include/gallt/driver/command_line.cpp
    include/gallt/driver/compiler.cpp
    include/gallt/lexer/lexer.cpp
    include/gallt/pal/platform_paths.cpp
    include/gallt/pal/platform_windows.cpp
    include/gallt/pal/platform_linux.cpp
    include/gallt/pal/entry_windows.cpp
    include/gallt/pal/entry_linux.cpp
    include/gallt/parser/ast.cpp
    include/gallt/parser/parser.cpp
    include/gallt/parser/parser_expression.cpp
    include/gallt/parser/parser_declaration.cpp
    include/gallt/parser/parser_generic.cpp
    include/gallt/parser/parser_statement.cpp
    include/gallt/semantic/generic_expander.cpp
    include/gallt/semantic/generic_expander_pattern.cpp
    include/gallt/semantic/generic_expander_clone.cpp
    include/gallt/semantic/generic_expander_property.cpp
    include/gallt/semantic/generic_expander_pack.cpp
    include/gallt/semantic/generic_expander_constexpr.cpp
    include/gallt/semantic/condition_compiler.cpp
    include/gallt/semantic/constant_folding.cpp
    include/gallt/semantic/constexpr_function.cpp
    include/gallt/semantic/lifecycle.cpp
    include/gallt/semantic/lifecycle_rewrite.cpp
    include/gallt/semantic/namespace_lowering.cpp
    include/gallt/semantic/type_checker.cpp
    include/gallt/semantic/type_checker_expression.cpp
    include/gallt/semantic/type_checker_overload.cpp
    include/gallt/semantic/type_checker_declaration.cpp
    include/gallt/semantic/type_checker_control.cpp
    include/gallt/semantic/type_checker_constexpr.cpp
    include/gallt/semantic/type_checker_layout.cpp
    include/gallt/semantic/type_checker_variadic.cpp
)

add_executable(sgc ${SGC_SOURCES})
target_include_directories(sgc PRIVATE "${SGC_INCLUDE_DIR}")

if(WIN32)
    set(_sgc_platform "windows")
elseif(UNIX AND NOT APPLE)
    set(_sgc_platform "linux")
elseif(APPLE)
    set(_sgc_platform "macos")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _sgc_platform)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    set(_sgc_compiler "clang")
elseif(MSVC)
    set(_sgc_compiler "msvc")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_sgc_compiler "gcc")
else()
    string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _sgc_compiler)
endif()

set(SGC_OUTPUT_NAME "sgc_${_sgc_platform}_${_sgc_compiler}")
set_target_properties(sgc PROPERTIES OUTPUT_NAME "${SGC_OUTPUT_NAME}")
message(STATUS
    "sgc output name: ${SGC_OUTPUT_NAME} "
    "(compiler: ${CMAKE_CXX_COMPILER_ID}, simulate: ${CMAKE_CXX_SIMULATE_ID})")

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"
            AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(sgc PRIVATE /utf-8 /EHsc /FS)
    if(WIN32)
        target_link_options(sgc PRIVATE
            /STACK:536870912
            /ENTRY:wmainCRTStartup
            /SUBSYSTEM:CONSOLE
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(sgc PRIVATE dl)
endif()
