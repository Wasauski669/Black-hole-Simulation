cmake_minimum_required(VERSION 3.16)
project(BlackHoleSimulation VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(BHS_BUILD_TESTS "Build the physics tests" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# GLFW: use the system package if available, otherwise download it.
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    message(STATUS "GLFW not found on the system - downloading it")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glfw)
endif()

# Dear ImGui (no CMake support upstream, so we build it ourselves).
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# stb_image_write (header only) for screenshots.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5)
FetchContent_MakeAvailable(stb)

# glad OpenGL 3.3 core loader (generated, checked into third_party/).
add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

# ---------------------------------------------------------------------------
# Physics library (no graphics dependencies, shared by the app and the tests)
# ---------------------------------------------------------------------------

add_library(bhs_physics STATIC src/physics.cpp)
target_include_directories(bhs_physics PUBLIC src)

if(MSVC)
    target_compile_options(bhs_physics PRIVATE /W4)
else()
    target_compile_options(bhs_physics PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

add_executable(blackhole
    src/main.cpp
    src/app.cpp
    src/renderer.cpp)
target_include_directories(blackhole SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(blackhole PRIVATE bhs_physics glad glfw imgui)

if(MSVC)
    target_compile_options(blackhole PRIVATE /W4)
else()
    target_compile_options(blackhole PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BHS_BUILD_TESTS)
    enable_testing()
    add_executable(physics_tests tests/test_physics.cpp)
    target_link_libraries(physics_tests PRIVATE bhs_physics)
    add_test(NAME physics_tests COMMAND physics_tests)
endif()
