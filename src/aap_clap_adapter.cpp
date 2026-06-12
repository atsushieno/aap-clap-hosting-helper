#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>
#include <aap/core/host/plugin-connections.h>
#include <aap/core/host/plugin-host.h>
#include <aap/core/host/android/audio-plugin-host-android.h>
#include <aap/core/android/android-application-context.h>
#include "aap_clap_pkg_entry.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef ANDROID
#include <jni.h>
#endif

// ---------------------------------------------------------------------------
// Singleton adapter state
// ---------------------------------------------------------------------------

struct AapClapHostingHelperAdapter {
    aap::PluginListSnapshot                        snapshot;
    aap::PluginClientConnectionList*               connections{nullptr};
    std::unique_ptr<aap::PluginClient>             client;
    std::vector<std::unique_ptr<AapClapPkgEntry>>  entries;
    std::vector<const aap_clap_hosting_helper_entry_t*>   entry_ptrs; // stable C pointers
};

static AapClapHostingHelperAdapter* g_adapter = nullptr;

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

#ifdef ANDROID
bool aap_clap_hosting_helper_init(JNIEnv* env, jobject android_context) {
#else
bool aap_clap_hosting_helper_init(void) {
#endif
    if (g_adapter) return true; // already initialised

#ifdef ANDROID
    aap::set_application_context(env, android_context);

    // AAP requires AudioPluginNatives.initializeAAPJni() to be called before any JNI
    // metadata (class/method IDs) are accessed. In a normal AAP host app this is done
    // from Kotlin; we do it here via JNI since we are a library, not an app.
    jclass cls = env->FindClass("org/androidaudioplugin/AudioPluginNatives");
    if (cls) {
        jmethodID mid = env->GetStaticMethodID(cls, "initializeAAPJni",
                                               "(Landroid/content/Context;)V");
        if (mid)
            env->CallStaticVoidMethod(cls, mid, android_context);
        else
            env->ExceptionClear();
    } else {
        env->ExceptionClear();
    }
#endif

    auto* adapter = new AapClapHostingHelperAdapter();

    adapter->snapshot    = aap::PluginListSnapshot::queryServices();
    // Connector instance id 0 is the standard single-process connector.
    adapter->connections = aap::getPluginConnectionListByConnectorInstanceId(0, /*createIfNotExist=*/true);
    adapter->client      = std::make_unique<aap::PluginClient>(
                               adapter->connections, &adapter->snapshot);

    // Group plugins by package name; create one entry per package.
    std::unordered_map<std::string, bool> seen;
    int32_t count = adapter->snapshot.getNumPluginInformation();
    for (int32_t i = 0; i < count; i++) {
        const aap::PluginInformation* info = adapter->snapshot.getPluginInformation(i);
        if (!info) continue;
        const std::string& pkg = info->getPluginPackageName();
        if (seen.count(pkg)) continue;
        seen[pkg] = true;

        adapter->entries.push_back(std::make_unique<AapClapPkgEntry>(
                pkg,
                &adapter->snapshot,
                adapter->client.get(),
                adapter->connections));
        adapter->entry_ptrs.push_back(adapter->entries.back()->cEntry());
    }

    g_adapter = adapter;
    return true;
}

void aap_clap_hosting_helper_deinit(void) {
    delete g_adapter;
    g_adapter = nullptr;
}

uint32_t aap_clap_hosting_helper_get_entry_count(void) {
    return g_adapter ? (uint32_t)g_adapter->entry_ptrs.size() : 0;
}

const aap_clap_hosting_helper_entry_t* aap_clap_hosting_helper_get_entry(uint32_t index) {
    if (!g_adapter || index >= g_adapter->entry_ptrs.size()) return nullptr;
    return g_adapter->entry_ptrs[index];
}
