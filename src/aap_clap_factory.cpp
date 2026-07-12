#include "aap_clap_factory.h"
#include "aap_clap_plugin.h"
#include <aap/core/host/plugin-client-system.h>
#include <condition_variable>
#include <mutex>
#include <cstring>

// ---------------------------------------------------------------------------
// C callback trampolines
// ---------------------------------------------------------------------------

// Recover AapClapFactory* from the factory pointer by treating the factory
// as the first field of AapClapFactory::Vtable.
static AapClapFactory* self_from(const clap_plugin_factory_t* f) {
    return reinterpret_cast<const AapClapFactory::Vtable*>(f)->self;
}

static uint32_t factory_plugin_count(const clap_plugin_factory_t* f) {
    return self_from(f)->pluginCount();
}

static const clap_plugin_descriptor_t* factory_plugin_descriptor(
        const clap_plugin_factory_t* f, uint32_t index) {
    return self_from(f)->pluginDescriptor(index);
}

static const clap_plugin_t* factory_create_plugin(
        const clap_plugin_factory_t* f,
        const clap_host_t* host,
        const char* plugin_id) {
    return self_from(f)->createPlugin(host, plugin_id);
}

// ---------------------------------------------------------------------------
// AapClapFactory
// ---------------------------------------------------------------------------

AapClapFactory::AapClapFactory(aap::PluginListSnapshot*      snapshot,
                                aap::PluginClient*             client,
                                aap::PluginClientConnectionList* connections,
                                std::string                    package_name)
    : _snapshot(snapshot)
    , _client(client)
    , _connections(connections)
    , _package_name(std::move(package_name))
{
    _vtable.self                        = this;
    _vtable.clap.get_plugin_count      = factory_plugin_count;
    _vtable.clap.get_plugin_descriptor = factory_plugin_descriptor;
    _vtable.clap.create_plugin         = factory_create_plugin;

    buildEntries();
}

const char* AapClapFactory::mapCategory(const std::string& cat) {
    if (cat.find("Instrument") != std::string::npos ||
        cat.find("Synth")      != std::string::npos)
        return CLAP_PLUGIN_FEATURE_INSTRUMENT;
    if (cat.find("Effect")     != std::string::npos ||
        cat.find("Delay")      != std::string::npos ||
        cat.find("Reverb")     != std::string::npos)
        return CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
    return nullptr;
}

void AapClapFactory::buildEntries() {
    int32_t count = _snapshot->getNumPluginInformation();
    for (int32_t i = 0; i < count; i++) {
        const aap::PluginInformation* info = _snapshot->getPluginInformation(i);
        if (!info) continue;
        if (info->getPluginPackageName() != _package_name) continue;

        auto e = std::make_unique<Entry>();
        e->info        = info;
        e->id_str      = info->getPluginID();
        e->name_str    = info->getDisplayName();
        e->vendor_str  = info->getDeveloperName();
        e->version_str = info->getVersion();

        const char* feature = mapCategory(info->getPrimaryCategory());
        if (feature) e->features[0] = feature;

        e->desc.clap_version = CLAP_VERSION_INIT;
        e->desc.id           = e->id_str.c_str();
        e->desc.name         = e->name_str.c_str();
        e->desc.vendor       = e->vendor_str.c_str();
        e->desc.url          = "";
        e->desc.manual_url   = "";
        e->desc.support_url  = "";
        e->desc.version      = e->version_str.c_str();
        e->desc.description  = "";
        e->desc.features     = e->features;

        _entries.push_back(std::move(e));
    }
}

uint32_t AapClapFactory::pluginCount() const {
    return (uint32_t)_entries.size();
}

const clap_plugin_descriptor_t* AapClapFactory::pluginDescriptor(uint32_t index) const {
    if (index >= _entries.size()) return nullptr;
    return &_entries[index]->desc;
}

bool AapClapFactory::ensureServiceConnected() {
    // Check using the first plugin in this package to get the service class name.
    if (_entries.empty()) return false;
    const aap::PluginInformation* info = _entries[0]->info;

    auto* handle = _connections->getServiceHandleForConnectedPlugin(
            info->getPluginPackageName(), info->getPluginLocalName());
    if (handle != nullptr) return true;

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string connect_error;

    aap::PluginClientSystem::getInstance()->ensurePluginServiceConnected(
        _connections,
        info->getPluginPackageName(),
        [&](std::string& err) {
            std::unique_lock<std::mutex> lk(m);
            connect_error = err;
            done = true;
            cv.notify_one();
        });

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]{ return done; });
    return connect_error.empty();
}

const clap_plugin_t* AapClapFactory::createPlugin(const clap_host_t* host,
                                                    const char* plugin_id) {
    if (!host || !plugin_id) return nullptr;

    const aap::PluginInformation* info = _snapshot->getPluginInformation(plugin_id);
    if (!info || info->getPluginPackageName() != _package_name) return nullptr;

    const clap_plugin_descriptor_t* desc = nullptr;
    for (auto& e : _entries) {
        if (e->id_str == plugin_id) { desc = &e->desc; break; }
    }
    if (!desc) return nullptr;

    if (!ensureServiceConnected()) return nullptr;

    auto result = _client->createInstance(plugin_id, /*isRemoteExplicit=*/true);
    if (!result.error.empty()) return nullptr;

    auto* instance = static_cast<aap::RemotePluginInstance*>(
            _client->getInstanceById(result.value));
    if (!instance) return nullptr;

    auto* plugin = new AapClapPlugin(desc, host, instance, _client);
    return plugin->clapPlugin();
}
