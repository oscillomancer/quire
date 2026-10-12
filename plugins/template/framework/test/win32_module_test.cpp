// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Load the product's CLAP and VST3 images together. Both contain clap_entry
// and the same Pugl class name; each editor must use its own module's class.
#include <windows.h>
#include <clap/clap.h>
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

struct Module {
    HMODULE library = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    std::string id, name;
    bool initialised = false;
    explicit Module(const char* path) {
        library = LoadLibraryA(path);
        if (!library) return;
        entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(library, "clap_entry"));
        if (!entry || !(initialised = entry->init(path))) return;
        factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory || !factory->get_plugin_count(factory)) return;
        if (const auto* descriptor = factory->get_plugin_descriptor(factory, 0)) {
            id = descriptor->id;
            name = descriptor->name;
        }
    }
    ~Module() { close(); }
    void close() {
        if (initialised) entry->deinit();
        initialised = false;
        if (library) FreeLibrary(library);
        library = nullptr;
    }
    bool ready() const { return initialised && factory && !id.empty(); }
};

struct Editor {
    clap_host_t host{};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    HWND parent = nullptr, child = nullptr;
    bool created = false;
    Editor() {
        host.clap_version = CLAP_VERSION_INIT;
        host.name = "Module ownership regression";
        host.vendor = "Plugin development";
        host.url = "";
        host.version = "1";
        host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
        host.request_restart = [](const clap_host_t*) {};
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t*) {};
    }
    ~Editor() { close(); }
    bool open(Module& module) {
        plugin = module.factory->create_plugin(module.factory, &host, module.id.c_str());
        if (!plugin || !plugin->init(plugin)) return false;
        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        if (!gui || !(created = gui->create(plugin, CLAP_WINDOW_API_WIN32, false))) return false;
        uint32_t w = 0, h = 0;
        if (!gui->get_size(plugin, &w, &h)) return false;
        parent = CreateWindowExW(0, L"STATIC", L"Module ownership", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 0, 0, static_cast<int>(w), static_cast<int>(h), nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        if (!parent) return false;
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = parent;
        if (!gui->set_parent(plugin, &window) || !gui->show(plugin)) return false;
        child = GetWindow(parent, GW_CHILD);
        return child != nullptr;
    }
    bool ownedBy(const Module& module) const {
        return child && reinterpret_cast<HMODULE>(GetClassLongPtrW(child, GCLP_HMODULE)) == module.library;
    }
    void close() {
        if (created) gui->destroy(plugin);
        created = false;
        if (plugin) plugin->destroy(plugin);
        plugin = nullptr;
        if (parent) DestroyWindow(parent);
        parent = child = nullptr;
    }
};

bool registered(const Module& module) {
    WNDCLASSEXA info{};
    info.cbSize = sizeof(info);
    return GetClassInfoExA(module.library, module.name.c_str(), &info) != 0;
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: win32-module-test product.clap product.vst3\n");
        return 2;
    }
    Module clap(argv[1]), vst3(argv[2]);
    check(clap.ready() && vst3.ready(), "both plugin formats expose their CLAP entry");
    if (!clap.ready() || !vst3.ready()) return 1;
    check(clap.name == vst3.name, "formats use the same product class name");
    Editor first, other, second;
    // The second CLAP editor opens after VST3 registered its same-named
    // class. With a NULL instance, it selects VST3's window procedure.
    check(first.open(clap), "first CLAP editor embeds");
    check(other.open(vst3), "VST3 editor embeds while CLAP stays open");
    check(second.open(clap), "second CLAP editor embeds after VST3");
    check(first.ownedBy(clap), "first CLAP editor belongs to the CLAP DLL");
    check(other.ownedBy(vst3), "VST3 editor belongs to the VST3 DLL");
    check(second.ownedBy(clap), "second CLAP editor still belongs to the CLAP DLL");
    other.close();
    check(!registered(vst3), "VST3 teardown removes its own class");
    // Avoid executing an already-diagnosed dangling procedure in the
    // unfixed regression: clean up all editors before the modules unload.
    if (failures) return 1;
    vst3.close();
    check(IsWindow(second.child) && second.ownedBy(clap), "CLAP survives unloading VST3");
    InvalidateRect(second.child, nullptr, FALSE);
    UpdateWindow(second.child);
    first.close();
    check(registered(clap), "closing one instance preserves the remaining instance's class");
    second.close();
    check(!registered(clap), "last CLAP editor teardown removes its class");
    return failures ? 1 : 0;
}
