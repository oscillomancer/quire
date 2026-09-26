supple_add_library(command-bus SOURCES src/bus.cpp src/jobs.cpp DEPS supple::core-schema supple::product-info)
supple_add_tests(command-bus SOURCES tests/test_bus.cpp)
