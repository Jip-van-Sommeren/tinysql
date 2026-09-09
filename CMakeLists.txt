cmake_minimum_required(VERSION 3.20)

project(db VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

function(db_configure_target target_name usage_requirement)
    target_compile_features(${target_name} ${usage_requirement} cxx_std_23)
    target_include_directories(${target_name} ${usage_requirement}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()
endfunction()

add_library(db_sql
    src/db_sql_lexer.cpp
    src/db_sql_parser.cpp
)
db_configure_target(db_sql PUBLIC)

add_library(db_core
    src/db_buffer_manager.cpp
    src/db_database.cpp
    src/db_decimal.cpp
    src/db_expression_evaluator.cpp
    src/db_page_factory.cpp
    src/db_query_executor.cpp
    src/db_query_validator.cpp
    src/db_read.cpp
    src/db_row_validator.cpp
    src/db_storage_engine.cpp
    src/db_table.cpp
    src/db_write.cpp
)
db_configure_target(db_core PUBLIC)
target_link_libraries(db_core PUBLIC db_sql)

add_executable(db src/main.cpp)
db_configure_target(db PRIVATE)
target_link_libraries(db PRIVATE db_core)

add_executable(db_sql_parser_demo
    src/db_sql_lexer_parser.cpp
)
db_configure_target(db_sql_parser_demo PRIVATE)
target_link_libraries(db_sql_parser_demo PRIVATE db_sql)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME db_sql_parser_demo COMMAND db_sql_parser_demo)

    add_executable(db_decimal_test tests/db_decimal_test.cpp)
    db_configure_target(db_decimal_test PRIVATE)
    target_link_libraries(db_decimal_test PRIVATE db_core)
    add_test(NAME db_decimal_test COMMAND db_decimal_test)

    add_executable(db_select_executor_test tests/db_select_executor_test.cpp)
    db_configure_target(db_select_executor_test PRIVATE)
    target_link_libraries(db_select_executor_test PRIVATE db_core)
    add_test(NAME db_select_executor_test COMMAND db_select_executor_test)
endif()
