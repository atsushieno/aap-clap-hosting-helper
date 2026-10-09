#include "clap_engine.h"

#include <android/log.h>
#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>
#include <clap/helpers/host.hh>
#include <clap/helpers/host.hxx>
#include <clap/helpers/plugin-proxy.hh>
#include <clap/helpers/plugin-proxy.hxx>
#include <clap/helpers/event-list.hh>
#include <clap/factory/preset-discovery.h>
#include <clap/ext/preset-load.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#define TAG "aap-clap-sample"

namespace {

// ---------------------------------------------------------------------------
// CLAP host — clap::helpers::Host<> with thread-ID tracking
// ---------------------------------------------------------------------------

using SamplePluginProxy = clap::helpers::PluginProxy<
    clap::helpers::MisbehaviourHandler::Ignore,
    clap::helpers::CheckingLevel::Maximal>;

class SampleCLAPHost final : public clap::helpers::Host<
    clap::helpers::MisbehaviourHandler::Ignore,
    clap::helpers::CheckingLevel::Maximal> {

    std::thread::id _mainThreadId;
    std::thread::id _audioThreadId;
    bool            _audioThreadSet = false;

public:
    SampleCLAPHost()
        : clap::helpers::Host<
              clap::helpers::MisbehaviourHandler::Ignore,
              clap::helpers::CheckingLevel::Maximal>(
              "CLAP Hosting Helper Sample", "AAP Developers", "", "0.1.0")
        , _mainThreadId(std::this_thread::get_id())
    {}

    void markAudioThread() noexcept {
        _audioThreadId = std::this_thread::get_id();
        _audioThreadSet = true;
    }

    // Call before deactivate()/destroy() if the calling thread differs from the
    // thread that called init() (e.g. Android UI thread vs loader thread).
    void updateMainThreadId() noexcept {
        _mainThreadId = std::this_thread::get_id();
    }

    bool threadCheckIsMainThread() const noexcept override {
        return std::this_thread::get_id() == _mainThreadId;
    }
    bool threadCheckIsAudioThread() const noexcept override {
        return _audioThreadSet && std::this_thread::get_id() == _audioThreadId;
    }

    // Set from any thread, serviced by engineIdle() on the main thread.
    std::atomic<bool> callbackRequested{false};
    // Set when the plugin rescanned its parameters, consumed by engineIdle().
    std::atomic<bool> paramsRescanned{false};
    // Set from any thread, consumed by engineTakeRestartRequest().
    std::atomic<bool> restartRequested{false};

protected:
    void requestRestart() noexcept override {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin requested restart");
        restartRequested.store(true);
    }
    void requestProcess()  noexcept override {}
    void requestCallback() noexcept override { callbackRequested.store(true); }

    bool implementsParams() const noexcept override { return true; }
    void paramsRescan(clap_param_rescan_flags flags) noexcept override {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin rescanned parameters (flags=0x%x)", flags);
        paramsRescanned.store(true);
    }

    bool implementsLog() const noexcept override { return true; }
    void logLog(clap_log_severity severity, const char* msg) const noexcept override {
        int prio = severity <= CLAP_LOG_WARNING ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
        __android_log_print(prio, TAG, "[CLAP] %s", msg);
    }
};

// ---------------------------------------------------------------------------
// Per-port audio buffers — owns sample data + float* array for clap_audio_buffer_t
// ---------------------------------------------------------------------------

struct PortBuffers {
    std::vector<std::vector<float>> channels;  // [channel][sample]
    std::vector<float*>             ptrs;      // data32 into channels[]
    clap_audio_buffer_t             buf{};

    void setup(uint32_t numChannels, uint32_t frames) {
        channels.assign(numChannels, std::vector<float>(frames, 0.0f));
        ptrs.resize(numChannels);
        for (uint32_t c = 0; c < numChannels; c++)
            ptrs[c] = channels[c].data();
        buf.data32        = ptrs.data();
        buf.channel_count = numChannels;
        buf.latency       = 0;
        buf.constant_mask = 0;
    }

