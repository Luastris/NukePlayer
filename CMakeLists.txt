cmake_minimum_required(VERSION 3.20)
project(NukePlayer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")   # VCPKG_ROOT env var, not a hardcoded path

add_executable(NukePlayer src/main.cpp player.rc)   # player.rc embeds icon.ico (swap for release)

# Consumer of the engine DLL (NUKEENGINE_API = dllimport). No editor, no ImGui.
target_compile_definitions(NukePlayer PRIVATE
    WIN32 _CONSOLE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    GLM_ENABLE_EXPERIMENTAL
    BOOST_ALL_DYN_LINK
    BOOST_ALL_NO_LIB   # disable boost auto-link (it mis-picked release); we link explicitly below
)

target_include_directories(NukePlayer PRIVATE
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/include
    ${NUKE_ROOT}/NukeEngine/deps
    ${VCPKG_INSTALLED}/include   # glm now comes from vcpkg (deps/glm removed)
)

target_link_directories(NukePlayer PRIVATE
    ${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>   # NukeEngine.lib (import lib)
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib        # boost
)

target_link_libraries(NukePlayer PRIVATE
    NukeEngine
    # Explicit boost (auto-link disabled). Debug build -> debug (-gd-) variant, matching /MDd
    # and the boost_filesystem-...-gd-...dll the editor already deploys next to the engine.
    $<$<CONFIG:Debug>:${VCPKG_INSTALLED}/debug/lib/boost_filesystem-vc143-mt-gd-x64-1_91.lib>
    $<$<NOT:$<CONFIG:Debug>>:${VCPKG_INSTALLED}/lib/boost_filesystem-vc143-mt-x64-1_91.lib>
)

# Deploy next to the engine DLL + modules/ + config + scripts, so it finds them at runtime.
add_custom_command(TARGET NukePlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukePlayer>" "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${NUKE_ROOT}/NukeEngine/shaders" "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/shaders"
    COMMENT "Deploying NukePlayer.exe + built-in shaders next to the engine"
)
