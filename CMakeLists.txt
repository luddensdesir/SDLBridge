cmake_minimum_required(VERSION 3.16)
project(SDLBridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

# ── Build SDL2 from source ──────────────────────────────────────────────────
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL-SDL2 EXCLUDE_FROM_ALL)

# ── Build libwebsockets from source ─────────────────────────────────────────
set(LWS_WITH_SSL           OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TESTAPPS   ON  CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TEST_CLIENT ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED        OFF CACHE BOOL "" FORCE)
set(LWS_WITH_STATIC        ON  CACHE BOOL "" FORCE)
set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(libwebsockets-main EXCLUDE_FROM_ALL)

# ── SDLBridge executable ────────────────────────────────────────────────────
add_executable(sdlbridge main.cpp)

target_include_directories(sdlbridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL-SDL2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libwebsockets-main/include
    ${CMAKE_CURRENT_BINARY_DIR}/libwebsockets-main   # for generated lws_config.h
)

target_link_libraries(sdlbridge PRIVATE
    SDL2-static
    SDL2main
    websockets
)
