#include <jni.h>
#include <android/log.h>
#include <oboe/Oboe.h>
#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>
#include <clap/helpers/host.hh>
#include <clap/helpers/host.hxx>
#include <clap/helpers/plugin-proxy.hh>
#include <clap/helpers/plugin-proxy.hxx>
#include <clap/helpers/event-list.hh>
#include <clap/factory/preset-discovery.h>
#include <clap/ext/preset-load.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstring>

#define TAG "aap-clap-sample"

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

protected:
    void requestRestart()  noexcept override {}
    void requestProcess()  noexcept override {}
    void requestCallback() noexcept override {}

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
// Note-event queue — written by JNI (UI thread), drained by Oboe callback
// ---------------------------------------------------------------------------

struct NoteEvent { int32_t key; bool on; };

static std::mutex              s_noteEventMu;
static std::vector<NoteEvent>  s_noteEventBuf;   // staging buffer (UI thread)
static std::vector<NoteEvent>  s_noteEventAudio; // swap buffer   (audio thread)

static void pushNoteEvent(int32_t key, bool on) {
    std::lock_guard<std::mutex> lk(s_noteEventMu);
    s_noteEventBuf.push_back({key, on});
}

// Called from audio thread: swap and return reference to drained events.
static const std::vector<NoteEvent>& swapNoteEvents() {
    s_noteEventAudio.clear();
    {
        std::unique_lock<std::mutex> lk(s_noteEventMu, std::try_to_lock);
        if (lk.owns_lock())
            std::swap(s_noteEventBuf, s_noteEventAudio);
    }
    return s_noteEventAudio;
}

// ---------------------------------------------------------------------------
// Parameter-change queue — written by JNI (UI thread), drained by Oboe callback
// ---------------------------------------------------------------------------

struct ParamChangeEvent { clap_id id; double value; };

static std::mutex                    s_paramEventMu;
static std::vector<ParamChangeEvent> s_paramEventBuf;
static std::vector<ParamChangeEvent> s_paramEventAudio;

struct ParamNotificationStats {
    uint64_t processBlocks = 0;
    uint64_t totalOutputEvents = 0;
    uint64_t parameterValueEvents = 0;
    clap_id lastParamId = CLAP_INVALID_ID;
    double lastValue = 0.0;
};

static std::mutex s_paramNotificationStatsMu;
static ParamNotificationStats s_paramNotificationStats;
static std::mutex s_paramNotificationMu;
static std::vector<ParamChangeEvent> s_paramNotificationBuf;

static void pushParamEvent(clap_id id, double value) {
    std::lock_guard<std::mutex> lk(s_paramEventMu);
    s_paramEventBuf.push_back({id, value});
}

static const std::vector<ParamChangeEvent>& swapParamEvents() {
    s_paramEventAudio.clear();
    {
        std::unique_lock<std::mutex> lk(s_paramEventMu, std::try_to_lock);
        if (lk.owns_lock())
            std::swap(s_paramEventBuf, s_paramEventAudio);
    }
    return s_paramEventAudio;
}

static void resetParamNotificationStats() {
    std::lock_guard<std::mutex> lk(s_paramNotificationStatsMu);
    s_paramNotificationStats = {};
    {
        std::lock_guard<std::mutex> qlk(s_paramNotificationMu);
        s_paramNotificationBuf.clear();
    }
}

