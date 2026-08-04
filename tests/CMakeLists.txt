add_executable(test_query test_query.cpp)

target_include_directories(test_query PRIVATE ${CMAKE_SOURCE_DIR}/src/lib)

find_package(SQLite3 REQUIRED)
target_link_libraries(test_query PRIVATE deepin-pkgfile-index SQLite::SQLite3)

add_test(NAME test_query COMMAND test_query)