    void clear() {
        for (auto& ch : channels)
            std::fill(ch.begin(), ch.end(), 0.0f);
    }
};

// ---------------------------------------------------------------------------
// Note-event queue — written by the UI thread, drained by engineProcessBlock()
// on the audio thread
// ---------------------------------------------------------------------------

struct NoteEvent { int32_t key; bool on; };

std::mutex              s_noteEventMu;
std::vector<NoteEvent>  s_noteEventBuf;   // staging buffer (UI thread)
std::vector<NoteEvent>  s_noteEventAudio; // swap buffer   (audio thread)

void pushNoteEvent(int32_t key, bool on) {
    std::lock_guard<std::mutex> lk(s_noteEventMu);
    s_noteEventBuf.push_back({key, on});
}

// Called from audio thread: swap and return reference to drained events.
const std::vector<NoteEvent>& swapNoteEvents() {
    s_noteEventAudio.clear();
    {
        std::unique_lock<std::mutex> lk(s_noteEventMu, std::try_to_lock);
        if (lk.owns_lock())
            std::swap(s_noteEventBuf, s_noteEventAudio);
    }
    return s_noteEventAudio;
}

// ---------------------------------------------------------------------------
// Parameter-change queue — written by the UI thread, drained by
// engineProcessBlock() on the audio thread
// ---------------------------------------------------------------------------

struct ParamChangeEvent { clap_id id; double value; };

std::mutex                    s_paramEventMu;
std::vector<ParamChangeEvent> s_paramEventBuf;
std::vector<ParamChangeEvent> s_paramEventAudio;

struct ParamNotificationStats {
    uint64_t processBlocks = 0;
    uint64_t totalOutputEvents = 0;
    uint64_t parameterValueEvents = 0;
    clap_id lastParamId = CLAP_INVALID_ID;
    double lastValue = 0.0;
};

std::mutex s_paramNotificationStatsMu;
ParamNotificationStats s_paramNotificationStats;
std::mutex s_paramNotificationMu;
std::vector<ParamChangeEvent> s_paramNotificationBuf;

void pushParamEvent(clap_id id, double value) {
    std::lock_guard<std::mutex> lk(s_paramEventMu);
    s_paramEventBuf.push_back({id, value});
}

const std::vector<ParamChangeEvent>& swapParamEvents() {
    s_paramEventAudio.clear();
    {
        std::unique_lock<std::mutex> lk(s_paramEventMu, std::try_to_lock);
        if (lk.owns_lock())
            std::swap(s_paramEventBuf, s_paramEventAudio);
    }
    return s_paramEventAudio;
}

void resetParamNotificationStats() {
    std::lock_guard<std::mutex> lk(s_paramNotificationStatsMu);
    s_paramNotificationStats = {};
    {
        std::lock_guard<std::mutex> qlk(s_paramNotificationMu);
        s_paramNotificationBuf.clear();
    }
}

void collectOutputEventStats(const clap::helpers::EventList& events) {
    std::lock_guard<std::mutex> lk(s_paramNotificationStatsMu);
    s_paramNotificationStats.processBlocks++;
    s_paramNotificationStats.totalOutputEvents += events.size();

    for (size_t i = 0; i < events.size(); i++) {
        const clap_event_header_t* ev = events.get((uint32_t)i);
        if (!ev || ev->type != CLAP_EVENT_PARAM_VALUE)
            continue;

        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
        s_paramNotificationStats.parameterValueEvents++;
        s_paramNotificationStats.lastParamId = pv->param_id;
        s_paramNotificationStats.lastValue = pv->value;
        {
            std::lock_guard<std::mutex> qlk(s_paramNotificationMu);
            s_paramNotificationBuf.push_back({pv->param_id, pv->value});
        }

        if (s_paramNotificationStats.parameterValueEvents <= 16) {
            __android_log_print(ANDROID_LOG_INFO, TAG,
                                "CLAP output parameter notification #%llu: id=%u value=%f",
                                (unsigned long long)s_paramNotificationStats.parameterValueEvents,
                                (uint32_t)pv->param_id,
                                pv->value);
        }
    }
}

// ---------------------------------------------------------------------------
// SampleEngine: owns one loaded CLAP plugin session
// ---------------------------------------------------------------------------

class SampleEngine {
public:
    std::unique_ptr<SampleCLAPHost>    host;
    std::unique_ptr<SamplePluginProxy> plugin;
    int     sampleRate       = 0;
    int     frameSize        = 0;
    int     entryIdx         = -1;   // index into aap_clap_hosting_helper entries
    int64_t steadyTime       = 0;
    bool    processingStarted = false;

