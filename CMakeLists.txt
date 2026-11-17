cmake_minimum_required(VERSION 3.16)
project(OmniGuard VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable automoc, autouic, autorcc for Qt6
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Find FFmpeg libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include ${FFMPEG_INCLUDE_DIRS})

# Source files
set(SOURCES
    src/main.cpp
    src/StreamWorker.cpp
    src/MainWindow.cpp
    src/CameraConfig.cpp
)

set(HEADERS
    include/StreamWorker.h
    include/MainWindow.h
    include/CameraConfig.h
    include/Common.h
)

# Resources
set(RESOURCES resources/resources.qrc)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${RESOURCES})

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${FFMPEG_LIBRARIES}
)

# Compile options for robustness
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
    -Werror=return-type
    -Wno-unused-parameter
    -fPIC
)

# ============================================================================
# Installazione (percorsi standard Linux: compare nel menu applicazioni)
# ============================================================================
include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS packaging/omniguard-watchdog DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/omniguard.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/omniguard.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES cameras.json.example README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ============================================================================
# Pacchetto .deb (Debian / Ubuntu / Mint):  cd build && cpack
# ============================================================================
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "omniguard")
set(CPACK_PACKAGE_VENDOR "Pygar04")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Videosorveglianza per 3 telecamere IP RTSP (Zosi)")
set(CPACK_PACKAGE_DESCRIPTION "Applicazione desktop Qt6/FFmpeg per il monitoraggio 24/7 di 3 telecamere IP via RTSP, con riconnessione automatica.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Pygar04/OmniGuard")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Pygar04 <101059425+Pygar04@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
# Le librerie collegate (Qt6, FFmpeg) vengono rilevate automaticamente con dpkg-shlibdeps;
# i plugin Qt caricati a runtime vanno dichiarati a mano.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, libqt6svg6")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland")

include(CPack)