static void collectOutputEventStats(const clap::helpers::EventList& events) {
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
// SampleEngine: owns CLAP plugin session and implements Oboe callback
// ---------------------------------------------------------------------------

class SampleEngine : public oboe::AudioStreamCallback {
public:
    std::unique_ptr<SampleCLAPHost>    host;
    std::unique_ptr<SamplePluginProxy> plugin;
    int     frameSize        = 0;
    int     entryIdx         = -1;   // index into aap_clap_hosting_helper entries
    int64_t steadyTime       = 0;
    bool    processingStarted = false;
    std::atomic<bool> shouldStop{false};

    bool            hasGui     = false;
    bool            guiCreated = false;

    std::vector<PortBuffers>         audioIn;
    std::vector<PortBuffers>         audioOut;
    std::vector<clap_audio_buffer_t> clapAudioIn;
    std::vector<clap_audio_buffer_t> clapAudioOut;

    clap::helpers::EventList eventsIn{};
    clap::helpers::EventList eventsOut{};

    std::shared_ptr<oboe::AudioStream> stream;

    // ---------------------------------------------------------------------------
    // Oboe callback — runs on Oboe audio thread
    // ---------------------------------------------------------------------------
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* /*stream*/, void* audioData, int32_t numFrames) override {

        host->markAudioThread();

        // Teardown: stopProcessing() must be called from the audio thread.
        if (shouldStop.load(std::memory_order_acquire)) {
            if (processingStarted) {
                plugin->stopProcessing();
                processingStarted = false;
            }
            return oboe::DataCallbackResult::Stop;
        }

        if (!processingStarted) {
            if (!plugin->startProcessing()) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "startProcessing failed");
                return oboe::DataCallbackResult::Stop;
            }
            processingStarted = true;
        }

        // Drain note and parameter events from the UI thread.
        eventsIn.clear();
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
            eventsIn.push(reinterpret_cast<const clap_event_header_t*>(&ev));
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
            eventsIn.push(reinterpret_cast<const clap_event_header_t*>(&ev));
        }

        // Clear audio port buffers.
        for (auto& p : audioIn)  p.clear();
        for (auto& p : audioOut) p.clear();
        eventsOut.clear();

        clap_process_t proc{};
        proc.steady_time         = steadyTime;
        proc.frames_count        = (uint32_t)frameSize;
        proc.transport           = nullptr;
        proc.audio_inputs        = clapAudioIn.data();
        proc.audio_inputs_count  = (uint32_t)clapAudioIn.size();
        proc.audio_outputs       = clapAudioOut.data();
        proc.audio_outputs_count = (uint32_t)clapAudioOut.size();
        proc.in_events           = eventsIn.clapInputEvents();
        proc.out_events          = eventsOut.clapOutputEvents();

        plugin->process(&proc);
        collectOutputEventStats(eventsOut);
        steadyTime += frameSize;

        // Write stereo interleaved float output to Oboe buffer.
        // Take first output port, channels 0 and 1 (or duplicate mono).
        auto* out = static_cast<float*>(audioData);
        if (!audioOut.empty()) {
            const auto& port = audioOut[0];
            const auto& ch0  = port.channels[0];
            const auto& ch1  = port.channels.size() > 1 ? port.channels[1] : port.channels[0];
            for (int i = 0; i < numFrames && i < frameSize; i++) {
                out[i * 2]     = ch0[i];
                out[i * 2 + 1] = ch1[i];
            }
        } else {
            memset(out, 0, sizeof(float) * (size_t)numFrames * 2);
        }

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) override {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Oboe stream error: %s",
                            oboe::convertToText(error));
    }
};

// Global engine instance (at most one loaded plugin at a time).
static SampleEngine* s_engine = nullptr;

// Query audio ports via PluginProxy after init(), allocate sample buffers.
static void setupAudioPorts(SamplePluginProxy* proxy, bool isInput,
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
// discoverPlugins
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_discoverPlugins(
        JNIEnv* env, jobject, jobject context) {

    if (!aap_clap_hosting_helper_init(env, context)) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "init failed");
        return env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
    }

    std::vector<std::string> results;
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

            results.push_back(std::to_string(i) + "|" + std::to_string(j) + "|" + label);
            __android_log_print(ANDROID_LOG_INFO, TAG, "  %s", label.c_str());
        }
    }

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)results.size(), str_cls, nullptr);
    for (size_t i = 0; i < results.size(); i++)
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(results[i].c_str()));
    return arr;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetAutomationClientHandle(
        JNIEnv*, jobject) {
    return (jlong)aap_clap_hosting_helper_get_native_client_handle();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeConnectPluginService(
        JNIEnv* env, jobject, jstring packageName) {
    if (!packageName) return JNI_FALSE;
    const char* pkg = env->GetStringUTFChars(packageName, nullptr);
    bool ok = pkg && aap_clap_hosting_helper_connect_plugin_service(pkg);
    if (pkg) env->ReleaseStringUTFChars(packageName, pkg);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetAutomationCatalogJson(
        JNIEnv* env, jobject) {
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

            auto appendJsonString = [&](const char* text) {
                json += "\"";
                for (const char* p = text ? text : ""; *p; ++p) {
                    if (*p == '"' || *p == '\\') json += '\\';
                    json += *p;
                }
                json += "\"";
            };

            if (!first) json += ",";
            first = false;
            json += "{\"pluginId\":";
            appendJsonString(desc->id);
            json += ",\"displayName\":";
            appendJsonString(desc->name);
            json += ",\"packageName\":";
            appendJsonString(entry->package_name);
            json += "}";
        }
    }

    json += "]";
    return env->NewStringUTF(json.c_str());
}

