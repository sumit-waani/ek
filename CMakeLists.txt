cmake_minimum_required(VERSION 3.16)
project(eka VERSION 1.0.0 LANGUAGES C)

# C11 is non-negotiable
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# -------------------------------------------------------------------
# Dependencies (system + vendored)
# -------------------------------------------------------------------

find_package(PkgConfig REQUIRED)

# libuv — async I/O
pkg_check_modules(LIBUV REQUIRED libuv)

# SQLite3
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# PCRE2
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)

# OpenSSL (libcrypto only)
pkg_check_modules(OPENSSL REQUIRED libcrypto)

# libcurl
pkg_check_modules(CURL REQUIRED libcurl)

# cmark — Markdown → HTML
pkg_check_modules(CMARK REQUIRED libcmark)

# -------------------------------------------------------------------
# Vendored: yyjson (header-only style, but single .c)
# -------------------------------------------------------------------
set(YYJSON_DIR "${CMAKE_SOURCE_DIR}/vendor")
add_library(yyjson STATIC "${YYJSON_DIR}/yyjson.c")
target_include_directories(yyjson PUBLIC "${YYJSON_DIR}")
target_compile_options(yyjson PRIVATE -O2)

# -------------------------------------------------------------------
# Eka sources
# -------------------------------------------------------------------
set(EKA_SOURCES
    src/main.c
    src/cli/cli.c
    src/core/value.c
    src/core/obj.c
    src/core/vm.c
    src/lexer/token.c
    src/lexer/lexer.c
    src/parser/ast.c
    src/parser/parser.c
    src/compiler/symtab.c
    src/compiler/compiler.c
    src/runtime/http.c
    src/runtime/server.c
    src/builtins/builtins.c
    src/client/client_runtime.c
    src/fmt/fmt.c
)

# -------------------------------------------------------------------
# Eka binary
# -------------------------------------------------------------------
add_executable(eka ${EKA_SOURCES})

target_include_directories(eka PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${LIBUV_INCLUDE_DIRS}"
    "${SQLITE3_INCLUDE_DIRS}"
    "${PCRE2_INCLUDE_DIRS}"
    "${OPENSSL_INCLUDE_DIRS}"
    "${CURL_INCLUDE_DIRS}"
    "${CMARK_INCLUDE_DIRS}"
)

target_link_libraries(eka PRIVATE
    yyjson
    ${LIBUV_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${PCRE2_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${CURL_LIBRARIES}
    ${CMARK_LIBRARIES}
    pthread
    m
    dl
)

# -------------------------------------------------------------------
# Compiler flags
# -------------------------------------------------------------------
target_compile_options(eka PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# Dev vs release
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(eka PRIVATE -g -O0 -fsanitize=address)
    target_link_options(eka PRIVATE -fsanitize=address)
else()
    target_compile_options(eka PRIVATE -O2 -DNDEBUG)
endif()

# -------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
