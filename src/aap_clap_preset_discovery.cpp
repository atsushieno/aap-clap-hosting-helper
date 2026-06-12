#include "aap_clap_preset_discovery.h"
#include <aap/core/host/plugin-client-system.h>
#include <aap/ext/presets.h>
#include <cstring>
#include <cstdio>
#include <condition_variable>
#include <mutex>

// ---------------------------------------------------------------------------
// Minimal indexer — used by the provider during init() to declare locations.
// ---------------------------------------------------------------------------

static bool indexer_declare_filetype(const clap_preset_discovery_indexer_t*,
                                     const clap_preset_discovery_filetype_t*) { return true; }
static bool indexer_declare_location(const clap_preset_discovery_indexer_t*,
                                     const clap_preset_discovery_location_t*) { return true; }
static bool indexer_declare_soundpack(const clap_preset_discovery_indexer_t*,
                                      const clap_preset_discovery_soundpack_t*) { return true; }
static const void* indexer_get_extension(const clap_preset_discovery_indexer_t*,
                                         const char*) { return nullptr; }

static clap_preset_discovery_indexer_t s_noop_indexer = {
    CLAP_VERSION_INIT,
    "aap-clap-host", nullptr, nullptr, nullptr,
    nullptr,
    indexer_declare_filetype,
    indexer_declare_location,
    indexer_declare_soundpack,
    indexer_get_extension
};

// ---------------------------------------------------------------------------
// AapClapPresetDiscoveryProvider
// ---------------------------------------------------------------------------

AapClapPresetDiscoveryProvider::AapClapPresetDiscoveryProvider(
        std::string plugin_id,
        std::string clap_plugin_id,
        aap::PluginListSnapshot*         snapshot,
        aap::PluginClient*               client,
        aap::PluginClientConnectionList* connections)
    : _plugin_id(std::move(plugin_id))
    , _clap_plugin_id(std::move(clap_plugin_id))
    , _snapshot(snapshot)
    , _client(client)
    , _connections(connections)
{
    _provider_id = "aap.presets." + _plugin_id;

    auto* info = _snapshot->getPluginInformation(_plugin_id.c_str());
    _name = (info ? std::string(info->getDisplayName()) : _plugin_id) + " Presets";

    _desc.clap_version = CLAP_VERSION_INIT;
    _desc.id           = _provider_id.c_str();
    _desc.name         = _name.c_str();
    _desc.vendor       = "";

    _provider.desc          = &_desc;
    _provider.provider_data = this;
    _provider.init          = s_init;
    _provider.destroy       = s_destroy;
    _provider.get_metadata  = s_get_metadata;
    _provider.get_extension = s_get_extension;
}

bool AapClapPresetDiscoveryProvider::s_init(const clap_preset_discovery_provider_t* p) {
    return static_cast<AapClapPresetDiscoveryProvider*>(p->provider_data)->init(&s_noop_indexer);
}

void AapClapPresetDiscoveryProvider::s_destroy(const clap_preset_discovery_provider_t* p) {
    // Provider lifetime is managed by AapClapPresetDiscoveryFactory. Nothing to free here.
    (void)p;
}

bool AapClapPresetDiscoveryProvider::s_get_metadata(
        const clap_preset_discovery_provider_t*          p,
        uint32_t                                         kind,
        const char*                                      location,
        const clap_preset_discovery_metadata_receiver_t* recv) {
    return static_cast<AapClapPresetDiscoveryProvider*>(p->provider_data)
               ->getMetadata(kind, location, recv);
}

const void* AapClapPresetDiscoveryProvider::s_get_extension(
        const clap_preset_discovery_provider_t*, const char*) {
    return nullptr;
}

bool AapClapPresetDiscoveryProvider::init(const clap_preset_discovery_indexer_t* indexer) {
    _indexer = indexer;
    clap_preset_discovery_location_t loc{};
    loc.flags    = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
    loc.name     = _name.c_str();
    loc.kind     = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
    loc.location = nullptr;
    indexer->declare_location(indexer, &loc);
    return true;
}

void AapClapPresetDiscoveryProvider::destroy() {
    _indexer = nullptr;
}

// Ensure the AAP service for this plugin is connected before creating instances.
static bool ensureConnected(aap::PluginListSnapshot*         snapshot,
                             aap::PluginClient*               client,
                             aap::PluginClientConnectionList* connections,
                             const std::string&               plugin_id) {
    auto* info = snapshot->getPluginInformation(plugin_id.c_str());
    if (!info) return false;

    auto* handle = connections->getServiceHandleForConnectedPlugin(
            info->getPluginPackageName(), info->getPluginLocalName());
    if (handle) return true;

    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string err;

    aap::PluginClientSystem::getInstance()->ensurePluginServiceConnected(
        connections,
        info->getPluginPackageName(),
        [&](std::string& e) {
            std::unique_lock<std::mutex> lk(m);
            err = e;
            done = true;
            cv.notify_one();
        });

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]{ return done; });
    return err.empty();
}

