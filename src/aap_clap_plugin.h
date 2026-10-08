#pragma once

#include <clap/helpers/plugin.hh>
#include <clap/helpers/event-list.hh>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/preset-load.h>
#include <aap/core/host/plugin-instance.h>
#include <aap/core/host/plugin-host.h>
#include <aap/ext/parameters.h>
#include <aap/ext/presets.h>
#include <aap/ext/gui.h>
#include "buffer_bridge.h"

#include <string>
#include <vector>

class AapClapPlugin final : public clap::helpers::Plugin<
    clap::helpers::MisbehaviourHandler::Ignore,
    clap::helpers::CheckingLevel::Maximal> {
public:
    AapClapPlugin(const clap_plugin_descriptor_t* desc,
                  const clap_host_t*               host,
                  aap::RemotePluginInstance*        instance,
                  aap::PluginClient*                client);
    ~AapClapPlugin() override;

    static AapClapPlugin& fromPlugin(const clap_plugin_t* p) {
        return static_cast<AapClapPlugin&>(from(p));
    }
    aap::RemotePluginInstance& instance() { return *_instance; }

protected:
    // clap_plugin lifecycle
    bool init() noexcept override;
    bool activate(double sample_rate,
                  uint32_t min_frames,
                  uint32_t max_frames) noexcept override;
    void deactivate() noexcept override;
    void onMainThread() noexcept override;
    bool startProcessing() noexcept override { return true; }
    void stopProcessing() noexcept override {}
    void reset() noexcept override {}
    clap_process_status process(const clap_process_t* p) noexcept override;

    // clap_plugin_audio_ports
    bool implementsAudioPorts() const noexcept override {
        return !_ports.audio_in.empty() || !_ports.audio_out.empty();
    }
    uint32_t audioPortsCount(bool isInput) const noexcept override;
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info* info) const noexcept override;

    // clap_plugin_note_ports
    bool implementsNotePorts() const noexcept override {
        return _ports.midi_in >= 0 || _ports.midi_out >= 0;
    }
    uint32_t notePortsCount(bool isInput) const noexcept override;
    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info* info) const noexcept override;

    // clap_plugin_params
    bool implementsParams() const noexcept override {
        return _instance->getNumParameters() > 0;
    }
    uint32_t paramsCount() const noexcept override;
    bool paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept override;
    bool paramsValue(clap_id paramId, double* value) noexcept override;
    bool paramsValueToText(clap_id paramId, double value,
                           char* display, uint32_t size) noexcept override;
    bool paramsTextToValue(clap_id paramId, const char* display,
                           double* value) noexcept override;
    void paramsFlush(const clap_input_events*  in,
                     const clap_output_events* out) noexcept override;

    // clap_plugin_state
    bool implementsState() const noexcept override;
    bool stateSave(const clap_ostream* stream) noexcept override;
    bool stateLoad(const clap_istream* stream) noexcept override;

    // clap_plugin_preset_load
    bool implementsPresetLoad() const noexcept override;
    bool presetLoadFromLocation(uint32_t location_kind,
                                const char* location,
                                const char* load_key) noexcept override;

    // clap_plugin_gui
    bool implementsGui() const noexcept override;
    bool guiIsApiSupported(const char* api, bool isFloating) noexcept override;
    bool guiGetPreferredApi(const char** api, bool* is_floating) noexcept override;
    bool guiCreate(const char* api, bool isFloating) noexcept override;
    void guiDestroy() noexcept override;
    bool guiSetScale(double) noexcept override { return false; }
    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiCanResize() const noexcept override { return _gui_created; }
    bool guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept override;
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiSetSize(uint32_t width, uint32_t height) noexcept override;
    bool guiSetParent(const clap_window* window) noexcept override;
    bool guiSetTransient(const clap_window*) noexcept override { return false; }
    void guiSuggestTitle(const char*) noexcept override {}
    bool guiShow() noexcept override;
    bool guiHide() noexcept override;

private:
    aap::RemotePluginInstance* _instance;
    aap::PluginClient*         _client;

    // Port layout built from PluginInformation in init()
    buffer_bridge::PortLayout _ports;

    // Parameter cache built from PluginInformation in init()
    std::vector<aap_parameter_info_t> _param_cache;
    std::vector<double>               _param_values;

    // Persistent UMP buffer: avoids per-process heap allocation.
    // Holds converted UMP bytes before handing to addEventUmpInput().
    std::vector<uint8_t> _ump_scratch;

    // GUI state
    bool     _gui_created{false};
    bool     _gui_connected{false};  // connectRemoteNativeView already called
    uint32_t _gui_width{0};
    uint32_t _gui_height{0};

    // AAP_BUSES_CHANGED_* from the plugin, applied on the main thread when deactivated.
    std::atomic<uint32_t> _pending_bus_changes{0};

    // Helpers
    int32_t paramIndexById(clap_id id) const;
    bool    updateParamValue(clap_id id, double value);
    void    injectInputEvents(const clap_input_events* in);
    void    drainOutputEvents(const clap_output_events* out);
};
