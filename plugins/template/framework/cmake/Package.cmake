# Release packaging, run by the product-package target (cmake -P): the
# formats a build made, signed, with the license, the notices and how to
# install them, as
#
#   <Name>-<version>-<platform>.zip      macOS, Windows (.tar.gz on Linux)
#   <Name>-<version>-<platform>.pkg      macOS: an installer, one choice per format
#   <Name>-<version>-<platform>-setup.exe  Windows: an installer (Inno Setup 6.3+)
#
# in OUT. Signing is optional and comes from the configure (SUPPLE_SIGN_*
# cache variables) or, if that left them empty, the same names in the
# environment, for CI secrets:
#
#   SUPPLE_SIGN_IDENTITY       macOS: "Developer ID Application: ..." for the
#                              bundles (hardened runtime, timestamped);
#                              without it they are signed ad hoc
#   SUPPLE_INSTALLER_IDENTITY  macOS: "Developer ID Installer: ..." for the .pkg
#   SUPPLE_NOTARY_PROFILE      macOS: a notarytool keychain profile
#                              (xcrun notarytool store-credentials); the
#                              bundles and the .pkg are notarized and stapled
#   SUPPLE_SIGN_COMMAND        Windows: a command that signs the file named
#                              after it, e.g. signtool sign /fd sha256 /tr
#                              http://timestamp.digicert.com /td sha256 /a
#
# Inputs: NAME VERSION VENDOR ID PLATFORM OUT LICENSE NOTICES, CLAP VST3 AU
# (the built formats; AU on macOS), INSTALLER (ON: build the installer too),
# ISCC (Inno Setup's compiler, Windows), TEST (ON: the archive only, no
# identity, for the package test).
cmake_minimum_required(VERSION 3.27)
include("${CMAKE_CURRENT_LIST_DIR}/Xml.cmake")

foreach(var NAME VERSION VENDOR ID PLATFORM OUT LICENSE NOTICES)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Package.cmake: ${var} is required")
    endif()
endforeach()
foreach(var SUPPLE_SIGN_IDENTITY SUPPLE_INSTALLER_IDENTITY SUPPLE_NOTARY_PROFILE SUPPLE_SIGN_COMMAND)
    if (TEST)  # the package test: the archive alone, signed ad hoc at most
        set(${var} "")
        set(INSTALLER OFF)
    elseif ("${${var}}" STREQUAL "" AND NOT "$ENV{${var}}" STREQUAL "")
        set(${var} "$ENV{${var}}")
    endif()
endforeach()

# The installer declares x64 (Inno Setup's x64compatible): an ARM64 build in
# it would install on x64 machines that cannot load it. Until ARM64 is
# supported, there is no Windows package of one.
if (INSTALLER AND PLATFORM MATCHES "^windows" AND NOT PLATFORM STREQUAL "windows-x86_64")
    message(FATAL_ERROR "package: the Windows installer is for x64 only; ${PLATFORM} is not supported yet")
endif()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if (NOT result EQUAL 0)
        string(REPLACE ";" " " command "${ARGN}")
        message(FATAL_ERROR "package: ${command} failed:\n${out}")
    endif()
endfunction()

set(base "${NAME}-${VERSION}-${PLATFORM}")
set(work "${OUT}/.work")
set(stage "${work}/${NAME}-${VERSION}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${stage}" "${OUT}")

# --- stage --------------------------------------------------------------------------

set(formats "")
foreach(spec "CLAP:CLAP" "VST3:VST3" "AU:Components")
    string(REPLACE ":" ";" parts "${spec}")
    list(GET parts 0 format)
    list(GET parts 1 folder)
    if ("${${format}}" STREQUAL "")
        continue()
    endif()
    if (NOT EXISTS "${${format}}")
        message(FATAL_ERROR "package: ${${format}} has not been built")
    endif()
    file(MAKE_DIRECTORY "${stage}/${folder}")
    file(COPY "${${format}}" DESTINATION "${stage}/${folder}")
    get_filename_component(leaf "${${format}}" NAME)
    list(APPEND formats "${format}|${folder}|${leaf}")
