cmake_minimum_required(VERSION 3.20)

project(GauntletRecompiled LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU RT64_SDL_WINDOW_VULKAN)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)
# RT64 builds with -Werror; newer clang warns in its vendored ImGui.
target_compile_options(rt64 PRIVATE -Wno-error)

# The runtime's bundled fmt 10.2 uses FMT_STRING in non-constant contexts that newer
# clang rejects under consteval; fmt turns consteval off when built as C++17.
set_target_properties(fmt PROPERTIES CXX_STANDARD 17)

set(RUNTIME_INCLUDES
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

# Recompiler output (regenerate with tools/elf2syms.py + N64Recomp).
add_library(RecompiledFuncs STATIC)
file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})
target_include_directories(RecompiledFuncs PRIVATE ${RUNTIME_INCLUDES})
target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

add_executable(GauntletRecompiled
    src/main.cpp
    src/rt64_context.cpp
    src/tlb.cpp
    src/boot.cpp
    src/game_funcs.c
)
target_include_directories(GauntletRecompiled PRIVATE
    ${RUNTIME_INCLUDES}
    ${SDL2_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_BINARY_DIR}/rt64/src
)
set_source_files_properties(src/game_funcs.c PROPERTIES COMPILE_FLAGS -fno-strict-aliasing)
target_compile_options(GauntletRecompiled PRIVATE -march=nehalem)
target_link_libraries(GauntletRecompiled PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    SDL2::SDL2
    Threads::Threads
    ${CMAKE_DL_LIBS}
    -latomic
)
