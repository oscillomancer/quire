# The agent panel's core (gui.md §5): LLM endpoints over HTTP streaming, the
# tool loop through the MCP surface, the branch review workflow. No GL.
find_package(CURL REQUIRED)
supple_add_library(agent SOURCES src/llm.cpp src/session.cpp src/config.cpp
  DEPS supple::command-bus supple::core-json supple::core-base supple::mcp
  PRIVATE_DEPS CURL::libcurl)
if(SUPPLE_BUILD_TESTS)
  supple_add_tests(agent SOURCES tests/test_agent.cpp DEPS supple::engine LABELS gui)
endif()