    bool            hasGui     = false;
    bool            guiCreated = false;

    std::vector<PortBuffers>         audioIn;
    std::vector<PortBuffers>         audioOut;
    std::vector<clap_audio_buffer_t> clapAudioIn;
    std::vector<clap_audio_buffer_t> clapAudioOut;

    clap::helpers::EventList eventsIn{};
    clap::helpers::EventList eventsOut{};
};

// Global engine instance (at most one loaded plugin at a time).
SampleEngine* s_engine = nullptr;

// The caller's audio stream is already closed, and its callback may have stopped before it
// could call engineStopProcessing(). Since nothing processes anymore, the calling thread takes
// over as the audio thread to stop processing before deactivate().
void ensureProcessingStopped(SampleEngine* engine) {
    if (!engine->processingStarted) return;
    engine->host->markAudioThread();
    engine->plugin->stopProcessing();
    engine->processingStarted = false;
}

// Query audio ports via PluginProxy after init(), allocate sample buffers.
void setupAudioPorts(SamplePluginProxy* proxy, bool isInput,
                      int frameSize,
                      std::vector<PortBuffers>& ports,
                      std::vector<clap_audio_buffer_t>& clapBufs) {
    uint32_t n = proxy->canUseAudioPorts() ? proxy->audioPortsCount(isInput) : 0;
    if (n == 0) {
        ports.resize(1);
        ports[0].setup(2, (uint32_t)frameSize);
    } else {
        ports.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            clap_audio_port_info_t info{};
            uint32_t ch = 2;
            if (proxy->audioPortsGet(i, isInput, &info) && info.channel_count > 0)
                ch = info.channel_count;
            ports[i].setup(ch, (uint32_t)frameSize);
        }
    }
    clapBufs.resize(ports.size());
    for (size_t i = 0; i < ports.size(); i++)
        clapBufs[i] = ports[i].buf;
}

// ---------------------------------------------------------------------------
// Preset discovery plumbing (CLAP_PRESET_DISCOVERY_FACTORY_ID)
// ---------------------------------------------------------------------------

// No-op indexer callbacks for the provider's init() call in engineGetPresets.
bool noop_declare_filetype(const clap_preset_discovery_indexer_t*,
                            const clap_preset_discovery_filetype_t*) { return true; }
bool noop_declare_location(const clap_preset_discovery_indexer_t*,
                            const clap_preset_discovery_location_t*) { return true; }
bool noop_declare_soundpack(const clap_preset_discovery_indexer_t*,
                             const clap_preset_discovery_soundpack_t*) { return true; }
const void* noop_get_extension(const clap_preset_discovery_indexer_t*,
                                const char*) { return nullptr; }

// Minimal metadata receiver for preset discovery: collects (name, load_key) pairs.
struct PresetCollector {
    clap_preset_discovery_metadata_receiver_t base;
    std::vector<std::pair<std::string, std::string>> presets;
};

