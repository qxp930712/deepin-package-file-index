add_library(deepin-pkgfile-index STATIC pkgfile_index.c)

target_include_directories(deepin-pkgfile-index PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(SQLite3 REQUIRED)
target_link_libraries(deepin-pkgfile-index PUBLIC SQLite::SQLite3)

find_package(Threads REQUIRED)
target_link_libraries(deepin-pkgfile-index PUBLIC Threads::Threads)

if(NOT APPLE)
    target_link_libraries(deepin-pkgfile-index PUBLIC dl)
endif()
