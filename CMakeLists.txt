cmake_minimum_required(VERSION 3.22)
project(stoker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

add_executable(stoker bpos-dash.cpp)
target_link_libraries(stoker PRIVATE ftxui::component ftxui::dom ftxui::screen)
if(MINGW)
  # ws2_32: SSO loopback callback listener; shell32: ShellExecute (browser open)
  target_link_libraries(stoker PRIVATE ws2_32 shell32)
endif()

# STOKER_STATIC=ON makes a self-contained binary (default for the Windows
# cross build so the .exe needs no mingw DLLs next to it).
if(MINGW)
  set(STOKER_STATIC ON)
endif()
if(STOKER_STATIC)
  target_link_options(stoker PRIVATE -static)
endif()

# --- stoker-gui: the same app in a native window (no terminal) -----------------
# The FTXUI component renders into an off-screen screen drawn with OpenGL;
# see gui_host.hpp. STOKER_GUI=OFF skips it (e.g. headless build machines).
option(STOKER_GUI "Build the windowed stoker-gui as well" ON)
if(STOKER_GUI)
  if(MINGW)
    # cross build: fetch and statically link GLFW (win32 backend, no deps)
    FetchContent_Declare(glfw
      URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    )
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
    set(STOKER_GLFW glfw)
  else()
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
      set(STOKER_GLFW glfw)
    else()
      message(STATUS "system glfw3 not found; fetching")
      FetchContent_Declare(glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
      )
      set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
      set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
      FetchContent_MakeAvailable(glfw)
      set(STOKER_GLFW glfw)
    endif()
  endif()

  add_executable(stoker-gui bpos-dash.cpp)
  target_compile_definitions(stoker-gui PRIVATE STOKER_GUI)
  target_link_libraries(stoker-gui PRIVATE ftxui::component ftxui::dom ftxui::screen ${STOKER_GLFW})
  if(MINGW)
    # -mwindows: real windowed app, no console box behind it
    target_link_libraries(stoker-gui PRIVATE opengl32 gdi32 ws2_32 shell32)
    target_link_options(stoker-gui PRIVATE -static -mwindows)
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(stoker-gui PRIVATE OpenGL::GL)
  endif()
endif()
