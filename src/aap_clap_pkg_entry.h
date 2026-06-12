#pragma once

#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>
#include <aap/core/host/plugin-host.h>
#include <aap/core/host/plugin-connections.h>
#include "aap_clap_factory.h"
#include "aap_clap_preset_discovery.h"
#include <memory>
#include <string>

// One entry per AAP package. Mirrors the role of a .clap shared library:
// get_factory(id) returns different factory types scoped to this package.
// The PluginClient and connection list are shared across all entries.
class AapClapPkgEntry {
public:
    AapClapPkgEntry(std::string                      package_name,
                    aap::PluginListSnapshot*          snapshot,
                    aap::PluginClient*                client,
                    aap::PluginClientConnectionList*  connections);

    const aap_clap_hosting_helper_entry_t* cEntry() { return &_entry; }

private:
    static const void* s_get_factory(const aap_clap_hosting_helper_entry_t* entry,
                                     const char*              factory_id);
    const void* getFactory(const char* factory_id);

    std::string                      _package_name;
    aap_clap_hosting_helper_entry_t  _entry{};
    aap::PluginListSnapshot*         _snapshot;
    aap::PluginClient*               _client;
    aap::PluginClientConnectionList* _connections;
    std::unique_ptr<AapClapFactory>                 _plugin_factory;    // lazy
    std::unique_ptr<AapClapPresetDiscoveryFactory>  _preset_factory;    // lazy
};
