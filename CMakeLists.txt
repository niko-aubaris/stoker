cmake_minimum_required(VERSION 3.22)
# C is only for the vendored tweetnacl.c (Ed25519 release-signature checks)
project(stoker C CXX)

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

if(MINGW)
  enable_language(RC)
  add_executable(stoker bpos-dash.cpp tweetnacl.c stoker.rc)
else()
  add_executable(stoker bpos-dash.cpp tweetnacl.c)
endif()
target_link_libraries(stoker PRIVATE ftxui::component ftxui::dom ftxui::screen)
if(MINGW)
  # ws2_32: SSO loopback listener; shell32: browser open; winhttp: native transport
  target_link_libraries(stoker PRIVATE ws2_32 shell32 winhttp)
endif()

# STOKER_STATIC=ON makes a self-contained binary (default for the Windows
# cross build so the .exe needs no mingw DLLs next to it).
if(MINGW)
  set(STOKER_STATIC ON)
endif()
if(STOKER_STATIC)
  target_link_options(stoker PRIVATE -static)
endif()

# --- stoker-gui: the full Dear ImGui GUI (no terminal) --------------------------
option(STOKER_GUI "Build the windowed stoker-gui as well" ON)
if(STOKER_GUI)
  if(MINGW)
    FetchContent_Declare(glfw
      URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
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
      FetchContent_Declare(glfw
        URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
      set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
      set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
      FetchContent_MakeAvailable(glfw)
      set(STOKER_GLFW glfw)
    endif()
  endif()

  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.9.tar.gz)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui_core PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_core PUBLIC ${STOKER_GLFW})

  if(MINGW)
    enable_language(RC)
    add_executable(stoker-gui stoker-imgui.cpp tweetnacl.c stoker.rc)
  else()
    add_executable(stoker-gui stoker-imgui.cpp tweetnacl.c)
  endif()
  target_link_libraries(stoker-gui PRIVATE
    ftxui::component ftxui::dom ftxui::screen imgui_core ${STOKER_GLFW})
  if(MINGW)
    target_link_libraries(stoker-gui PRIVATE opengl32 gdi32 ws2_32 shell32 winhttp)
    target_link_options(stoker-gui PRIVATE -static -mwindows)
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(stoker-gui PRIVATE OpenGL::GL)
  endif()
endif()
