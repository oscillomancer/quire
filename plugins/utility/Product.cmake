# Read the one display-name definition used by the SDK-free engine and GUI.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/engine/include/utility/params.h" utility_name_line
     REGEX "kDisplayName")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" UTILITY_DISPLAY_NAME "${utility_name_line}")
