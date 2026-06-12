#pragma once

#include <clap/factory/preset-discovery.h>
#include <aap/core/host/plugin-host.h>
#include <aap/core/host/plugin-connections.h>
#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>
#include <string>
#include <vector>
#include <memory>

// One preset discovery provider per plugin. Wraps AAP preset metadata as
// CLAP preset discovery (CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN).
// load_key format: decimal preset index string ("0", "1", ...).
class AapClapPresetDiscoveryProvider {
public:
    AapClapPresetDiscoveryProvider(std::string plugin_id,
                                    std::string clap_plugin_id,
                                    aap::PluginListSnapshot*         snapshot,
                                    aap::PluginClient*               client,
                                    aap::PluginClientConnectionList* connections);

    const clap_preset_discovery_provider_t*            provider()    const { return &_provider; }
    const clap_preset_discovery_provider_descriptor_t* descriptor()  const { return &_desc; }
    bool        init(const clap_preset_discovery_indexer_t* indexer);

private:
    void        destroy();
    bool        getMetadata(uint32_t location_kind,
                            const char* location,
                            const clap_preset_discovery_metadata_receiver_t* recv) const;

    static bool       s_init(const clap_preset_discovery_provider_t*);
    static void       s_destroy(const clap_preset_discovery_provider_t*);
    static bool       s_get_metadata(const clap_preset_discovery_provider_t*,
                                     uint32_t, const char*,
                                     const clap_preset_discovery_metadata_receiver_t*);
    static const void* s_get_extension(const clap_preset_discovery_provider_t*, const char*);

    std::string  _plugin_id;
    std::string  _clap_plugin_id;
    std::string  _provider_id;
    std::string  _name;
    aap::PluginListSnapshot*         _snapshot;
    aap::PluginClient*               _client;
    aap::PluginClientConnectionList* _connections;
    const clap_preset_discovery_indexer_t* _indexer{nullptr};
    clap_preset_discovery_provider_descriptor_t _desc{};
    clap_preset_discovery_provider_t            _provider{};
};

// One preset discovery factory per AAP package.
// Returned by aap_clap_hosting_helper_entry_t::get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID).
class AapClapPresetDiscoveryFactory {
public:
    AapClapPresetDiscoveryFactory(std::string                      package_name,
                                   aap::PluginListSnapshot*         snapshot,
                                   aap::PluginClient*               client,
                                   aap::PluginClientConnectionList* connections);

    // clap_preset_discovery_factory_t has no user-data field; embed it as the
    // first member of Vtable so trampolines can recover `this` via cast.
    struct Vtable {
        clap_preset_discovery_factory_t clap{};  // must be first
        AapClapPresetDiscoveryFactory*  self{};
    };

    const clap_preset_discovery_factory_t* clapFactory() const { return &_vtable.clap; }
    uint32_t count() const;

private:
    const clap_preset_discovery_provider_descriptor_t* getDescriptor(uint32_t index) const;
    const clap_preset_discovery_provider_t*            create(
        const clap_preset_discovery_indexer_t* indexer,
        const char*                            provider_id) const;

    static uint32_t     s_count(const clap_preset_discovery_factory_t*);
    static const clap_preset_discovery_provider_descriptor_t* s_get_descriptor(
        const clap_preset_discovery_factory_t*, uint32_t);
    static const clap_preset_discovery_provider_t* s_create(
        const clap_preset_discovery_factory_t*,
        const clap_preset_discovery_indexer_t*,
        const char*);

    Vtable _vtable{};
    std::vector<std::unique_ptr<AapClapPresetDiscoveryProvider>> _providers;
};