void pc_on_error(const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {}
bool pc_begin_preset(const clap_preset_discovery_metadata_receiver_t* r,
                      const char* name, const char* load_key) {
    auto* pc = static_cast<PresetCollector*>(r->receiver_data);
    pc->presets.emplace_back(name ? name : "", load_key ? load_key : "");
    return true;
}
void pc_add_plugin_id(const clap_preset_discovery_metadata_receiver_t*,
                       const clap_universal_plugin_id_t*) {}
void pc_set_soundpack_id(const clap_preset_discovery_metadata_receiver_t*,
                          const char*) {}
void pc_set_flags(const clap_preset_discovery_metadata_receiver_t*, uint32_t) {}
void pc_add_creator(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
void pc_set_description(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
void pc_set_timestamps(const clap_preset_discovery_metadata_receiver_t*,
                        clap_timestamp, clap_timestamp) {}
void pc_add_feature(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
void pc_add_extra_info(const clap_preset_discovery_metadata_receiver_t*,
                        const char*, const char*) {}

void appendJsonString(std::string& json, const char* text) {
    json += "\"";
    for (const char* p = text ? text : ""; *p; ++p) {
        if (*p == '"' || *p == '\\') json += '\\';
        json += *p;
    }
    json += "\"";
}

} // namespace

// ---------------------------------------------------------------------------
// Public engine API (see clap_engine.h)
// ---------------------------------------------------------------------------

std::vector<PluginListEntry> listAvailablePlugins() {
    std::vector<PluginListEntry> results;
    uint32_t nentries = aap_clap_hosting_helper_get_entry_count();
    __android_log_print(ANDROID_LOG_INFO, TAG, "%u AAP package(s) found", nentries);

    for (uint32_t i = 0; i < nentries; i++) {
        const aap_clap_hosting_helper_entry_t* entry = aap_clap_hosting_helper_get_entry(i);
        if (!entry) continue;

        auto* factory = static_cast<const clap_plugin_factory_t*>(
                entry->get_factory(entry, CLAP_PLUGIN_FACTORY_ID));
        if (!factory) continue;

        uint32_t nplugins = factory->get_plugin_count(factory);
        for (uint32_t j = 0; j < nplugins; j++) {
            const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, j);
            if (!desc) continue;

            std::string label =
                    (desc->vendor && desc->vendor[0] ? std::string(desc->vendor) + " / " : "")
                    + (desc->name ? desc->name : "(unnamed)")
                    + "  [" + entry->package_name + "]";

            __android_log_print(ANDROID_LOG_INFO, TAG, "  %s", label.c_str());
            results.push_back({i, j, std::move(label)});
        }
    }
    return results;
}

std::string buildAutomationCatalogJson() {
    std::string json = "[";
    bool first = true;
    uint32_t nentries = aap_clap_hosting_helper_get_entry_count();

    for (uint32_t i = 0; i < nentries; i++) {
        const aap_clap_hosting_helper_entry_t* entry = aap_clap_hosting_helper_get_entry(i);
        if (!entry) continue;

        auto* factory = static_cast<const clap_plugin_factory_t*>(
                entry->get_factory(entry, CLAP_PLUGIN_FACTORY_ID));
        if (!factory) continue;

        uint32_t nplugins = factory->get_plugin_count(factory);
        for (uint32_t j = 0; j < nplugins; j++) {
            const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, j);
            if (!desc || !desc->id) continue;

            if (!first) json += ",";
            first = false;
            json += "{\"pluginId\":";
            appendJsonString(json, desc->id);
            json += ",\"displayName\":";
            appendJsonString(json, desc->name);
            json += ",\"packageName\":";
            appendJsonString(json, entry->package_name);
            json += "}";
        }
    }

    json += "]";
    return json;
}

bool engineLoadPlugin(uint32_t entryIdx, uint32_t pluginIdx, int32_t sampleRate, int32_t frameSize) {
    if (s_engine) return false; // already loaded

    uint32_t nentries = aap_clap_hosting_helper_get_entry_count();
    if (entryIdx >= nentries) return false;

    const aap_clap_hosting_helper_entry_t* entry =
            aap_clap_hosting_helper_get_entry(entryIdx);
    if (!entry) return false;

    auto* factory = static_cast<const clap_plugin_factory_t*>(
            entry->get_factory(entry, CLAP_PLUGIN_FACTORY_ID));
    if (!factory || pluginIdx >= factory->get_plugin_count(factory)) return false;

    const clap_plugin_descriptor_t* desc =
            factory->get_plugin_descriptor(factory, pluginIdx);
    if (!desc) return false;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Loading plugin: %s", desc->name ? desc->name : "?");
    resetParamNotificationStats();

    auto engine = std::make_unique<SampleEngine>();
    engine->host      = std::make_unique<SampleCLAPHost>();
    engine->sampleRate = sampleRate;
    engine->frameSize  = frameSize;
    engine->entryIdx  = (int)entryIdx;

    const clap_plugin_t* rawPlugin =
            factory->create_plugin(factory, engine->host->clapHost(), desc->id);
    if (!rawPlugin) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "create_plugin failed");
        return false;
    }

    engine->plugin = std::make_unique<SamplePluginProxy>(*rawPlugin, *engine->host);

    if (!engine->plugin->init()) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "plugin init failed");
        engine->plugin->destroy();
        return false;
    }

    // Check GUI support using the AAP GUI API string.
    engine->hasGui = engine->plugin->canUseGui() &&
                     engine->plugin->guiIsApiSupported(CLAP_WINDOW_API_AAP_GUI, false);
    __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin hasGui=%s", engine->hasGui ? "true" : "false");

    // Query audio port layout via PluginProxy API and allocate sample buffers.
    setupAudioPorts(engine->plugin.get(), true,  frameSize, engine->audioIn,  engine->clapAudioIn);
    setupAudioPorts(engine->plugin.get(), false, frameSize, engine->audioOut, engine->clapAudioOut);

    if (!engine->plugin->activate((double)sampleRate, (uint32_t)frameSize, (uint32_t)frameSize)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "plugin activate failed");
        engine->plugin->deactivate();
        engine->plugin->destroy();
        return false;
    }
    // startProcessing() is deferred to the first engineProcessBlock() call
    // (must run on the audio thread; see engineEnsureProcessingStarted()).

    s_engine = engine.release();
    __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin loaded");
    return true;
}

