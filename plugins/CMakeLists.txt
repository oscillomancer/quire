# Shared plugin support is always built (the host needs the extension ABI).
add_subdirectory(common)

# Template-derived stock plugins are added here (ADR-0011) when
# SUPPLE_BUILD_PLUGINS is ON.