endforeach()
if (NOT formats)
    message(FATAL_ERROR "package: no formats to package")
endif()
configure_file("${LICENSE}" "${stage}/LICENSE.txt" COPYONLY)
configure_file("${NOTICES}" "${stage}/NOTICES.txt" COPYONLY)

if (PLATFORM MATCHES "^macos")
    set(install "Run ${base}.pkg, or copy each folder's contents into the
folder of the same name in /Library/Audio/Plug-Ins (every user) or
~/Library/Audio/Plug-Ins (you only):

  CLAP/${NAME}.clap               -> Plug-Ins/CLAP
  VST3/${NAME}.vst3               -> Plug-Ins/VST3
  Components/${NAME}.component    -> Plug-Ins/Components (Audio Unit)
")
elseif (PLATFORM MATCHES "^windows")
    set(install "Run ${base}-setup.exe, or copy each folder's contents into the
folder of the same name in C:\\Program Files\\Common Files (every user) or
%LOCALAPPDATA%\\Programs\\Common (you only):

  CLAP\\${NAME}.clap    -> Common Files\\CLAP
  VST3\\${NAME}.vst3    -> Common Files\\VST3
")
else()
    set(install "Copy each folder's contents into ~/.clap and ~/.vst3 (you only)
or /usr/lib/clap and /usr/lib/vst3 (every user):

  CLAP/${NAME}.clap    -> ~/.clap
  VST3/${NAME}.vst3    -> ~/.vst3
")
endif()
file(WRITE "${stage}/INSTALL.txt" "${NAME} ${VERSION} by ${VENDOR}

${install}
Then rescan plugins in your host. LICENSE.txt is ${NAME}'s license;
NOTICES.txt lists the third-party software in it and their licenses.
")

# --- sign -----------------------------------------------------------------------------

set(signed "")
foreach(entry ${formats})
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 1 folder)
    list(GET parts 2 leaf)
    list(APPEND signed "${stage}/${folder}/${leaf}")
endforeach()

if (PLATFORM MATCHES "^macos")
    # Ad hoc at least: the linker signed the binary alone, and the license
    # and notices added to each bundle since are sealed only by signing the
    # bundle again.
    foreach(bundle ${signed})
        if (SUPPLE_SIGN_IDENTITY)
            run(/usr/bin/codesign --force --options runtime --timestamp --sign "${SUPPLE_SIGN_IDENTITY}" "${bundle}")
        else()
            run(/usr/bin/codesign --force --sign - --timestamp=none "${bundle}")
        endif()
        run(/usr/bin/codesign --verify --strict "${bundle}")
    endforeach()
    if (SUPPLE_NOTARY_PROFILE)
        if (NOT SUPPLE_SIGN_IDENTITY)
            message(FATAL_ERROR "package: notarizing needs SUPPLE_SIGN_IDENTITY (a Developer ID Application identity)")
        endif()
        # The bundles go to Apple once, zipped, and each gets its ticket.
        run(/usr/bin/ditto -c -k --keepParent "${stage}" "${work}/notarize.zip")
        run(/usr/bin/xcrun notarytool submit "${work}/notarize.zip" --keychain-profile "${SUPPLE_NOTARY_PROFILE}" --wait)
        foreach(bundle ${signed})
            run(/usr/bin/xcrun stapler staple "${bundle}")
        endforeach()
        file(REMOVE "${work}/notarize.zip")
    endif()
elseif (PLATFORM MATCHES "^windows" AND SUPPLE_SIGN_COMMAND)
    separate_arguments(sign NATIVE_COMMAND "${SUPPLE_SIGN_COMMAND}")
    foreach(file ${signed})
        if (NOT IS_DIRECTORY "${file}")
            run(${sign} "${file}")
        endif()
    endforeach()
endif()

# --- archive ----------------------------------------------------------------------------

file(GLOB_RECURSE staged RELATIVE "${work}" "${stage}/*")
if (PLATFORM MATCHES "^linux")
    set(archive "${OUT}/${base}.tar.gz")
    set(options czf)
    set(format "")
else()
    set(archive "${OUT}/${base}.zip")
    set(options cf)
    set(format --format=zip)
endif()
file(REMOVE "${archive}")
execute_process(COMMAND ${CMAKE_COMMAND} -E tar ${options} "${archive}" ${format} "${NAME}-${VERSION}"
    WORKING_DIRECTORY "${work}" COMMAND_ERROR_IS_FATAL ANY)
# Everything staged is in it.
execute_process(COMMAND ${CMAKE_COMMAND} -E tar tf "${archive}" OUTPUT_VARIABLE listing COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" listing "${listing}")
foreach(file ${staged})
    if (NOT file IN_LIST listing)
        message(FATAL_ERROR "package: ${file} is missing from ${archive}")
    endif()
endforeach()
message(STATUS "Wrote ${archive}")

# --- installers -------------------------------------------------------------------------

if (INSTALLER AND PLATFORM MATCHES "^macos")
    set(pkgs "${work}/pkgs")
    set(resources "${work}/resources")
    file(MAKE_DIRECTORY "${pkgs}" "${resources}")
    configure_file("${LICENSE}" "${resources}/LICENSE.txt" COPYONLY)
    # The architectures the installer may run on are the binary's own;
    # without them, an Apple silicon Mac is asked to install Rosetta first.
    list(GET signed 0 first)
    get_filename_component(stem "${first}" NAME_WLE)
    execute_process(COMMAND /usr/bin/lipo -archs "${first}/Contents/MacOS/${stem}"
        OUTPUT_VARIABLE archs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE " " "," archs "${archs}")
    set(outline "")
    set(choices "")
    supple_xml_escape(name_xml "${NAME}")
    foreach(entry ${formats})
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 format)
        list(GET parts 1 folder)
        string(TOLOWER "${format}" key)
        set(root "${work}/root-${key}")
        file(MAKE_DIRECTORY "${root}")
        file(COPY "${stage}/${folder}/" DESTINATION "${root}")
        # Installed where it says, never "relocated" to wherever the
        # Installer finds another bundle with the same identifier.
        run(/usr/bin/pkgbuild --analyze --root "${root}" "${work}/${key}.plist")
        run(/usr/bin/plutil -replace 0.BundleIsRelocatable -bool NO "${work}/${key}.plist")
        run(/usr/bin/pkgbuild --root "${root}" --component-plist "${work}/${key}.plist"
            --identifier "${ID}.${key}" --version "${VERSION}"
            --install-location "/Library/Audio/Plug-Ins/${folder}" "${pkgs}/${key}.pkg")
        if (format STREQUAL "AU")
            set(title "Audio Unit")
        else()
            set(title "${format}")
        endif()
        string(APPEND outline "        <line choice=\"${key}\"/>\n")
        string(APPEND choices "    <choice id=\"${key}\" title=\"${title}\" description=\"${name_xml} as ${title}, in /Library/Audio/Plug-Ins/${folder}\">
        <pkg-ref id=\"${ID}.${key}\"/>
    </choice>
    <pkg-ref id=\"${ID}.${key}\" version=\"${VERSION}\">${key}.pkg</pkg-ref>
")
    endforeach()
    file(WRITE "${work}/distribution.xml" "<?xml version=\"1.0\" encoding=\"utf-8\"?>
<installer-gui-script minSpecVersion=\"2\">
    <title>${name_xml} ${VERSION}</title>
    <license file=\"LICENSE.txt\"/>
    <options customize=\"allow\" require-scripts=\"false\" hostArchitectures=\"${archs}\"/>
    <domains enable_localSystem=\"true\"/>
    <choices-outline>
${outline}    </choices-outline>
${choices}</installer-gui-script>
")
    set(pkg "${OUT}/${base}.pkg")
    set(sign "")
    if (SUPPLE_INSTALLER_IDENTITY)
        set(sign --sign "${SUPPLE_INSTALLER_IDENTITY}")
    endif()
    run(/usr/bin/productbuild --distribution "${work}/distribution.xml" --package-path "${pkgs}"
        --resources "${resources}" ${sign} "${pkg}")
    if (SUPPLE_NOTARY_PROFILE)
        if (NOT SUPPLE_INSTALLER_IDENTITY)
            message(FATAL_ERROR "package: notarizing the installer needs SUPPLE_INSTALLER_IDENTITY")
        endif()
        run(/usr/bin/xcrun notarytool submit "${pkg}" --keychain-profile "${SUPPLE_NOTARY_PROFILE}" --wait)
        run(/usr/bin/xcrun stapler staple "${pkg}")
    endif()
    message(STATUS "Wrote ${pkg}")
elseif (INSTALLER AND PLATFORM MATCHES "^windows")
    if (NOT ISCC)
        message(WARNING "package: Inno Setup (iscc) not found, so no installer; "
                        "install Inno Setup 6.3 or later, or pass -DSUPPLE_ISCC=path/to/ISCC.exe")
    else()
        file(TO_NATIVE_PATH "${stage}" stage_native)
        file(TO_NATIVE_PATH "${OUT}" out_native)
        set(components "")
        set(files "")
        foreach(entry ${formats})
            string(REPLACE "|" ";" parts "${entry}")
            list(GET parts 0 format)
            list(GET parts 1 folder)
            list(GET parts 2 leaf)
            string(TOLOWER "${format}" key)
            string(APPEND components "Name: \"${key}\"; Description: \"${format}\"; Types: full custom\n")
            if (IS_DIRECTORY "${stage}/${folder}/${leaf}")
                string(APPEND files "Source: \"${stage_native}\\${folder}\\${leaf}\\*\"; DestDir: \"{commoncf64}\\${folder}\\${leaf}\"; Components: ${key}; Flags: ignoreversion recursesubdirs\n")
            else()
                string(APPEND files "Source: \"${stage_native}\\${folder}\\${leaf}\"; DestDir: \"{commoncf64}\\${folder}\"; Components: ${key}; Flags: ignoreversion\n")
            endif()
        endforeach()
        file(WRITE "${work}/setup.iss" "[Setup]
AppId=${ID}
AppName=${NAME}
AppVersion=${VERSION}
AppPublisher=${VENDOR}
DefaultDirName={commonpf64}\\${VENDOR}\\${NAME}
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
LicenseFile=${stage_native}\\LICENSE.txt
OutputDir=${out_native}
OutputBaseFilename=${base}-setup
UninstallDisplayName=${NAME}
Compression=lzma2
SolidCompression=yes

[Types]
Name: \"full\"; Description: \"Every format\"
Name: \"custom\"; Description: \"Choose formats\"; Flags: iscustom

[Components]
${components}
[Files]
${files}Source: \"${stage_native}\\LICENSE.txt\"; DestDir: \"{app}\"; Flags: ignoreversion
Source: \"${stage_native}\\NOTICES.txt\"; DestDir: \"{app}\"; Flags: ignoreversion
")
        run("${ISCC}" /Q "${work}/setup.iss")
        set(setup "${OUT}/${base}-setup.exe")
        if (SUPPLE_SIGN_COMMAND)
            separate_arguments(sign NATIVE_COMMAND "${SUPPLE_SIGN_COMMAND}")
            run(${sign} "${setup}")
        endif()
        message(STATUS "Wrote ${setup}")
    endif()
endif()

file(REMOVE_RECURSE "${work}")
