add_subdirectory(agent-panel)  # no GL: the core builds and tests without the GUI
if(SUPPLE_BUILD_GUI)
  add_subdirectory(gui)
endif()
