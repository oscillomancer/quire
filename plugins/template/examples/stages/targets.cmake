# The product's own targets, after the framework's (framework/cmake/Framework.cmake).

# A host in process: the catalog through the CLAP factory, the layer's
# extension and state chunk, and redefinition when a plugin's file changes.
if (SUPPLE_BUILD_PLUGIN AND COMMAND supple_test)
    supple_test(stages-plugin-tests SOURCES ${SUPPLE_PRODUCT}/test/plugin_test.cpp LINK product-impl)
endif()
