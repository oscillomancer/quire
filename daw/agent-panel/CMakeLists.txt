# The agent panel's core (gui.md §5): LLM endpoints over HTTP streaming, the
# tool loop through the MCP surface, the branch review workflow. No GL.
find_package(CURL REQUIRED)
if(WIN32)
  set(agent_config_platform src/config_windows.cpp)
else()
  set(agent_config_platform src/config_posix.cpp)
endif()
supple_add_library(agent SOURCES ${agent_config_platform} src/llm.cpp src/session.cpp src/config.cpp
  DEPS supple::command-bus supple::core-json supple::core-base supple::mcp
  PRIVATE_DEPS CURL::libcurl)
if(WIN32)
  target_link_libraries(agent PRIVATE advapi32)
endif()
if(SUPPLE_BUILD_TESTS)
  supple_add_tests(agent SOURCES tests/test_agent.cpp DEPS supple::engine LABELS gui)
  target_include_directories(agent.tests PRIVATE ${PROJECT_SOURCE_DIR}/daw)
  if(WIN32)
    target_link_libraries(agent.tests PRIVATE ws2_32 advapi32)
  endif()
endif()