// ---------------------------------------------------------------------------
// nativeLoadPlugin — init + activate plugin, start Oboe stream
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeLoadPlugin(
        JNIEnv*, jobject, jint entryIdx, jint pluginIdx, jint sampleRate, jint frameSize) {

    if (s_engine) return JNI_FALSE; // already loaded

    uint32_t nentries = aap_clap_hosting_helper_get_entry_count();
    if ((uint32_t)entryIdx >= nentries) return JNI_FALSE;

    const aap_clap_hosting_helper_entry_t* entry =
            aap_clap_hosting_helper_get_entry((uint32_t)entryIdx);
    if (!entry) return JNI_FALSE;

    auto* factory = static_cast<const clap_plugin_factory_t*>(
            entry->get_factory(entry, CLAP_PLUGIN_FACTORY_ID));
    if (!factory || (uint32_t)pluginIdx >= factory->get_plugin_count(factory)) return JNI_FALSE;

    const clap_plugin_descriptor_t* desc =
            factory->get_plugin_descriptor(factory, (uint32_t)pluginIdx);
    if (!desc) return JNI_FALSE;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Loading plugin: %s", desc->name ? desc->name : "?");
    resetParamNotificationStats();

    auto engine = std::make_unique<SampleEngine>();
    engine->host      = std::make_unique<SampleCLAPHost>();
    engine->frameSize = (int)frameSize;
    engine->entryIdx  = (int)entryIdx;

    const clap_plugin_t* rawPlugin =
            factory->create_plugin(factory, engine->host->clapHost(), desc->id);
    if (!rawPlugin) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "create_plugin failed");
        return JNI_FALSE;
    }

    engine->plugin = std::make_unique<SamplePluginProxy>(*rawPlugin, *engine->host);

    if (!engine->plugin->init()) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "plugin init failed");
        engine->plugin->destroy();
        return JNI_FALSE;
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
        return JNI_FALSE;
    }
    // startProcessing() is deferred to the first Oboe callback (audio thread).

    // Open and start Oboe output stream.
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(2)
           ->setSampleRate((int32_t)sampleRate)
           ->setFramesPerDataCallback((int32_t)frameSize)
           ->setDataCallback(engine.get())
           ->setErrorCallback(engine.get());

    oboe::Result result = builder.openStream(engine->stream);
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Oboe openStream failed: %s",
                            oboe::convertToText(result));
        engine->plugin->deactivate();
        engine->plugin->destroy();
        return JNI_FALSE;
    }

    result = engine->stream->requestStart();
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Oboe requestStart failed: %s",
                            oboe::convertToText(result));
        engine->stream->close();
        engine->plugin->deactivate();
        engine->plugin->destroy();
        return JNI_FALSE;
    }

    s_engine = engine.release();
    __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin loaded and Oboe stream started");
    return JNI_TRUE;
}

// ---------------------------------------------------------------------------
// nativeSendNoteOn / nativeSendNoteOff — called from UI thread
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeSendNoteOn(
        JNIEnv*, jobject, jint key) {
    pushNoteEvent((int32_t)key, true);
}

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeSendNoteOff(
        JNIEnv*, jobject, jint key) {
    pushNoteEvent((int32_t)key, false);
}

