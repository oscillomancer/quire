# Verify that the built AU is one a host will actually see.
#
# macOS discovers Audio Units by reading the AudioComponents array out of the
# bundle's Info.plist and nothing else. clap-wrapper generates that array, but
# an incremental build -- a reconfigure in particular -- can leave CMake's own
# generic plist in its place. The result is a component that is present,
# correctly signed and loads under dlopen, yet is invisible to every host
# including auval, which reports only that it cannot find the component. That
# looks exactly like "the AU did not build" when the binary is right there,
# which is a slow thing to work out from first principles.
#
# Invoked with -DBUNDLE=<path/to/Foo.component>, both as a test and from the
# install target.

if (NOT DEFINED BUNDLE OR NOT EXISTS "${BUNDLE}/Contents/Info.plist")
    message(FATAL_ERROR "AUv2 bundle not found: ${BUNDLE}")
endif()

file(READ "${BUNDLE}/Contents/Info.plist" plist)
string(FIND "${plist}" "<key>AudioComponents</key>" components)
if (components EQUAL -1)
    get_filename_component(bundle_dir "${BUNDLE}" DIRECTORY)
    message(FATAL_ERROR
        "\n"
        "The AU bundle's Info.plist is CMake's generic one, with no\n"
        "AudioComponents entry, so no host will see it -- including auval,\n"
        "which will report that it cannot find the component.\n"
        "\n"
        "This happens when the bundle is regenerated incrementally. Rebuild\n"
        "the assets from clean:\n"
        "\n"
        "    rm -rf ${bundle_dir}\n"
        "    cmake --build <build> --target product-install\n")
endif()

execute_process(
    COMMAND /usr/bin/codesign --verify --deep --strict "${BUNDLE}"
    RESULT_VARIABLE signature_result
    ERROR_VARIABLE signature_error)
if (NOT signature_result EQUAL 0)
    message(FATAL_ERROR "AUv2 bundle signature is invalid: ${signature_error}")
endif()
