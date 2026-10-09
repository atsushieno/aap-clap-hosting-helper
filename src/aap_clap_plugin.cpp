#include "aap_clap_plugin.h"
#include "midi2_event_bridge.h"
#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>
// Instantiate clap-helpers template implementations for our handler/level combination.
#include <clap/helpers/plugin.hxx>
#include <aap/ext/midi.h>
#include <aap/ext/gui.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

// Reserve enough UMP scratch space for one full block of CLAP events.
// 64 CLAP events × 16 bytes (max UMP packet) = 1024 bytes. We use 4096 to be safe.
static constexpr size_t UMP_SCRATCH_INITIAL = 4096;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AapClapPlugin::AapClapPlugin(const clap_plugin_descriptor_t* desc,
                              const clap_host_t*               host,
                              aap::RemotePluginInstance*        instance,
                              aap::PluginClient*                client)
    : Plugin(desc, host)
    , _instance(instance)
    , _client(client)
{
    _ump_scratch.reserve(UMP_SCRATCH_INITIAL);
}

AapClapPlugin::~AapClapPlugin() {
    if (_instance)
        _instance->setBusesChangedHandler({});
    if (_client && _instance) {
        _client->destroyInstance(_instance);
        _instance = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AapClapPlugin::init() noexcept {
    // completeInstantiation() and configurePorts() were already called by
    // PluginClient::createInstance(). Calling them again resets port state.

    _ports = buffer_bridge::build_port_layout(_instance);
    // Invoked on an AAP worker thread; CLAP audio-ports rescan has to happen on the main thread.
    _instance->setBusesChangedHandler([this](uint32_t flags) {
        _pending_bus_changes.fetch_or(flags);
        _host.requestCallback();
    });

    // Invoked on the AAP extension worker after the instance published refreshed parameter
    // metadata; CLAP params rescan has to happen on the main thread.
    _instance->parametersChangedHandler = [this](aap::RemotePluginInstance&) {
        _pending_param_changes.store(true);
        _host.requestCallback();
    };

    _param_cache = buildParameterCache();
    _param_values.clear();
    _param_values.reserve(_param_cache.size());
    for (const auto& p : _param_cache)
        _param_values.push_back(p.default_value);
    return true;
}

bool AapClapPlugin::activate(double sample_rate,
                              uint32_t /*min_frames*/,
                              uint32_t max_frames) noexcept {
    // The instance has to be prepared first, and again after refreshBusLayout() (UNPREPARED).
    // A prepared instance can be prepared again with another configuration only in bus mode.
    bool configChanged = (int32_t)max_frames != _prepared_max_frames ||
                         (int32_t)sample_rate != _prepared_sample_rate;
    auto state = _instance->getInstanceState();
    if (state == aap::PLUGIN_INSTANTIATION_STATE_INACTIVE && configChanged && !_instance->isBusMode()) {
        log(CLAP_LOG_ERROR, "The plugin cannot be activated with another sample rate or block size");
        return false;
    }
    if (state == aap::PLUGIN_INSTANTIATION_STATE_UNPREPARED || configChanged) {
        _instance->prepare((int)max_frames, (int32_t)sample_rate);
        _prepared_max_frames = (int32_t)max_frames;
        _prepared_sample_rate = (int32_t)sample_rate;
    }
    if (_instance->getInstanceState() != aap::PLUGIN_INSTANTIATION_STATE_INACTIVE)
        return false;
    _instance->activate();
    return true;
}

void AapClapPlugin::deactivate() noexcept {
    _instance->deactivate();
    if (_pending_bus_changes.load() || _pending_param_changes.load())
        _host.requestCallback();
}

void AapClapPlugin::onMainThread() noexcept {
    applyParameterChanges();
    applyBusChanges();
}

void AapClapPlugin::applyParameterChanges() {
    if (!_pending_param_changes.exchange(false))
        return;

    auto params = buildParameterCache();
    bool layoutChanged = params.size() != _param_cache.size();
    for (size_t i = 0; !layoutChanged && i < params.size(); i++) {
        const auto& a = params[i];
        const auto& b = _param_cache[i];
        layoutChanged = a.stable_id != b.stable_id || a.min_value != b.min_value ||
                        a.max_value != b.max_value || a.default_value != b.default_value;
    }

    if (!layoutChanged) {
        // Only names changed. The audio thread does not read them, so update them in place.
        bool infoChanged = false;
        for (size_t i = 0; i < params.size(); i++) {
            if (strcmp(params[i].display_name, _param_cache[i].display_name) == 0)
                continue;
            memcpy(_param_cache[i].display_name, params[i].display_name, sizeof(params[i].display_name));
            infoChanged = true;
        }
        if (infoChanged && _host.canUseParams())
            _host.paramsRescan(CLAP_PARAM_RESCAN_INFO);
        return;
    }

    // The parameter list and ranges are read on the audio thread, and CLAP_PARAM_RESCAN_ALL
    // is valid only while deactivated; ask the host to restart us first.
    if (isActive()) {
        _pending_param_changes.store(true);
        _host.requestRestart();
        return;
    }

    // Keep the current values of the parameters that still exist.
    std::vector<double> values;
    values.reserve(params.size());
    for (const auto& p : params) {
        int32_t idx = paramIndexById((clap_id)p.stable_id);
        values.push_back(idx >= 0 ? std::clamp(_param_values[(size_t)idx], p.min_value, p.max_value)
                                  : p.default_value);
    }
    _param_cache = std::move(params);
    _param_values = std::move(values);
    if (_host.canUseParams())
        _host.paramsRescan(CLAP_PARAM_RESCAN_ALL);
}

void AapClapPlugin::applyBusChanges() {
    auto flags = _pending_bus_changes.exchange(0);
    if (!flags)
        return;
    // The ports can change only while deactivated; ask the host to restart us first.
    if (isActive()) {
        _pending_bus_changes.fetch_or(flags);
        if (flags & AAP_BUSES_CHANGED_LAYOUT)
            _host.requestRestart();
        return;
    }
    if (!_instance->refreshBusLayout().empty())
        return;
    _ports = buffer_bridge::build_port_layout(_instance);
    if (_host.canUseAudioPorts())
        _host.audioPortsRescan((flags & AAP_BUSES_CHANGED_LAYOUT) ? CLAP_AUDIO_PORTS_RESCAN_LIST : CLAP_AUDIO_PORTS_RESCAN_NAMES);
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

clap_process_status AapClapPlugin::process(const clap_process_t* p) noexcept {
    aap_buffer_t* aap_buf = _instance->getAudioPluginBuffer();

    buffer_bridge::copy_clap_to_aap(p, aap_buf, _ports.audio_in);

    injectInputEvents(p->in_events);

    _instance->process((int32_t)p->frames_count, /*timeout_ns=*/0);

    buffer_bridge::copy_aap_to_clap(aap_buf, p, _ports.audio_out);

    drainOutputEvents(p->out_events);

    return CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------------------
// Audio ports
// ---------------------------------------------------------------------------

uint32_t AapClapPlugin::audioPortsCount(bool isInput) const noexcept {
    return (uint32_t) (isInput ? _ports.audio_in.size() : _ports.audio_out.size());
}

bool AapClapPlugin::audioPortsInfo(uint32_t index, bool isInput,
                                    clap_audio_port_info* info) const noexcept {
    const auto& buses = isInput ? _ports.audio_in : _ports.audio_out;
    if (index >= buses.size()) return false;

    const auto& bus = buses[index];
    info->id            = bus.id;
    info->flags         = bus.main ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = (uint32_t) bus.port_indices.size();
    info->port_type     = bus.layout == AAP_BUS_LAYOUT_STEREO ? CLAP_PORT_STEREO
                        : bus.layout == AAP_BUS_LAYOUT_MONO ? CLAP_PORT_MONO
                        : nullptr;
    info->in_place_pair = CLAP_INVALID_ID;
    snprintf(info->name, sizeof(info->name), "%s", bus.name.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Note ports
// ---------------------------------------------------------------------------

uint32_t AapClapPlugin::notePortsCount(bool isInput) const noexcept {
    return (isInput ? (_ports.midi_in >= 0) : (_ports.midi_out >= 0)) ? 1 : 0;
}

bool AapClapPlugin::notePortsInfo(uint32_t index, bool isInput,
                                   clap_note_port_info* info) const noexcept {
    if (index != 0) return false;
    if (isInput  && _ports.midi_in  < 0) return false;
    if (!isInput && _ports.midi_out < 0) return false;

    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP
                             | CLAP_NOTE_DIALECT_MIDI
                             | CLAP_NOTE_DIALECT_MIDI2;
    info->preferred_dialect  = CLAP_NOTE_DIALECT_MIDI2;
    snprintf(info->name, sizeof(info->name), "%s", isInput ? "MIDI In" : "MIDI Out");
    return true;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

uint32_t AapClapPlugin::paramsCount() const noexcept {
    return (uint32_t)_param_cache.size();
}

bool AapClapPlugin::paramsInfo(uint32_t paramIndex,
                                clap_param_info* info) const noexcept {
    if (paramIndex >= _param_cache.size()) return false;
    const auto& ap = _param_cache[paramIndex];

    info->id            = (clap_id)ap.stable_id;
    info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
    if (ap.per_note_enabled)
        info->flags |= CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID;
    info->cookie        = nullptr;
    info->min_value     = ap.min_value;
    info->max_value     = ap.max_value;
    info->default_value = ap.default_value;
    snprintf(info->name,   sizeof(info->name),   "%s", ap.display_name);
    snprintf(info->module, sizeof(info->module), "%s", ap.path);
    return true;
}

bool AapClapPlugin::paramsValue(clap_id paramId, double* value) noexcept {
    int32_t idx = paramIndexById(paramId);
    if (idx < 0) return false;
    *value = _param_values[(size_t)idx];
    return true;
}

bool AapClapPlugin::paramsValueToText(clap_id paramId, double value,
                                       char* display, uint32_t size) noexcept {
    if (paramIndexById(paramId) < 0) return false;
    snprintf(display, size, "%.4g", value);
    return true;
}

bool AapClapPlugin::paramsTextToValue(clap_id paramId, const char* display,
                                       double* value) noexcept {
    int32_t idx = paramIndexById(paramId);
    if (idx < 0) return false;
    char* end = nullptr;
    double v = strtod(display, &end);
    if (!end || end == display) return false;
    const auto& ap = _param_cache[idx];
    *value = std::clamp(v, ap.min_value, ap.max_value);
    return true;
}

void AapClapPlugin::paramsFlush(const clap_input_events*  in,
                                 const clap_output_events* out) noexcept {
    // Inject param events into UMP stream; a zero-frame process delivers them.
    if (in) injectInputEvents(in);
    if (out) drainOutputEvents(out);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool AapClapPlugin::implementsState() const noexcept {
    return _instance->getPluginInformation()->hasExtension(AAP_STATE_EXTENSION_URI);
}

bool AapClapPlugin::stateSave(const clap_ostream* stream) noexcept {
    auto& ext = _instance->getStandardExtensions();
    auto size = ext.getStateSize();
    if (!size.isOk()) return false;

    aap_state_t s{nullptr, 0};
    if (size.value > 0) {
        auto result = ext.getState();
        if (!result.isOk()) return false;
        s = result.value;
    }

    // An empty state is still written as a zero length so that stateLoad() can read it back.
    uint32_t u32sz = (uint32_t)s.data_size;
    if (stream->write(stream, &u32sz, 4) != 4) return false;
    if (u32sz == 0) return true;
    int64_t written = stream->write(stream, s.data, (uint64_t)u32sz);
    return written == (int64_t)u32sz;
}

bool AapClapPlugin::stateLoad(const clap_istream* stream) noexcept {
    uint32_t u32sz = 0;
    if (stream->read(stream, &u32sz, 4) != 4) return false;
    if (u32sz == 0) return true;

    std::vector<uint8_t> buf(u32sz);
    int64_t got = stream->read(stream, buf.data(), u32sz);
    if (got != (int64_t)u32sz) return false;

    return _instance->getStandardExtensions().setState(buf.data(), (int32_t)u32sz).isOk();
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------

bool AapClapPlugin::implementsGui() const noexcept {
    return _instance->getPluginInformation()->hasExtension(AAP_GUI_EXTENSION_URI);
}

bool AapClapPlugin::guiIsApiSupported(const char* api, bool isFloating) noexcept {
    return !isFloating && api && strcmp(api, CLAP_WINDOW_API_AAP_GUI) == 0;
}

bool AapClapPlugin::guiGetPreferredApi(const char** api,
                                        bool* is_floating) noexcept {
    *api         = CLAP_WINDOW_API_AAP_GUI;
    *is_floating = false;
    return true;
}

bool AapClapPlugin::guiCreate(const char* api, bool isFloating) noexcept {
    if (!guiIsApiSupported(api, isFloating)) return false;
    _instance->prepareSurfaceControlForRemoteNativeUI();
    _gui_created = true;
    return true;
}

void AapClapPlugin::guiDestroy() noexcept {
    if (!_gui_created) return;
    // No explicit destroy call in RemotePluginInstance; cleanup happens in dtor.
    _gui_created    = false;
    _gui_connected  = false;
    _gui_width      = 0;
    _gui_height     = 0;
}

bool AapClapPlugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept {
    int32_t w = 0, h = 0;
    if (_gui_created && _instance->getRemoteNativeViewPreferredSize(w, h)) {
        *width  = (uint32_t)w;
        *height = (uint32_t)h;
    } else {
        *width  = _gui_width  > 0 ? _gui_width  : 400;
        *height = _gui_height > 0 ? _gui_height : 300;
    }
    return true;
}

bool AapClapPlugin::guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept {
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically   = true;
    hints->preserve_aspect_ratio   = false;
    hints->aspect_ratio_width      = 0;
    hints->aspect_ratio_height     = 0;
    return true;
}

bool AapClapPlugin::guiAdjustSize(uint32_t* width, uint32_t* height) noexcept {
    (void)width; (void)height;
    return true;
}

bool AapClapPlugin::guiSetSize(uint32_t width, uint32_t height) noexcept {
    _gui_width  = width;
    _gui_height = height;
    return true;
}

bool AapClapPlugin::guiSetParent(const clap_window* window) noexcept {
    if (!window || strcmp(window->api, CLAP_WINDOW_API_AAP_GUI) != 0) return false;
    if (!_gui_created) return false;

    // connectRemoteNativeView starts the async SurfaceControlViewHost connection.
    // Call it only once — it must run on any JNI-attached thread but need not be main.
    // getRemoteNativeView() returns a jobject (SurfaceView), which is only safe to
    // hand to the Android UI framework from the main thread.  The caller therefore
    // passes ptr==nullptr here (background thread, connection setup) and calls
    // guiSetParent a second time with ptr!=nullptr from the main thread to retrieve
    // the view handle.  The second call skips connectRemoteNativeView via the guard.
    if (!_gui_connected) {
        int32_t w = (int32_t)_gui_width, h = (int32_t)_gui_height;
        if (w <= 0 || h <= 0)
            _instance->getRemoteNativeViewPreferredSize(w, h);
        if (w <= 0) w = 400;
        if (h <= 0) h = 300;
        _instance->connectRemoteNativeView(w, h);
        _gui_connected = true;
    }

    // Write the SurfaceView handle into the host-provided output slot.
    // Only called when ptr != nullptr, i.e. from the main thread.
    if (window->ptr)
        *static_cast<void**>(window->ptr) = _instance->getRemoteNativeView();
    return true;
}

bool AapClapPlugin::guiShow() noexcept {
    // Visibility is managed by the host embedding/removing the SurfaceView.
    return _gui_created;
}

bool AapClapPlugin::guiHide() noexcept {
    // Visibility is managed by the host embedding/removing the SurfaceView.
    return _gui_created;
}

// ---------------------------------------------------------------------------
// Preset load
// ---------------------------------------------------------------------------

bool AapClapPlugin::implementsPresetLoad() const noexcept {
    return _instance->getPluginInformation()->hasExtension(AAP_PRESETS_EXTENSION_URI);
}

bool AapClapPlugin::presetLoadFromLocation(uint32_t location_kind,
                                            const char* location,
                                            const char* load_key) noexcept {
    if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location != nullptr)
        return false;
    if (!load_key || !*load_key) return false;
    char* end = nullptr;
    long index = strtol(load_key, &end, 10);
    if (!end || end == load_key || *end != '\0' || index < 0) return false;
    auto result = _instance->getStandardExtensions().setCurrentPresetIndex((int32_t)index);
    return result.isOk();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::vector<aap_parameter_info_t> AapClapPlugin::buildParameterCache() {
    // An aap_parameter_info_t shadow cache so that midi2_bridge can normalise
    // parameter values without needing an extension call per event.
    std::vector<aap_parameter_info_t> ret;
    int32_t nparams = _instance->getNumParameters();
    ret.reserve((size_t)std::max(nparams, 0));
    for (int32_t i = 0; i < nparams; i++) {
        const aap::ParameterInformation* pi = _instance->getParameter(i);
        if (!pi) continue;
        aap_parameter_info_t info{};
        info.stable_id     = (int16_t)pi->getId();
        info.min_value     = pi->getMinimumValue();
        info.max_value     = pi->getMaximumValue();
        info.default_value = pi->getDefaultValue();
        strncpy(info.display_name, pi->getName(), AAP_MAX_PARAMETER_NAME_CHARS - 1);
        ret.push_back(info);
    }
    return ret;
}

int32_t AapClapPlugin::paramIndexById(clap_id id) const {
    for (int32_t i = 0; i < (int32_t)_param_cache.size(); i++)
        if ((clap_id)_param_cache[i].stable_id == id) return i;
    return -1;
}

bool AapClapPlugin::updateParamValue(clap_id id, double value) {
    int32_t idx = paramIndexById(id);
    if (idx < 0 || (size_t)idx >= _param_values.size()) return false;
    const auto& p = _param_cache[(size_t)idx];
    double clamped = std::clamp(value, p.min_value, p.max_value);
    _param_values[(size_t)idx] = clamped;
    return true;
}

void AapClapPlugin::injectInputEvents(const clap_input_events* in) {
    if (!in || _ports.midi_in < 0) return;

    // Translate CLAP events → UMP into the persistent scratch buffer.
    // EventList::size/get are the clap-helpers-idiomatic way to iterate input events.
    uint32_t count = in->size(in);
    if (count == 0) return;

    // Ensure scratch has room: at most 16 bytes (one 128-bit UMP) per event.
    size_t needed = (size_t)count * 16;
    if (_ump_scratch.size() < needed)
        _ump_scratch.resize(needed);

    uint8_t* ump     = _ump_scratch.data();
    size_t remaining = _ump_scratch.size();
    uint32_t total   = 0;

    for (uint32_t i = 0; i < count; i++) {
        const clap_event_header_t* ev = in->get(in, i);
        if (!ev) continue;
        size_t written = midi2_bridge::clap_event_to_ump(ev, ump, remaining, _param_cache);
        if (written > 0 && ev->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
            updateParamValue(pv->param_id, pv->value);
        }
        if (written == 0 && remaining < 16) {
            log(CLAP_LOG_WARNING, "UMP scratch buffer too small; some events dropped");
            break;
        }
        ump       += written;
        remaining -= written;
        total     += (uint32_t)written;
    }

    if (total > 0)
        _instance->addEventUmpInput(_ump_scratch.data(), (int32_t)total);
}

void AapClapPlugin::drainOutputEvents(const clap_output_events* out) {
    if (!out || _ports.midi_out < 0) return;

    aap_buffer_t* aap_buf = _instance->getAudioPluginBuffer();
    const auto* raw = static_cast<const uint8_t*>(
            aap_buf->get_buffer(aap_buf, _ports.midi_out));
    if (!raw) {
        log(CLAP_LOG_WARNING, "AAP MIDI2 output port buffer is null; output events dropped");
        return;
    }

    const auto* hdr = reinterpret_cast<const AAPMidiBufferHeader*>(raw);
    if (hdr->length == 0) return;

    const uint8_t* ump = raw + sizeof(AAPMidiBufferHeader);
    const auto* ptr = reinterpret_cast<const uint32_t*>(ump);
    uint32_t remaining = hdr->length / 4;
    while (remaining > 0) {
        uint32_t w0 = ptr[0];
        int wcount = midi2_bridge::ump_word_count(w0);
        if ((uint32_t)wcount > remaining) break;

        if (((w0 >> 28) & 0xF) == 0x5 && wcount >= 4) {
            uint8_t group, channel, key, extra;
            uint16_t param_id;
            uint32_t transport;
            if (aapReadMidi2ParameterSysex8(&group, &channel, &key, &extra,
                                             &param_id, &transport,
                                             ptr[0], ptr[1], ptr[2], ptr[3])) {
                int32_t idx = paramIndexById((clap_id)param_id);
                if (idx >= 0) {
                    const auto& pi = _param_cache[(size_t)idx];
                    double normalized = aapParameterUint32ToNormalized(transport);
                    double plain = aapParameterNormalizedToPlain(
                            pi.min_value, pi.max_value, normalized);
                    updateParamValue((clap_id)param_id, plain);
                }
            }
        }

        ptr += wcount;
        remaining -= wcount;
    }

    midi2_bridge::ump_to_clap_events(ump, hdr->length, out, _param_cache);
}
