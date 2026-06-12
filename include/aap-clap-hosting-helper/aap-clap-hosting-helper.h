#pragma once

#ifdef ANDROID
#include <jni.h>
#endif

#include <stdint.h>
#include <clap/plugin.h>

// The CLAP window API identifier for AAP GUI, versioned to match the AAP GUI extension.
//
// clap_window_t semantics for CLAP_WINDOW_API_AAP_GUI:
//   window.ptr is a void** (pointer to a void* slot allocated by the host).
//
// On a successful set_parent() call the plugin writes the Android SurfaceView
// (jobject) into *window.ptr.  The host must embed that SurfaceView in its
// layout; the SurfaceControlViewHost connection completes automatically once
// the view is attached to a live window.  Pass ptr == nullptr to skip the
// view-handle handoff (e.g. when reconnecting after a size change).
#define CLAP_WINDOW_API_AAP_GUI "aap/gui/v4"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// aap-clap-hosting-helper: host-side AAP → CLAP adapter
//
// Models CLAP's per-library entry concept, one entry per installed AAP
// package (Android service). Only the services actually used by the host
// are connected — querying one package's factory does not bind other services.
//
// Usage for a CLAP host app developer on Android:
//
//   1. Call aap_clap_hosting_helper_init() once at startup (after the Android Context is ready).
//   2. Call aap_clap_hosting_helper_get_entry_count() / aap_clap_hosting_helper_get_entry() to iterate entries.
//   3. On each entry call get_factory(entry, CLAP_PLUGIN_FACTORY_ID) to enumerate
//      that package's plugins, or get_factory with the preset discovery ID for presets.
//   4. Call aap_clap_hosting_helper_deinit() on shutdown.
//
// Plugin developers do not need to package or link aap-clap-hosting-helper.
// ---------------------------------------------------------------------------

// One entry per AAP package (Android service). Mirrors the role of a .clap
// shared library in standard CLAP: get_factory returns different factory types
// by ID string, scoped to this package only.
typedef struct aap_clap_hosting_helper_entry_t {
    const char* package_name;
    // factory_id: CLAP_PLUGIN_FACTORY_ID, CLAP_PRESET_DISCOVERY_FACTORY_ID, etc.
    // Returns nullptr for unsupported factory types.
    const void* (*get_factory)(const struct aap_clap_hosting_helper_entry_t* entry,
                               const char*                             factory_id);
    void* impl;  // opaque; do not access directly
} aap_clap_hosting_helper_entry_t;

#ifdef ANDROID
bool aap_clap_hosting_helper_init(JNIEnv* env, jobject android_context);
#else
bool aap_clap_hosting_helper_init(void);
#endif

void aap_clap_hosting_helper_deinit(void);

// Number of installed AAP packages discovered at init time.
uint32_t                       aap_clap_hosting_helper_get_entry_count(void);

// Returns the entry for the given package index.
// Valid until aap_clap_hosting_helper_deinit() is called.
const aap_clap_hosting_helper_entry_t* aap_clap_hosting_helper_get_entry(uint32_t index);


#ifdef __cplusplus
} // extern "C"
#endif
