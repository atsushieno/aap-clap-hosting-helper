#pragma once

#include <clap/process.h>
#include <aap/android-audio-plugin.h>
#include <aap/core/host/plugin-instance.h>
#include <string>
#include <cstring>
#include <vector>
#include <cstdint>

// Helpers for copying audio data between CLAP float buffers and the aap_buffer_t* of a
// RemotePluginInstance. A CLAP audio port corresponds to an AAP audio bus; the bus layout
// is built once at init time and reused every process() call.

namespace buffer_bridge {

struct AudioBus {
    uint32_t id{0};
    bool main{false};
    std::string name{};
    std::string layout{};               // AAP bus layout tag e.g. "mono", "stereo"
    std::vector<int32_t> port_indices;  // AAP port index of each channel
};

struct PortLayout {
    std::vector<AudioBus> audio_in;
    std::vector<AudioBus> audio_out;
    int32_t midi_in{-1};                // main event bus port, -1 = none
    int32_t midi_out{-1};
};

inline std::vector<AudioBus> build_audio_buses(aap::PluginInstance* instance, aap_port_direction direction) {
    std::vector<AudioBus> ret;
    for (int32_t b = 0, n = instance->getNumBuses(AAP_BUS_KIND_AUDIO, direction); b < n; b++) {
        auto bus = instance->getBus(AAP_BUS_KIND_AUDIO, direction, b);
        AudioBus entry{bus->getId(), bus->getRole() == AAP_BUS_ROLE_MAIN, bus->getName(), bus->getLayout(), {}};
        for (int32_t ch = 0, nc = bus->getChannelCount(); ch < nc; ch++)
            entry.port_indices.push_back(bus->getPortIndex(ch));
        ret.push_back(std::move(entry));
    }
    return ret;
}

inline PortLayout build_port_layout(aap::PluginInstance* instance) {
    PortLayout layout;
    layout.audio_in = build_audio_buses(instance, AAP_PORT_DIRECTION_INPUT);
    layout.audio_out = build_audio_buses(instance, AAP_PORT_DIRECTION_OUTPUT);
    layout.midi_in = instance->getMainEventPortIndex(AAP_PORT_DIRECTION_INPUT);
    layout.midi_out = instance->getMainEventPortIndex(AAP_PORT_DIRECTION_OUTPUT);
    return layout;
}

// Copy CLAP audio input ports → AAP audio input buses.
inline void copy_clap_to_aap(const clap_process_t* p,
                               aap_buffer_t* aap,
                               const std::vector<AudioBus>& audio_in) {
    for (uint32_t bus = 0; bus < p->audio_inputs_count && bus < audio_in.size(); bus++) {
        const auto& ports = audio_in[bus].port_indices;
        for (uint32_t ch = 0; ch < p->audio_inputs[bus].channel_count && ch < ports.size(); ch++) {
            void* dst = aap->get_buffer(aap, ports[ch]);
            const float* src = p->audio_inputs[bus].data32[ch];
            if (dst && src)
                memcpy(dst, src, p->frames_count * sizeof(float));
        }
    }
}

// Copy AAP audio output buses → CLAP audio output ports.
inline void copy_aap_to_clap(aap_buffer_t* aap,
                               const clap_process_t* p,
                               const std::vector<AudioBus>& audio_out) {
    for (uint32_t bus = 0; bus < p->audio_outputs_count && bus < audio_out.size(); bus++) {
        const auto& ports = audio_out[bus].port_indices;
        for (uint32_t ch = 0; ch < p->audio_outputs[bus].channel_count && ch < ports.size(); ch++) {
            const void* src = aap->get_buffer(aap, ports[ch]);
            float* dst = p->audio_outputs[bus].data32[ch];
            if (src && dst)
                memcpy(dst, src, p->frames_count * sizeof(float));
        }
    }
}

} // namespace buffer_bridge
