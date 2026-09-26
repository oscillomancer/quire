# Shared core libraries. Order matters only for readability; CMake resolves
# targets lazily.
add_subdirectory(core-base)
add_subdirectory(core-json)
add_subdirectory(core-schema)
add_subdirectory(core-params)
add_subdirectory(core-patch)
add_subdirectory(command-bus)
add_subdirectory(audio-file)
