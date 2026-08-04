find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_executable(deepin-pkgfile-indexd main.c)
target_link_libraries(deepin-pkgfile-indexd PRIVATE
    deepin-pkgfile-index
    ${DBUS_LIBRARIES}
)

target_include_directories(deepin-pkgfile-indexd PRIVATE
    ${DBUS_INCLUDE_DIRS}
)

install(TARGETS deepin-pkgfile-indexd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