bool AapClapPresetDiscoveryProvider::getMetadata(
        uint32_t kind, const char* location,
        const clap_preset_discovery_metadata_receiver_t* recv) const {

    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location != nullptr)
        return false;

    auto* info = _snapshot->getPluginInformation(_plugin_id.c_str());
    if (!info || !info->hasExtension(AAP_PRESETS_EXTENSION_URI))
        return false;

    if (!ensureConnected(_snapshot, _client, _connections, _plugin_id))
        return false;

    auto result = _client->createInstance(_plugin_id.c_str(), 48000, /*isRemoteExplicit=*/true);
    if (!result.error.empty()) return false;

    auto* instance = static_cast<aap::RemotePluginInstance*>(
            _client->getInstanceById(result.value));
    if (!instance) return false;

    auto& ext = instance->getStandardExtensions();
    int32_t count = ext.getPresetCount();

    clap_universal_plugin_id_t uid{ "clap", _clap_plugin_id.c_str() };

    for (int32_t i = 0; i < count; i++) {
        aap_preset_t preset{};
        ext.getPreset(i, preset);

        char load_key[32];
        snprintf(load_key, sizeof(load_key), "%d", i);

        if (!recv->begin_preset(recv, preset.name, load_key))
            break;
        recv->add_plugin_id(recv, &uid);
    }

    _client->destroyInstance(instance);
    return true;
}

// ---------------------------------------------------------------------------
// AapClapPresetDiscoveryFactory
// ---------------------------------------------------------------------------

AapClapPresetDiscoveryFactory::AapClapPresetDiscoveryFactory(
        std::string                      package_name,
        aap::PluginListSnapshot*         snapshot,
        aap::PluginClient*               client,
        aap::PluginClientConnectionList* connections)
{
    _vtable.self                  = this;
    _vtable.clap.count            = s_count;
    _vtable.clap.get_descriptor   = s_get_descriptor;
    _vtable.clap.create           = s_create;

    int32_t n = snapshot->getNumPluginInformation();
    for (int32_t i = 0; i < n; i++) {
        const aap::PluginInformation* info = snapshot->getPluginInformation(i);
        if (!info) continue;
        if (info->getPluginPackageName() != package_name) continue;
        if (!info->hasExtension(AAP_PRESETS_EXTENSION_URI)) continue;

        _providers.push_back(std::make_unique<AapClapPresetDiscoveryProvider>(
            info->getPluginID(),
            info->getPluginID(),   // CLAP plugin ID == AAP plugin ID
            snapshot,
            client,
            connections));
    }
}

static AapClapPresetDiscoveryFactory* self_from_factory(const clap_preset_discovery_factory_t* f) {
    return reinterpret_cast<const AapClapPresetDiscoveryFactory::Vtable*>(f)->self;
}

uint32_t AapClapPresetDiscoveryFactory::s_count(const clap_preset_discovery_factory_t* f) {
    return self_from_factory(f)->count();
}

const clap_preset_discovery_provider_descriptor_t*
AapClapPresetDiscoveryFactory::s_get_descriptor(
        const clap_preset_discovery_factory_t* f, uint32_t index) {
    return self_from_factory(f)->getDescriptor(index);
}

const clap_preset_discovery_provider_t*
AapClapPresetDiscoveryFactory::s_create(
        const clap_preset_discovery_factory_t*  f,
        const clap_preset_discovery_indexer_t*  indexer,
        const char*                             provider_id) {
    return self_from_factory(f)->create(indexer, provider_id);
}

uint32_t AapClapPresetDiscoveryFactory::count() const {
    return (uint32_t)_providers.size();
}

const clap_preset_discovery_provider_descriptor_t*
AapClapPresetDiscoveryFactory::getDescriptor(uint32_t index) const {
    if (index >= _providers.size()) return nullptr;
    return _providers[index]->descriptor();
}

const clap_preset_discovery_provider_t*
AapClapPresetDiscoveryFactory::create(
        const clap_preset_discovery_indexer_t* indexer,
        const char*                            provider_id) const {
    for (auto& p : _providers) {
        if (strcmp(p->descriptor()->id, provider_id) == 0) {
            p->init(indexer);
            return p->provider();
        }
    }
    return nullptr;
}
