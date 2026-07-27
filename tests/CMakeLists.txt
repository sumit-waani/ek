add_executable(test_value test_value.c)
target_include_directories(test_value PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_value PRIVATE -g -O0)
target_link_libraries(test_value PRIVATE m)
add_test(NAME value COMMAND test_value)

add_executable(test_obj test_obj.c "${CMAKE_SOURCE_DIR}/src/core/obj.c" "${CMAKE_SOURCE_DIR}/src/core/vm.c" "${CMAKE_SOURCE_DIR}/src/core/value.c")
target_include_directories(test_obj PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_obj PRIVATE -g -O0)
target_link_libraries(test_obj PRIVATE m)
add_test(NAME obj COMMAND test_obj)

add_executable(test_vm test_vm.c "${CMAKE_SOURCE_DIR}/src/core/obj.c" "${CMAKE_SOURCE_DIR}/src/core/vm.c" "${CMAKE_SOURCE_DIR}/src/core/value.c")
target_include_directories(test_vm PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_vm PRIVATE -g -O0)
target_link_libraries(test_vm PRIVATE m)
add_test(NAME vm COMMAND test_vm)

add_executable(test_lexer test_lexer.c "${CMAKE_SOURCE_DIR}/src/lexer/token.c" "${CMAKE_SOURCE_DIR}/src/lexer/lexer.c")
target_include_directories(test_lexer PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_lexer PRIVATE -g -O0)
target_link_libraries(test_lexer PRIVATE m)
add_test(NAME lexer COMMAND test_lexer)

add_executable(test_parser test_parser.c
    "${CMAKE_SOURCE_DIR}/src/lexer/token.c"
    "${CMAKE_SOURCE_DIR}/src/lexer/lexer.c"
    "${CMAKE_SOURCE_DIR}/src/parser/ast.c"
    "${CMAKE_SOURCE_DIR}/src/parser/parser.c"
    "${CMAKE_SOURCE_DIR}/src/core/obj.c"
    "${CMAKE_SOURCE_DIR}/src/core/vm.c"
    "${CMAKE_SOURCE_DIR}/src/core/value.c")
target_include_directories(test_parser PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_parser PRIVATE -g -O0)
target_link_libraries(test_parser PRIVATE m)
add_test(NAME parser COMMAND test_parser)

add_executable(test_compiler test_compiler.c
    "${CMAKE_SOURCE_DIR}/src/lexer/token.c"
    "${CMAKE_SOURCE_DIR}/src/lexer/lexer.c"
    "${CMAKE_SOURCE_DIR}/src/parser/ast.c"
    "${CMAKE_SOURCE_DIR}/src/parser/parser.c"
    "${CMAKE_SOURCE_DIR}/src/compiler/symtab.c"
    "${CMAKE_SOURCE_DIR}/src/compiler/compiler.c"
    "${CMAKE_SOURCE_DIR}/src/core/obj.c"
    "${CMAKE_SOURCE_DIR}/src/core/vm.c"
    "${CMAKE_SOURCE_DIR}/src/core/value.c")
target_include_directories(test_compiler PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_compiler PRIVATE -g -O0)
target_link_libraries(test_compiler PRIVATE m)
add_test(NAME compiler COMMAND test_compiler)

add_executable(test_fmt test_fmt.c
    "${CMAKE_SOURCE_DIR}/src/fmt/fmt.c")
target_include_directories(test_fmt PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_fmt PRIVATE -g -O0)
target_link_libraries(test_fmt PRIVATE m)
add_test(NAME fmt COMMAND test_fmt)

# Builtins test — links all the real dependencies
add_executable(test_builtins test_builtins.c
    "${CMAKE_SOURCE_DIR}/src/core/obj.c"
    "${CMAKE_SOURCE_DIR}/src/core/vm.c"
    "${CMAKE_SOURCE_DIR}/src/core/value.c"
    "${CMAKE_SOURCE_DIR}/src/builtins/builtins.c"
    "${CMAKE_SOURCE_DIR}/src/runtime/http.c")
target_include_directories(test_builtins PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(test_builtins PRIVATE -g -O0)
target_link_libraries(test_builtins PRIVATE
    yyjson
    ${SQLITE3_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${CMARK_LIBRARIES}
    ${CURL_LIBRARIES}
    ${PCRE2_LIBRARIES}
    ${LIBUV_LIBRARIES}
    m
    pthread)
add_test(NAME builtins COMMAND test_builtins)
