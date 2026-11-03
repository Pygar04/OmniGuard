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

# Install target (optional)
install(TARGETS ${PROJECT_NAME} DESTINATION bin)