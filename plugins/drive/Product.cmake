# Read the one display-name definition used by the SDK-free engine and GUI.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/engine/include/drive/params.h" drive_name_line
     REGEX "kDisplayName")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" DRIVE_DISPLAY_NAME "${drive_name_line}")
