cmake_minimum_required(VERSION 3.10)
project(deepin-package-file-index VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

option(BUILD_TESTS  "Build tests" OFF)
option(BUILD_CLI    "Build index builder CLI" ON)
option(BUILD_DAEMON "Build D-Bus daemon" ON)

add_subdirectory(src/lib)

if(BUILD_DAEMON)
    add_subdirectory(src/daemon)
endif()

if(BUILD_CLI)
    add_subdirectory(src/build)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install data files
install(FILES
    data/com.deepin.pkgfileindex.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system-services
)
install(FILES
    data/com.deepin.pkgfileindex.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d
)
install(FILES
    data/deepin-pkgfile-indexd.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system
)
