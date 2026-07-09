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