// ---------------------------------------------------------------------------
// nativePluginHasGui / nativeShowGui / nativeHideGui
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativePluginHasGui(
        JNIEnv*, jobject) {
    return s_engine && s_engine->hasGui ? JNI_TRUE : JNI_FALSE;
}

// nativeShowGui — AAPXS setup only (safe to call from a background thread).
// Calls guiCreate / guiSetParent / guiShow via Binder IPC and starts the
// async SurfaceControlViewHost connection.  The SurfaceView is NOT fetched
// here; use nativeGetGuiView() on the main thread afterwards.
extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeShowGui(
        JNIEnv*, jobject) {

    SampleEngine* engine = s_engine;
    if (!engine || !engine->hasGui) return JNI_FALSE;

    engine->host->updateMainThreadId();

    if (!engine->guiCreated) {
        if (!engine->plugin->guiCreate(CLAP_WINDOW_API_AAP_GUI, false)) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "guiCreate failed");
            return JNI_FALSE;
        }
        engine->guiCreated = true;
    }

    // ptr=nullptr: starts the async SurfaceControlViewHost connection only.
    // The SurfaceView handle is retrieved on the main thread by nativeGetGuiView().
    clap_window_t clapWin{};
    clapWin.api = CLAP_WINDOW_API_AAP_GUI;
    clapWin.ptr = nullptr;

    try {
        if (!engine->plugin->guiSetParent(&clapWin))
            __android_log_print(ANDROID_LOG_WARN, TAG, "guiSetParent returned false");
        if (!engine->plugin->guiShow()) {
            __android_log_print(ANDROID_LOG_WARN, TAG, "guiShow returned false");
            return JNI_FALSE;
        }
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "GUI show failed: %s", e.what());
        return JNI_FALSE;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "GUI connection started");
    return JNI_TRUE;
}

// nativeGetGuiView — MUST be called on the Android main thread.
// Returns the SurfaceView owned by AudioPluginSurfaceControlClient.
// Calling on the main thread ensures the View is created on the correct thread.
// The host must embed this view; the SurfaceControlViewHost connection
// (started by nativeShowGui) completes once the view is attached to a window.
extern "C"
JNIEXPORT jobject JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetGuiView(
        JNIEnv* env, jobject) {

    SampleEngine* engine = s_engine;
    if (!engine || !engine->guiCreated) return nullptr;

    // Called from the main thread — safe to retrieve the Android SurfaceView (jobject).
    // guiSetParent with ptr!=nullptr skips connectRemoteNativeView (already done) and
    // writes the SurfaceView into the provided slot.
    void* surfaceViewSlot = nullptr;
    clap_window_t win{};
    win.api = CLAP_WINDOW_API_AAP_GUI;
    win.ptr = &surfaceViewSlot;
    engine->plugin->guiSetParent(&win);

    __android_log_print(ANDROID_LOG_INFO, TAG, "nativeGetGuiView: SurfaceView=%p", surfaceViewSlot);
    if (!surfaceViewSlot) return nullptr;
    return env->NewLocalRef(static_cast<jobject>(surfaceViewSlot));
}

extern "C"
JNIEXPORT jlongArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetGuiSize(
        JNIEnv* env, jobject) {
    jlongArray result = env->NewLongArray(2);
    SampleEngine* engine = s_engine;
    if (engine && engine->guiCreated) {
        uint32_t w = 0, h = 0;
        engine->plugin->guiGetSize(&w, &h);
        jlong buf[2] = {(jlong)w, (jlong)h};
        env->SetLongArrayRegion(result, 0, 2, buf);
        __android_log_print(ANDROID_LOG_INFO, TAG, "nativeGetGuiSize: %ux%u", w, h);
    }
    return result;
}

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeHideGui(
        JNIEnv*, jobject) {

    SampleEngine* engine = s_engine;
    if (!engine || !engine->guiCreated) return;

    engine->host->updateMainThreadId();
    try { engine->plugin->guiHide(); } catch (...) {}
    try { engine->plugin->guiDestroy(); } catch (...) {}
    engine->guiCreated = false;
    __android_log_print(ANDROID_LOG_INFO, TAG, "GUI hidden and destroyed");
}

