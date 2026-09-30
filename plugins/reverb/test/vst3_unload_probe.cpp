// SPDX-License-Identifier: MIT
//
// Drives a built VST3 module the way a scanning host does: load it, read the
// factory, ModuleExit, dlclose, and confirm the module really left the
// process; then load it once more and exit with it still mapped.
//
// It guards two failures that only show once a host unloads plugins. The
// wrapper used to hold a second reference to its own image, which pinned the
// module against dlclose and then unmapped it from inside its own static
// destructors at process exit. And an exported GNU unique symbol makes glibc
// mark the module NODELETE, which hides that first failure rather than
// fixing it -- so this test also fails if the module cannot be unloaded.
//
// The factory is walked through its raw vtable so the probe needs no VST3 SDK:
// FUnknown is queryInterface/addRef/release, IPluginFactory continues with
// getFactoryInfo, countClasses, getClassInfo.
#include <dlfcn.h>

#include <cstdio>
#include <cstring>

namespace {

struct PClassInfo {
    char cid[16];
    int cardinality;
    char category[32];
    char name[64];
};

struct Factory {
    void** vtable;
};

constexpr int kRelease = 2;
constexpr int kCountClasses = 4;
constexpr int kGetClassInfo = 5;

int fail(const char* what) {
    std::fprintf(stderr, "vst3-unload: %s\n", what);
    return 1;
}

// Loads, enumerates and (unless keep) unloads. Returns nonzero on failure.
int cycle(const char* path, bool keep) {
    void* module = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!module) {
        std::fprintf(stderr, "vst3-unload: dlopen: %s\n", dlerror());
        return 1;
    }
    auto entry = reinterpret_cast<bool (*)(void*)>(dlsym(module, "ModuleEntry"));
    auto leave = reinterpret_cast<bool (*)()>(dlsym(module, "ModuleExit"));
    auto get = reinterpret_cast<Factory* (*)()>(dlsym(module, "GetPluginFactory"));
    if (!entry || !leave || !get) return fail("a VST3 entry point is not exported");
    if (!entry(module)) return fail("ModuleEntry failed");

    Factory* factory = get();
    if (!factory) return fail("GetPluginFactory returned null");
    auto count = reinterpret_cast<int (*)(Factory*)>(factory->vtable[kCountClasses]);
    auto info = reinterpret_cast<int (*)(Factory*, int, PClassInfo*)>(factory->vtable[kGetClassInfo]);
    auto release = reinterpret_cast<unsigned (*)(Factory*)>(factory->vtable[kRelease]);
    if (count(factory) < 1) return fail("the factory lists no classes");
    PClassInfo first{};
    if (info(factory, 0, &first) != 0 || first.name[0] == '\0')
        return fail("getClassInfo returned no class");
    std::printf("vst3-unload: %s (%s)\n", first.name, keep ? "kept loaded" : "unloading");
    release(factory);

    if (keep) return 0;
    if (!leave()) return fail("ModuleExit failed");
    if (dlclose(module) != 0) return fail("dlclose failed");
    // RTLD_NOLOAD only reports a module that is still mapped.
    if (void* still = dlopen(path, RTLD_LAZY | RTLD_NOLOAD)) {
        dlclose(still);
        return fail("module is still mapped after dlclose");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: vst3-unload-probe <plugin.so>");
    for (int pass = 0; pass < 2; ++pass)
        if (cycle(argv[1], false)) return 1;
    // Leave the last load mapped: the crash this guards against happens when
    // the process exits with the module's static destructors still to run.
    return cycle(argv[1], true);
}