void engineUnloadPlugin() {
    SampleEngine* engine = s_engine;
    s_engine = nullptr;
    if (!engine) return;

    // Discard any queued events from the previous session.
    { std::lock_guard<std::mutex> lk(s_noteEventMu); s_noteEventBuf.clear(); }
    { std::lock_guard<std::mutex> lk(s_paramEventMu); s_paramEventBuf.clear(); }

    if (engine->plugin) {
        // deactivate()/destroy() must be called from the "main" thread.
        // Our host captured the loader thread ID as main; update it to this
        // thread (Android UI thread) so the CLAP checker agrees.
        engine->host->updateMainThreadId();
        if (engine->guiCreated) {
            try { engine->plugin->guiHide(); } catch (...) {}
            try { engine->plugin->guiDestroy(); } catch (...) {}
            engine->guiCreated = false;
        }
        ensureProcessingStopped(engine);
        engine->plugin->deactivate();
        engine->plugin->destroy();
    }

    delete engine;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin unloaded");
}

bool engineIsLoaded() { return s_engine != nullptr; }

bool engineIdle() {
    SampleEngine* engine = s_engine;
    if (!engine) return false;

    if (engine->host->callbackRequested.exchange(false)) {
        engine->host->updateMainThreadId();
        engine->plugin->onMainThread();
    }
    return engine->host->paramsRescanned.exchange(false);
}

bool engineTakeRestartRequest() {
    SampleEngine* engine = s_engine;
    return engine && engine->host->restartRequested.exchange(false);
}

