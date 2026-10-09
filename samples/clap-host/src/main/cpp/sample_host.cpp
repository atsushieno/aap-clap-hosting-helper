// JNI bridge for the CLAP hosting sample, plus the Oboe audio stream that
// drives it. All CLAP hosting logic (independent of both JNI and Oboe) lives
// in clap_engine.{h,cpp}; this file owns the JNI<->engine translation and the
// one audio backend (Oboe) chosen for this Android sample, calling into the
// engine's audio-thread-only API from the Oboe callback.

#include <jni.h>
#include <android/log.h>
#include <oboe/Oboe.h>
#include <aap-clap-hosting-helper/aap-clap-hosting-helper.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "clap_engine.h"

#define TAG "aap-clap-sample"

namespace {

// ---------------------------------------------------------------------------
// Oboe glue — bridges Oboe's audio callback to the engine's audio-thread-only API
// ---------------------------------------------------------------------------

class OboeCallback : public oboe::AudioStreamCallback {
public:
    std::atomic<bool> shouldStop{false};

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* /*stream*/, void* audioData, int32_t numFrames) override {
        engineMarkAudioThread();

        // Teardown: engineStopProcessing() must run on the audio thread.
        if (shouldStop.load(std::memory_order_acquire)) {
            engineStopProcessing();
            return oboe::DataCallbackResult::Stop;
        }

        if (!engineEnsureProcessingStarted())
            return oboe::DataCallbackResult::Stop;

        engineProcessBlock(static_cast<float*>(audioData), numFrames);
        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) override {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Oboe stream error: %s",
                            oboe::convertToText(error));
    }
};

std::shared_ptr<oboe::AudioStream> s_stream;
std::unique_ptr<OboeCallback>      s_callback;

// Stops and closes the Oboe stream, if open. Must run before engineUnloadPlugin().
void closeOboeStream() {
    if (s_stream) {
        s_callback->shouldStop.store(true, std::memory_order_release);
        s_stream->stop();   // blocks until the callback has returned Stop
        s_stream->close();
        s_stream.reset();
    }
    s_callback.reset();
}

int32_t s_sampleRate{0};
int32_t s_frameSize{0};

// Opens and starts the Oboe stream that drives the loaded plugin.
bool openOboeStream(int32_t sampleRate, int32_t frameSize) {
    s_sampleRate = sampleRate;
    s_frameSize  = frameSize;
    s_callback = std::make_unique<OboeCallback>();

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(2)
           ->setSampleRate(sampleRate)
           ->setFramesPerDataCallback(frameSize)
           ->setDataCallback(s_callback.get())
           ->setErrorCallback(s_callback.get());

    oboe::Result result = builder.openStream(s_stream);
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Oboe openStream failed: %s",
                            oboe::convertToText(result));
        s_callback.reset();
        return false;
    }

    result = s_stream->requestStart();
    if (result != oboe::Result::OK) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Oboe requestStart failed: %s",
                            oboe::convertToText(result));
        s_stream->close();
        s_stream.reset();
        s_callback.reset();
        return false;
    }

    return true;
}

} // namespace

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

    auto plugins = listAvailablePlugins();

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)plugins.size(), str_cls, nullptr);
    for (size_t i = 0; i < plugins.size(); i++) {
        std::string line = std::to_string(plugins[i].entryIdx) + "|"
                          + std::to_string(plugins[i].pluginIdx) + "|"
                          + plugins[i].label;
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(line.c_str()));
    }
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
    return env->NewStringUTF(buildAutomationCatalogJson().c_str());
}

// ---------------------------------------------------------------------------
// nativeLoadPlugin — init + activate plugin, then open and start an Oboe stream
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeLoadPlugin(
        JNIEnv*, jobject, jint entryIdx, jint pluginIdx, jint sampleRate, jint frameSize) {

    if (!engineLoadPlugin((uint32_t)entryIdx, (uint32_t)pluginIdx, sampleRate, frameSize))
        return JNI_FALSE;

    if (!openOboeStream(sampleRate, frameSize)) {
        engineUnloadPlugin();
        return JNI_FALSE;
    }

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
    engineSendNoteOn((int32_t)key);
}

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeSendNoteOff(
        JNIEnv*, jobject, jint key) {
    engineSendNoteOff((int32_t)key);
}

// ---------------------------------------------------------------------------
// nativePluginHasGui / nativeShowGui / nativeHideGui
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativePluginHasGui(
        JNIEnv*, jobject) {
    return engineHasGui() ? JNI_TRUE : JNI_FALSE;
}

