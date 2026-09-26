find_package(SDL3 REQUIRED CONFIG)
find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

add_library(gui-spike-nanovg STATIC
  ${PROJECT_SOURCE_DIR}/third_party/nanovg/src/nanovg.c
  nanovg_gl3.c)
target_include_directories(gui-spike-nanovg SYSTEM PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/nanovg/src)
target_link_libraries(gui-spike-nanovg PUBLIC OpenGL::GL)

add_executable(gui-shell-spike main.cpp)
target_include_directories(gui-shell-spike SYSTEM PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/clap/include)
target_link_libraries(gui-shell-spike PRIVATE SDL3::SDL3 X11::X11 OpenGL::GL
  Threads::Threads gui-spike-nanovg ${CMAKE_DL_LIBS} supple::product-info)
supple_target_defaults(gui-shell-spike)
