add_executable(deepin-pkgfile-index-build main.cpp dpkg_parser.cpp desktop_parser.cpp common.cpp)

target_include_directories(deepin-pkgfile-index-build PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SQLite3 REQUIRED)
target_link_libraries(deepin-pkgfile-index-build PRIVATE SQLite::SQLite3)

install(TARGETS deepin-pkgfile-index-build
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
