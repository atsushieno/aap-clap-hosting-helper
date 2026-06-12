#include "aap_clap_pkg_entry.h"
#include <clap/factory/plugin-factory.h>
#include <clap/factory/preset-discovery.h>

AapClapPkgEntry::AapClapPkgEntry(std::string                      package_name,
                                   aap::PluginListSnapshot*          snapshot,
                                   aap::PluginClient*                client,
                                   aap::PluginClientConnectionList*  connections)
    : _package_name(std::move(package_name))
    , _snapshot(snapshot)
    , _client(client)
    , _connections(connections)
{
    _entry.package_name = _package_name.c_str();
    _entry.get_factory  = s_get_factory;
    _entry.impl         = this;
}

const void* AapClapPkgEntry::s_get_factory(const aap_clap_hosting_helper_entry_t* entry,
                                             const char*              factory_id) {
    return static_cast<AapClapPkgEntry*>(entry->impl)->getFactory(factory_id);
}

const void* AapClapPkgEntry::getFactory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) {
        if (!_plugin_factory)
            _plugin_factory = std::make_unique<AapClapFactory>(
                    _snapshot, _client, _connections, _package_name);
        return _plugin_factory->clapFactory();
    }
    if (strcmp(factory_id, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 ||
        strcmp(factory_id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0) {
        if (!_preset_factory)
            _preset_factory = std::make_unique<AapClapPresetDiscoveryFactory>(
                    _package_name, _snapshot, _client, _connections);
        if (_preset_factory->count() == 0) return nullptr;
        return _preset_factory->clapFactory();
    }
    return nullptr;
}