// nativeShowGui — AAPXS setup only (safe to call from a background thread).
// Calls guiCreate / guiSetParent / guiShow via Binder IPC and starts the
// async SurfaceControlViewHost connection. The SurfaceView is NOT fetched
// here; use nativeGetGuiView() on the main thread afterwards.
extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeShowGui(
        JNIEnv*, jobject) {
    return engineShowGui() ? JNI_TRUE : JNI_FALSE;
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
    void* surfaceView = engineGetGuiViewHandle();
    if (!surfaceView) return nullptr;
    return env->NewLocalRef(static_cast<jobject>(surfaceView));
}

extern "C"
JNIEXPORT jlongArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetGuiSize(
        JNIEnv* env, jobject) {
    uint32_t w = 0, h = 0;
    engineGetGuiSize(&w, &h);
    jlongArray result = env->NewLongArray(2);
    jlong buf[2] = {(jlong)w, (jlong)h};
    env->SetLongArrayRegion(result, 0, 2, buf);
    return result;
}

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeHideGui(
        JNIEnv*, jobject) {
    engineHideGui();
}

// ---------------------------------------------------------------------------
// nativeUnloadPlugin — stop Oboe, deactivate and destroy plugin
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeUnloadPlugin(
        JNIEnv*, jobject) {
    closeOboeStream();
    engineUnloadPlugin();
}

// ---------------------------------------------------------------------------
// nativeIdle — services main-thread callbacks and restart requests; true if parameters were rescanned
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jboolean JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeIdle(
        JNIEnv*, jobject) {
    bool paramsRescanned = engineIdle();
    if (engineTakeRestartRequest()) {
        closeOboeStream();
        bool restarted = engineRestartPlugin();
        paramsRescanned |= engineIdle();
        if (!restarted || !openOboeStream(s_sampleRate, s_frameSize))
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to restart the plugin");
    }
    return paramsRescanned ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// nativeGetParameters — returns "id|name|min|max|default" per parameter
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetParameters(
        JNIEnv* env, jobject) {
    auto params = engineGetParameters();
    jclass strClass = env->FindClass("java/lang/String");
    auto arr = env->NewObjectArray((jsize)params.size(), strClass, nullptr);
    for (size_t i = 0; i < params.size(); i++) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%u|%s|%.6g|%.6g|%.6g",
                 params[i].id, params[i].name.c_str(),
                 params[i].minValue, params[i].maxValue, params[i].defaultValue);
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(buf));
    }
    return arr;
}

extern "C"
JNIEXPORT jfloatArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetParameterValues(
        JNIEnv* env, jobject) {
    auto values = engineGetParameterValues();
    auto arr = env->NewFloatArray((jsize)values.size());
    if (!values.empty())
        env->SetFloatArrayRegion(arr, 0, (jsize)values.size(), values.data());
    return arr;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetParameterNotificationStats(
        JNIEnv* env, jobject) {
    return env->NewStringUTF(engineGetParameterNotificationStatsString().c_str());
}

extern "C"
JNIEXPORT jdoubleArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeDrainParameterNotifications(
        JNIEnv* env, jobject) {
    auto changes = engineDrainParameterNotifications();
    auto arr = env->NewDoubleArray((jsize)(changes.size() * 2));
    if (changes.empty())
        return arr;

    std::vector<jdouble> flattened;
    flattened.reserve(changes.size() * 2);
    for (const auto& change : changes) {
        flattened.push_back((jdouble)change.first);
        flattened.push_back((jdouble)change.second);
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
    engineSendParameterChange((uint32_t)paramId, value);
}

// ---------------------------------------------------------------------------
// nativeGetPresets — returns "name\tload_key" per preset via CLAP preset discovery
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT jobjectArray JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeGetPresets(
        JNIEnv* env, jobject) {
    auto presets = engineGetPresets();
    jclass strClass = env->FindClass("java/lang/String");
    auto arr = env->NewObjectArray((jsize)presets.size(), strClass, nullptr);
    for (size_t i = 0; i < presets.size(); i++) {
        std::string entry = presets[i].first + "\t" + presets[i].second;
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(entry.c_str()));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// nativeSetPreset — activates a preset by load_key via CLAP_EXT_PRESET_LOAD
// ---------------------------------------------------------------------------

extern "C"
JNIEXPORT void JNICALL
Java_org_androidaudioplugin_claphosting_sample_MainActivity_nativeSetPreset(
        JNIEnv* env, jobject, jstring loadKey) {
    if (!loadKey) return;
    const char* key = env->GetStringUTFChars(loadKey, nullptr);
    if (key) {
        engineSetPreset(key);
        env->ReleaseStringUTFChars(loadKey, key);
    }
}
