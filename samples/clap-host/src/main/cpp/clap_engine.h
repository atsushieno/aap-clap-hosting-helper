#pragma once

// ---------------------------------------------------------------------------
// clap_engine: CLAP plugin discovery + hosting logic for the sample host.
//
// This header (and clap_engine.cpp) intentionally has no dependency on JNI
// or on any particular audio backend (Oboe included). It only needs the
// aap-clap-hosting-helper, CLAP, and clap-helpers headers, so a desktop CLAP
// host developer can lift this file into a non-Android project as-is.
//
// Audio I/O is left entirely to the caller: the "audio-thread-only API"
// below is meant to be driven from whatever audio callback the caller's
// backend provides (Oboe, PortAudio, JACK, ...). CLAP requires
// start/stop_processing() and process() to consistently run on one "audio
// thread", so the caller must invoke engineMarkAudioThread() and
// engineProcessBlock() from that same callback thread.
//
// The one JNI-shaped exception is aap_clap_hosting_helper_init(), which on
// Android takes a JNIEnv*/jobject; that call is made by the JNI layer
// (sample_host.cpp) before any of the functions below are used.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One selectable plugin, as discovered from an aap-clap-hosting-helper entry's
// CLAP_PLUGIN_FACTORY_ID factory.
struct PluginListEntry {
    uint32_t    entryIdx;
    uint32_t    pluginIdx;
    std::string label;
};

// Enumerates every plugin exposed by every discovered aap-clap-hosting-helper
// entry. Requires aap_clap_hosting_helper_init() to have already succeeded.
std::vector<PluginListEntry> listAvailablePlugins();

// Builds a JSON array of {pluginId, displayName, packageName} for every
// discovered plugin, for consumption by the automation-catalog JNI bridge.
std::string buildAutomationCatalogJson();

// Creates and activates the plugin at (entryIdx, pluginIdx) at the given
// sample rate / block size. Returns false (and leaves no plugin loaded) on
// any failure. Only one plugin may be loaded at a time. Does not start audio
// processing; the caller opens its own audio stream and drives processing
// via the audio-thread-only API below.
bool engineLoadPlugin(uint32_t entryIdx, uint32_t pluginIdx, int32_t sampleRate, int32_t frameSize);

// Deactivates and destroys the loaded plugin, if any. The caller must have
// already stopped its audio stream and guaranteed no further
// engineProcessBlock() calls before calling this; processing is stopped here
// if the audio callback did not get to call engineStopProcessing().
void engineUnloadPlugin();

bool engineIsLoaded();
bool engineHasGui();

// Must be called periodically on the main thread. Runs clap_plugin::on_main_thread() when the
// plugin requested it, and returns true if the plugin rescanned its parameters since the last
// call (the caller should then re-read engineGetParameters()).
bool engineIdle();

// Returns true (once) if the plugin requested a restart (clap_host::request_restart).
bool engineTakeRestartRequest();

// Must be called on the main thread, after the caller stopped its audio stream (no further
// engineProcessBlock() calls). Stops processing if still started, deactivates the
// plugin, lets it apply its pending changes, re-reads the audio ports and activates it again.
// Returns false if the plugin failed to re-activate.
bool engineRestartPlugin();

// --- Audio-thread-only API --------------------------------------------------
// Must all be invoked from the caller's audio callback thread, consistently
// across the lifetime of a loaded plugin (CLAP's threading model requires a
// single, stable "audio thread" for start/stop_processing() and process()).

// Records the calling thread as the CLAP audio thread. Call this once per
// callback thread lifetime, e.g. on the first audio callback after opening
// the stream.
void engineMarkAudioThread();

// Calls clap_plugin::start_processing() the first time it's needed (a no-op
// on later calls). Returns false if the plugin refused to start.
bool engineEnsureProcessingStarted();

// Calls clap_plugin::stop_processing(). Safe to call even if processing was
// never started.
void engineStopProcessing();

// Drains queued note/parameter-change events, runs one clap_plugin::process()
// block, and writes interleaved stereo float output into `out`. `numFrames`
// is the number of frames the audio backend actually requested; at most the
// engine's configured block size (from engineLoadPlugin) is written.
void engineProcessBlock(float* out, int32_t numFrames);

// Creates the plugin GUI and starts the async SurfaceControlViewHost
// connection (AAP GUI window API). Safe to call from a background thread.
bool engineShowGui();

// Must be called on the main thread. Returns the raw window handle the
// plugin wrote via guiSetParent (an Android jobject under the hood, exposed
// here as void* so this header stays JNI-free); nullptr if unavailable.
void* engineGetGuiViewHandle();

void engineGetGuiSize(uint32_t* width, uint32_t* height);
void engineHideGui();

void engineSendNoteOn(int32_t key);
void engineSendNoteOff(int32_t key);
void engineSendParameterChange(uint32_t paramId, double value);

struct ParamInfoLine {
    uint32_t    id;
    std::string name;
    double      minValue;
    double      maxValue;
    double      defaultValue;
};

std::vector<ParamInfoLine> engineGetParameters();
std::vector<float>         engineGetParameterValues();

// "blocks=.. events=.. paramEvents=.. lastId=.. lastValue=.." summary string.
std::string engineGetParameterNotificationStatsString();

// Drains parameter-value output events collected since the last call, as
// (paramId, value) pairs.
std::vector<std::pair<uint32_t, double>> engineDrainParameterNotifications();

// Preset name/load_key pairs for the currently loaded plugin, discovered via
// CLAP_PRESET_DISCOVERY_FACTORY_ID.
std::vector<std::pair<std::string, std::string>> engineGetPresets();

// Activates a preset by load_key via CLAP_EXT_PRESET_LOAD.
void engineSetPreset(const std::string& loadKey);
