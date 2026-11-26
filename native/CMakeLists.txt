cmake_minimum_required(VERSION 3.20)
project(NogginKnockers2Native LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(nk_core STATIC
    core/nk_anim.c
    core/nk_anim_data.c
    core/nk_aspect.c
    core/nk_cinema.c
    core/nk_cinema_data.c
    core/nk_collision.c
    core/nk_compositor.c
    core/nk_dialogue.c
    core/nk_dialogue_data.c
    core/nk_effects.c
    core/nk_fade.c
    core/nk_flow.c
    core/nk_font.c
    core/nk_font_data.c
    core/nk_game.c
    core/nk_match_present.c
    core/nk_math.c
    core/nk_port_credit.c
    core/nk_scene.c
    core/nk_scene_data.c
    core/nk_select.c
    core/nk_sound_logic.c
    core/nk_sticky.c
    core/nk_ui.c
)
target_include_directories(nk_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core")

# The official MinGW development archive provides headers, import libraries,
# and the runtime DLL in one prefix. Point SDL2_MINGW_ROOT at that prefix when
# cross-compiling from macOS or Linux.
set(SDL2_MINGW_ROOT "" CACHE PATH "Prefix of the official SDL2 MinGW development package")
if(WIN32 AND SDL2_MINGW_ROOT)
    add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_IMPLIB "${SDL2_MINGW_ROOT}/lib/libSDL2.dll.a"
        IMPORTED_LOCATION "${SDL2_MINGW_ROOT}/bin/SDL2.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_MINGW_ROOT}/include/SDL2")
    add_library(SDL2::SDL2main STATIC IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2main PROPERTIES
        IMPORTED_LOCATION "${SDL2_MINGW_ROOT}/lib/libSDL2main.a")
else()
    find_package(SDL2 CONFIG REQUIRED)
endif()

# The SDL frontend lives in src/. This condition allows the core to be built
# on its own while the frontend is being assembled.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
    file(GLOB NK_FRONTEND_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
    add_executable(noggin2 ${NK_FRONTEND_SOURCES})
    target_include_directories(noggin2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(noggin2 PRIVATE nk_core)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(noggin2 PRIVATE SDL2::SDL2main)
    endif()
    target_link_libraries(noggin2 PRIVATE SDL2::SDL2)
    if(WIN32 AND SDL2_MINGW_ROOT)
        add_custom_command(TARGET noggin2 POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SDL2_MINGW_ROOT}/bin/SDL2.dll"
                "$<TARGET_FILE_DIR:noggin2>/SDL2.dll")
    endif()
endif()