// ---------------------------------------------------------------------------
// nativeUnloadPlugin — stop Oboe, deactivate and destroy plugin
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeUnloadPlugin(
        JNIEnv*, jobject) {

    SampleEngine* engine = s_engine;
    s_engine = nullptr;
    if (!engine) return;

    // Discard any queued events from the previous session.
    { std::lock_guard<std::mutex> lk(s_noteEventMu); s_noteEventBuf.clear(); }
    { std::lock_guard<std::mutex> lk(s_paramEventMu); s_paramEventBuf.clear(); }

    if (engine->stream) {
        // Signal onAudioReady to call stopProcessing() from the audio thread,
        // then let Oboe stop the stream naturally before we close it.
        engine->shouldStop.store(true, std::memory_order_release);
        engine->stream->stop();   // blocks until the callback has returned Stop
        engine->stream->close();
        engine->stream.reset();
    }

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
        engine->plugin->deactivate();
        engine->plugin->destroy();
    }

    delete engine;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Plugin unloaded");
}

// ---------------------------------------------------------------------------
// nativeGetParameters — returns "id|name|min|max|default" per parameter
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetParameters(
        JNIEnv* env, jobject) {

    jclass strClass = env->FindClass("java/lang/String");
    SampleEngine* engine = s_engine;
    if (!engine || !engine->plugin->canUseParams()) {
        return env->NewObjectArray(0, strClass, nullptr);
    }

    uint32_t count = engine->plugin->paramsCount();
    auto arr = env->NewObjectArray((jsize)count, strClass, nullptr);
    for (uint32_t i = 0; i < count; i++) {
        clap_param_info_t info{};
        if (!engine->plugin->paramsGetInfo(i, &info)) continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "%u|%s|%.6g|%.6g|%.6g",
                 (uint32_t)info.id, info.name,
                 info.min_value, info.max_value, info.default_value);
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(buf));
    }
    return arr;
}