bool engineRestartPlugin() {
    SampleEngine* engine = s_engine;
    if (!engine) return false;

    engine->host->updateMainThreadId();
    ensureProcessingStopped(engine);
    engine->plugin->deactivate();
    // The plugin applies the changes that needed the restart in its main-thread callback.
    if (engine->host->callbackRequested.exchange(false))
        engine->plugin->onMainThread();

    // The audio ports may have changed as well.
    setupAudioPorts(engine->plugin.get(), true,  engine->frameSize, engine->audioIn,  engine->clapAudioIn);
    setupAudioPorts(engine->plugin.get(), false, engine->frameSize, engine->audioOut, engine->clapAudioOut);

    if (!engine->plugin->activate((double)engine->sampleRate, (uint32_t)engine->frameSize, (uint32_t)engine->frameSize)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "plugin re-activate failed");
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin restarted");
    return true;
}

void engineMarkAudioThread() {
    if (s_engine) s_engine->host->markAudioThread();
}

bool engineEnsureProcessingStarted() {
    SampleEngine* engine = s_engine;
    if (!engine) return false;
    if (engine->processingStarted) return true;

    if (!engine->plugin->startProcessing()) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "startProcessing failed");
        return false;
    }
    engine->processingStarted = true;
    return true;
}

void engineStopProcessing() {
    SampleEngine* engine = s_engine;
    if (!engine || !engine->processingStarted) return;
    engine->plugin->stopProcessing();
    engine->processingStarted = false;
}

void engineProcessBlock(float* out, int32_t numFrames) {
    SampleEngine* engine = s_engine;
    if (!engine) {
        memset(out, 0, sizeof(float) * (size_t)numFrames * 2);
        return;
    }

    // Drain note and parameter events from the UI thread.
    engine->eventsIn.clear();
    for (const auto& pe : swapParamEvents()) {
        clap_event_param_value_t ev{};
        ev.header.size     = sizeof(clap_event_param_value_t);
        ev.header.time     = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags    = 0;
        ev.param_id        = pe.id;
        ev.cookie          = nullptr;
        ev.note_id         = -1;
        ev.port_index      = -1;
        ev.channel         = -1;
        ev.key             = -1;
        ev.value           = pe.value;
        engine->eventsIn.push(reinterpret_cast<const clap_event_header_t*>(&ev));
    }
    for (const auto& ne : swapNoteEvents()) {
        clap_event_note_t ev{};
        ev.header.size     = sizeof(clap_event_note_t);
        ev.header.time     = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = ne.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        ev.header.flags    = 0;
        ev.note_id         = -1;
        ev.port_index      = 0;
        ev.channel         = 0;
        ev.key             = ne.key;
        ev.velocity        = ne.on ? 0.8 : 0.0;
        engine->eventsIn.push(reinterpret_cast<const clap_event_header_t*>(&ev));
    }

    // Clear audio port buffers.
    for (auto& p : engine->audioIn)  p.clear();
    for (auto& p : engine->audioOut) p.clear();
    engine->eventsOut.clear();

    clap_process_t proc{};
    proc.steady_time         = engine->steadyTime;
    proc.frames_count        = (uint32_t)engine->frameSize;
    proc.transport           = nullptr;
    proc.audio_inputs        = engine->clapAudioIn.data();
    proc.audio_inputs_count  = (uint32_t)engine->clapAudioIn.size();
    proc.audio_outputs       = engine->clapAudioOut.data();
    proc.audio_outputs_count = (uint32_t)engine->clapAudioOut.size();
    proc.in_events           = engine->eventsIn.clapInputEvents();
    proc.out_events          = engine->eventsOut.clapOutputEvents();

    engine->plugin->process(&proc);
    collectOutputEventStats(engine->eventsOut);
    engine->steadyTime += engine->frameSize;

    // Write stereo interleaved float output to the caller's buffer.
    // Take first output port, channels 0 and 1 (or duplicate mono).
    if (!engine->audioOut.empty()) {
        const auto& port = engine->audioOut[0];
        const auto& ch0  = port.channels[0];
        const auto& ch1  = port.channels.size() > 1 ? port.channels[1] : port.channels[0];
        for (int i = 0; i < numFrames && i < engine->frameSize; i++) {
            out[i * 2]     = ch0[i];
            out[i * 2 + 1] = ch1[i];
        }
    } else {
        memset(out, 0, sizeof(float) * (size_t)numFrames * 2);
    }
}

