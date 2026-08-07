cmake_minimum_required(VERSION 3.20)
project(NukePlayer CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine/cmake/NukeRunDir.cmake")   # NUKE_RUN_SUBDIR: x64 / macos / linux

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NUKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(WIN32)
    # Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

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
        ${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>   # NukeEngine.lib (import lib)
        ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib        # boost
    )

    target_link_libraries(NukePlayer PRIVATE
        NukeEngine
        # Explicit boost (auto-link disabled). Debug build -> debug (-gd-) variant, matching /MDd
        # and the boost_filesystem-...-gd-...dll the editor already deploys next to the engine.
        $<$<CONFIG:Debug>:${VCPKG_INSTALLED}/debug/lib/boost_filesystem-vc143-mt-gd-x64-1_91.lib>
        $<$<NOT:$<CONFIG:Debug>>:${VCPKG_INSTALLED}/lib/boost_filesystem-vc143-mt-x64-1_91.lib>
    )
else()
    # Deuterium: the engine core builds in the same superbuild as the NukeEngine CMake
    # target — link it directly; vcpkg deps come through the toolchain's find_package.
    add_executable(NukePlayer src/main.cpp)   # .rc is Windows resource compiler only

    target_compile_definitions(NukePlayer PRIVATE
        _USE_MATH_DEFINES
        GLM_ENABLE_EXPERIMENTAL)

    target_include_directories(NukePlayer PRIVATE
        ${NUKE_ROOT}/NukeEngine
        ${NUKE_ROOT}/NukeEngine/deps)

    find_package(Boost REQUIRED COMPONENTS filesystem system)
    find_package(glm CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)

    # NukeEngine's PUBLIC include dir (engine headers) rides in on the target link.
    target_link_libraries(NukePlayer PRIVATE
        NukeEngine
        Boost::filesystem Boost::system
        glm::glm
        nlohmann_json::nlohmann_json)

    if(NOT MSVC)
        target_compile_options(NukePlayer PRIVATE -Wno-unknown-attributes)
    endif()

    # The run dir carries libNukeEngine.dylib/.so as a sibling of the exe.
    set_target_properties(NukePlayer PROPERTIES
        BUILD_RPATH "@loader_path;$ORIGIN")
endif()

# Deploy next to the engine DLL + modules/ + config + scripts, so it finds them at runtime.
add_custom_command(TARGET NukePlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukePlayer>" "${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${NUKE_ROOT}/NukeEngine/shaders" "${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>/shaders"
    COMMENT "Deploying NukePlayer + built-in shaders next to the engine"
)