extern "C"
JNIEXPORT jfloatArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetParameterValues(
        JNIEnv* env, jobject) {

    SampleEngine* engine = s_engine;
    if (!engine || !engine->plugin->canUseParams())
        return env->NewFloatArray(0);

    engine->host->updateMainThreadId();

    uint32_t count = engine->plugin->paramsCount();
    auto arr = env->NewFloatArray((jsize)count);
    std::vector<jfloat> values(count);
    for (uint32_t i = 0; i < count; i++) {
        clap_param_info_t info{};
        double value = 0.0;
        if (engine->plugin->paramsGetInfo(i, &info) &&
            engine->plugin->paramsGetValue(info.id, &value))
            values[i] = (jfloat)value;
    }
    if (!values.empty())
        env->SetFloatArrayRegion(arr, 0, (jsize)values.size(), values.data());
    return arr;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetParameterNotificationStats(
        JNIEnv* env, jobject) {
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
    return env->NewStringUTF(buf);
}

extern "C"
JNIEXPORT jdoubleArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeDrainParameterNotifications(
        JNIEnv* env, jobject) {
    std::vector<ParamChangeEvent> changes;
    {
        std::lock_guard<std::mutex> lk(s_paramNotificationMu);
        changes.swap(s_paramNotificationBuf);
    }

    auto arr = env->NewDoubleArray((jsize)(changes.size() * 2));
    if (changes.empty())
        return arr;

    std::vector<jdouble> flattened;
    flattened.reserve(changes.size() * 2);
    for (const auto& change : changes) {
        flattened.push_back((jdouble)change.id);
        flattened.push_back((jdouble)change.value);
    }
    env->SetDoubleArrayRegion(arr, 0, (jsize)flattened.size(), flattened.data());
    return arr;
}

// ---------------------------------------------------------------------------
// nativeSendParameterChange — queues a CLAP_EVENT_PARAM_VALUE for audio thread
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeSendParameterChange(
        JNIEnv*, jobject, jint paramId, jdouble value) {
    pushParamEvent((clap_id)paramId, value);
}

// ---------------------------------------------------------------------------
// nativeGetPresets — returns "name\tload_key" per preset via CLAP preset discovery
// ---------------------------------------------------------------------------

// No-op indexer callbacks for the provider's init() call in nativeGetPresets.
static bool noop_declare_filetype(const clap_preset_discovery_indexer_t*,
                                  const clap_preset_discovery_filetype_t*) { return true; }
static bool noop_declare_location(const clap_preset_discovery_indexer_t*,
                                  const clap_preset_discovery_location_t*) { return true; }
static bool noop_declare_soundpack(const clap_preset_discovery_indexer_t*,
                                   const clap_preset_discovery_soundpack_t*) { return true; }
static const void* noop_get_extension(const clap_preset_discovery_indexer_t*,
                                      const char*) { return nullptr; }

// Minimal metadata receiver for preset discovery: collects "name\tload_key" strings.
struct PresetCollector {
    clap_preset_discovery_metadata_receiver_t base;
    std::vector<std::string> presets;
};

static void pc_on_error(const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {}
static bool pc_begin_preset(const clap_preset_discovery_metadata_receiver_t* r,
                             const char* name, const char* load_key) {
    auto* pc = static_cast<PresetCollector*>(r->receiver_data);
    std::string entry = (name ? name : "");
    entry += '\t';
    entry += (load_key ? load_key : "");
    pc->presets.push_back(entry);
    return true;
}
static void pc_add_plugin_id(const clap_preset_discovery_metadata_receiver_t*,
                              const clap_universal_plugin_id_t*) {}
static void pc_set_soundpack_id(const clap_preset_discovery_metadata_receiver_t*,
                                 const char*) {}
static void pc_set_flags(const clap_preset_discovery_metadata_receiver_t*, uint32_t) {}
static void pc_add_creator(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void pc_set_description(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void pc_set_timestamps(const clap_preset_discovery_metadata_receiver_t*,
                               clap_timestamp, clap_timestamp) {}
static void pc_add_feature(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void pc_add_extra_info(const clap_preset_discovery_metadata_receiver_t*,
                               const char*, const char*) {}

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetPresets(
        JNIEnv* env, jobject) {

    jclass strClass = env->FindClass("java/lang/String");
    SampleEngine* engine = s_engine;
    if (!engine || engine->entryIdx < 0) {
        return env->NewObjectArray(0, strClass, nullptr);
    }

    const aap_clap_hosting_helper_entry_t* entry =
            aap_clap_hosting_helper_get_entry((uint32_t)engine->entryIdx);
    if (!entry) return env->NewObjectArray(0, strClass, nullptr);

    auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
            entry->get_factory(entry, CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory) return env->NewObjectArray(0, strClass, nullptr);

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

    jobjectArray arr = env->NewObjectArray((jsize)collector.presets.size(), strClass, nullptr);
    for (size_t i = 0; i < collector.presets.size(); i++)
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(collector.presets[i].c_str()));
    return arr;
}

// ---------------------------------------------------------------------------
// nativeSetPreset — activates a preset by load_key via CLAP_EXT_PRESET_LOAD
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeSetPreset(
        JNIEnv* env, jobject, jstring loadKey) {
    SampleEngine* engine = s_engine;
    if (!engine || !loadKey) return;

    engine->host->updateMainThreadId();

    const clap_plugin_t* rawPlugin = engine->plugin->clapPlugin();
    if (!rawPlugin) return;

    auto* ext = static_cast<const clap_plugin_preset_load_t*>(
            rawPlugin->get_extension(rawPlugin, CLAP_EXT_PRESET_LOAD));
    if (!ext || !ext->from_location) return;

    const char* key = env->GetStringUTFChars(loadKey, nullptr);
    if (key) {
        ext->from_location(rawPlugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key);
        env->ReleaseStringUTFChars(loadKey, key);
    }
}