bool engineHasGui() { return s_engine && s_engine->hasGui; }

bool engineShowGui() {
    SampleEngine* engine = s_engine;
    if (!engine || !engine->hasGui) return false;

    engine->host->updateMainThreadId();

    if (!engine->guiCreated) {
        if (!engine->plugin->guiCreate(CLAP_WINDOW_API_AAP_GUI, false)) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "guiCreate failed");
            return false;
        }
        engine->guiCreated = true;
    }

    // ptr=nullptr: starts the async SurfaceControlViewHost connection only.
    // The SurfaceView handle is retrieved on the main thread by engineGetGuiViewHandle().
    clap_window_t clapWin{};
    clapWin.api = CLAP_WINDOW_API_AAP_GUI;
    clapWin.ptr = nullptr;

    try {
        if (!engine->plugin->guiSetParent(&clapWin))
            __android_log_print(ANDROID_LOG_WARN, TAG, "guiSetParent returned false");
        if (!engine->plugin->guiShow()) {
            __android_log_print(ANDROID_LOG_WARN, TAG, "guiShow returned false");
            return false;
        }
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GUI show failed: %s", e.what());
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "GUI connection started");
    return true;
}

// MUST be called on the Android main thread. Returns the SurfaceView owned by
// AudioPluginSurfaceControlClient (an Android jobject exposed as void*).
void* engineGetGuiViewHandle() {
    SampleEngine* engine = s_engine;
    if (!engine || !engine->guiCreated) return nullptr;

    // guiSetParent with ptr!=nullptr skips connectRemoteNativeView (already done) and
    // writes the SurfaceView into the provided slot.
    void* surfaceViewSlot = nullptr;
    clap_window_t win{};
    win.api = CLAP_WINDOW_API_AAP_GUI;
    win.ptr = &surfaceViewSlot;
    engine->plugin->guiSetParent(&win);

    __android_log_print(ANDROID_LOG_INFO, TAG, "engineGetGuiViewHandle: SurfaceView=%p", surfaceViewSlot);
    return surfaceViewSlot;
}

void engineGetGuiSize(uint32_t* width, uint32_t* height) {
    *width = 0;
    *height = 0;
    SampleEngine* engine = s_engine;
    if (engine && engine->guiCreated) {
        engine->plugin->guiGetSize(width, height);
        __android_log_print(ANDROID_LOG_INFO, TAG, "engineGetGuiSize: %ux%u", *width, *height);
    }
}

void engineHideGui() {
    SampleEngine* engine = s_engine;
    if (!engine || !engine->guiCreated) return;

    engine->host->updateMainThreadId();
    try { engine->plugin->guiHide(); } catch (...) {}
    try { engine->plugin->guiDestroy(); } catch (...) {}
    engine->guiCreated = false;
    __android_log_print(ANDROID_LOG_INFO, TAG, "GUI hidden and destroyed");
}

void engineSendNoteOn(int32_t key)  { pushNoteEvent(key, true); }
void engineSendNoteOff(int32_t key) { pushNoteEvent(key, false); }
void engineSendParameterChange(uint32_t paramId, double value) { pushParamEvent((clap_id)paramId, value); }

std::vector<ParamInfoLine> engineGetParameters() {
    std::vector<ParamInfoLine> result;
    SampleEngine* engine = s_engine;
    if (!engine || !engine->plugin->canUseParams()) return result;

    uint32_t count = engine->plugin->paramsCount();
    result.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        clap_param_info_t info{};
        if (!engine->plugin->paramsGetInfo(i, &info)) continue;
        result.push_back({(uint32_t)info.id, info.name, info.min_value, info.max_value, info.default_value});
    }
    return result;
}

