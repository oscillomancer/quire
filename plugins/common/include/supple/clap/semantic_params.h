/*
 * CLAP extension: com.suppleaudio.semantic-params/1
 * Spec: docs/specs/semantic-params-extension.md (frozen).
 *
 * Lets a host that understands it see a plugin's parameters with keys,
 * physical units, curves and roles, and read/replace/merge/validate its
 * canonical JSON patch. Hosts that do not know it use clap.params and
 * clap.state unchanged.
 *
 * C ABI; depends only on the CLAP headers. All functions are main-thread.
 */
#pragma once

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const CLAP_CONSTEXPR char CLAP_EXT_SUPPLE_SEMANTIC_PARAMS[] =
    "com.suppleaudio.semantic-params/1";

enum {
    CLAP_SUPPLE_PATCH_DETAIL_FULL = 0,    /* every non-readonly value */
    CLAP_SUPPLE_PATCH_DETAIL_CHANGED = 1, /* only values differing from defaults */
};

enum {
    CLAP_SUPPLE_SET_REPLACE = 0,       /* the document is a complete patch */
    CLAP_SUPPLE_SET_MERGE = 1,         /* RFC 7396 merge patch into the current patch */
    CLAP_SUPPLE_SET_VALIDATE_ONLY = 2, /* validate as MERGE would; change nothing */
};

typedef struct clap_plugin_supple_semantic_params {
    /* sizeof(clap_plugin_supple_semantic_params_t) of the header the plugin was
     * built with. Hosts must not call members beyond it; new members are only
     * ever appended. */
    uint32_t struct_size;

    /* [main-thread] Writes the semantic schema document (JSON) to `out`. */
    bool(CLAP_ABI* get_schema)(const clap_plugin_t* plugin, const clap_ostream_t* out);

    /* [main-thread] Writes the current patch as canonical JSON. */
    bool(CLAP_ABI* get_patch)(const clap_plugin_t* plugin, uint32_t detail,
                              const clap_ostream_t* out);

    /* [main-thread] Applies `json` (UTF-8, `size` bytes, not NUL-terminated).
     * Transactional: the whole document applies or nothing changes. On failure
     * returns false and, if `errors` is non-NULL, writes a JSON array of error
     * objects {code, message, path, hint, details}. May be called while the
     * plugin is active and processing; the change takes effect at a block
     * boundary without blocking the audio thread. */
    bool(CLAP_ABI* set_patch)(const clap_plugin_t* plugin, uint32_t mode, const char* json,
                              uint32_t size, const clap_ostream_t* errors);

    /* [main-thread] Writes a compact prose description of the current patch
     * (UTF-8 plain text, <= 600 characters for DETAIL_CHANGED). */
    bool(CLAP_ABI* describe)(const clap_plugin_t* plugin, uint32_t detail,
                             const clap_ostream_t* out);
    /* [main-thread] Optional live supple.analysis-report/1.0 JSON. Hosts check
     * struct_size through this member AND this pointer before calling it.
     * NULL means no telemetry; false means unavailable or stream failure. */
    bool(CLAP_ABI* get_telemetry)(const clap_plugin_t* plugin, const clap_ostream_t* out);
} clap_plugin_supple_semantic_params_t;

enum {
    CLAP_SUPPLE_CHANGED_PATCH = 1u
                                << 0, /* values changed (GUI edit, preset load in the plugin UI) */
    CLAP_SUPPLE_CHANGED_SCHEMA = 1u << 1, /* the schema changed */
};

typedef struct clap_host_supple_semantic_params {
    uint32_t struct_size;
    /* [main-thread] Tells the host to re-read the patch and/or schema. */
    void(CLAP_ABI* changed)(const clap_host_t* host, uint32_t flags);
} clap_host_supple_semantic_params_t;

#ifdef __cplusplus
}
#endif
