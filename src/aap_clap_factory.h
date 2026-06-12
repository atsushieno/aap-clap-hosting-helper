#pragma once

#include <clap/factory/plugin-factory.h>
#include <clap/plugin-features.h>
#include <aap/core/host/plugin-host.h>
#include <aap/core/host/plugin-connections.h>
#include <memory>
#include <string>
#include <vector>

// Plugin factory scoped to a single AAP package (Android service).
// All instances share the global PluginClient and PluginClientConnectionList;
// only this package's service is connected on first use.
class AapClapFactory {
public:
    AapClapFactory(aap::PluginListSnapshot*      snapshot,
                   aap::PluginClient*             client,
                   aap::PluginClientConnectionList* connections,
                   std::string                    package_name);

    // clap_plugin_factory_t has no user-data field; embed it as the first
    // member of this struct so trampolines can recover `this` from the factory pointer.
    struct Vtable {
        clap_plugin_factory_t clap{};  // must be first
        AapClapFactory*       self{};
    };

    const clap_plugin_factory_t* clapFactory() { return &_vtable.clap; }

    uint32_t                         pluginCount() const;
    const clap_plugin_descriptor_t*  pluginDescriptor(uint32_t index) const;
    const clap_plugin_t*             createPlugin(const clap_host_t* host,
                                                  const char*        plugin_id);

private:
    struct Entry {
        const aap::PluginInformation* info;   // owned by snapshot
        std::string id_str;
        std::string name_str;
        std::string vendor_str;
        std::string version_str;
        const char* features[3]{nullptr, nullptr, nullptr};
        clap_plugin_descriptor_t desc{};
    };

    void buildEntries();
    static const char* mapCategory(const std::string& cat);
    bool ensureServiceConnected();

    aap::PluginListSnapshot*         _snapshot;
    aap::PluginClient*               _client;
    aap::PluginClientConnectionList* _connections;
    std::string                      _package_name;
    std::vector<std::unique_ptr<Entry>> _entries;
    Vtable _vtable{};
};