std::vector<float> engineGetParameterValues() {
    SampleEngine* engine = s_engine;
    if (!engine || !engine->plugin->canUseParams()) return {};

    engine->host->updateMainThreadId();

    uint32_t count = engine->plugin->paramsCount();
    std::vector<float> values(count, 0.0f);
    for (uint32_t i = 0; i < count; i++) {
        clap_param_info_t info{};
        double value = 0.0;
        if (engine->plugin->paramsGetInfo(i, &info) &&
            engine->plugin->paramsGetValue(info.id, &value))
            values[i] = (float)value;
    }
    return values;
}

std::string engineGetParameterNotificationStatsString() {
    ParamNotificationStats stats;
    {
        std::lock_guard<std::mutex> lk(s_paramNotificationStatsMu);
        stats = s_paramNotificationStats;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "blocks=%llu events=%llu paramEvents=%llu lastId=%u lastValue=%.9g",
             (unsigned long long)stats.processBlocks,
             (unsigned long long)stats.totalOutputEvents,
             (unsigned long long)stats.parameterValueEvents,
             stats.lastParamId == CLAP_INVALID_ID ? 0xFFFFFFFFu : (uint32_t)stats.lastParamId,
             stats.lastValue);
    return buf;
}

std::vector<std::pair<uint32_t, double>> engineDrainParameterNotifications() {
    std::vector<ParamChangeEvent> changes;
    {
        std::lock_guard<std::mutex> lk(s_paramNotificationMu);
        changes.swap(s_paramNotificationBuf);
    }

    std::vector<std::pair<uint32_t, double>> result;
    result.reserve(changes.size());
    for (const auto& change : changes)
        result.emplace_back((uint32_t)change.id, change.value);
    return result;
}

std::vector<std::pair<std::string, std::string>> engineGetPresets() {
    SampleEngine* engine = s_engine;
    if (!engine || engine->entryIdx < 0) return {};

    const aap_clap_hosting_helper_entry_t* entry =
            aap_clap_hosting_helper_get_entry((uint32_t)engine->entryIdx);
    if (!entry) return {};

    auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
            entry->get_factory(entry, CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory) return {};

    clap_preset_discovery_indexer_t indexer{};
    indexer.clap_version      = CLAP_VERSION_INIT;
    indexer.name              = "aap-clap-sample";
    indexer.declare_filetype  = noop_declare_filetype;
    indexer.declare_location  = noop_declare_location;
    indexer.declare_soundpack = noop_declare_soundpack;
    indexer.get_extension     = noop_get_extension;

    PresetCollector collector{};
    collector.base.receiver_data   = &collector;
    collector.base.on_error        = pc_on_error;
    collector.base.begin_preset    = pc_begin_preset;
    collector.base.add_plugin_id   = pc_add_plugin_id;
    collector.base.set_soundpack_id = pc_set_soundpack_id;
    collector.base.set_flags       = pc_set_flags;
    collector.base.add_creator     = pc_add_creator;
    collector.base.set_description = pc_set_description;
    collector.base.set_timestamps  = pc_set_timestamps;
    collector.base.add_feature     = pc_add_feature;
    collector.base.add_extra_info  = pc_add_extra_info;

    uint32_t n = factory->count(factory);
    for (uint32_t i = 0; i < n; i++) {
        const clap_preset_discovery_provider_descriptor_t* desc = factory->get_descriptor(factory, i);
        if (!desc) continue;

        const clap_preset_discovery_provider_t* provider =
                factory->create(factory, &indexer, desc->id);
        if (!provider) continue;

        provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN,
                               nullptr, &collector.base);
        provider->destroy(provider);
    }

    return collector.presets;
}

void engineSetPreset(const std::string& loadKey) {
    SampleEngine* engine = s_engine;
    if (!engine) return;

    engine->host->updateMainThreadId();

    const clap_plugin_t* rawPlugin = engine->plugin->clapPlugin();
    if (!rawPlugin) return;

    auto* ext = static_cast<const clap_plugin_preset_load_t*>(
            rawPlugin->get_extension(rawPlugin, CLAP_EXT_PRESET_LOAD));
    if (!ext || !ext->from_location) return;

    ext->from_location(rawPlugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, loadKey.c_str());
}
