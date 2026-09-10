cmake_minimum_required(VERSION 3.24)
project(VBufferAnimatedSail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.8
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_dx12 STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui_dx12 PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_dx12 PRIVATE UNICODE _UNICODE)
target_link_libraries(imgui_dx12 PUBLIC d3d12 dxgi)

add_executable(VBufferAnimatedSail WIN32
  src/main.cpp
  src/renderer.cpp
  src/renderer.h
  src/sail_mesh.cpp
  src/sail_mesh.h
  src/shaders.h
)
target_compile_definitions(VBufferAnimatedSail PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(VBufferAnimatedSail PRIVATE imgui_dx12 d3d12 dxgi d3dcompiler)
if(MSVC)
  target_compile_options(VBufferAnimatedSail PRIVATE /W4 /permissive- /EHsc)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT VBufferAnimatedSail)
endif()

