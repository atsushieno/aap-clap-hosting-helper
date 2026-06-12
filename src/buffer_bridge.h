#pragma once

#include <clap/process.h>
#include <aap/android-audio-plugin.h>
#include <aap/core/plugin-information.h>
#include <cstring>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Helpers for copying audio data between CLAP float buffers and the
// aap_buffer_t* owned by a RemotePluginInstance (shared memory).
//
// Port index lists are built once from PluginInformation at init time and
// reused every process() call.
// ---------------------------------------------------------------------------

namespace buffer_bridge {

// Build ordered lists of AAP port indices for audio and MIDI ports.
struct PortLayout {
    std::vector<uint32_t> audio_in;   // AAP port indices, in declaration order
    std::vector<uint32_t> audio_out;
    int32_t midi_in{-1};              // first MIDI/MIDI2 input port, -1 = none
    int32_t midi_out{-1};
};

inline PortLayout build_port_layout(const aap::PluginInformation* info) {
    PortLayout layout;
    int n = info->getNumDeclaredPorts();
    for (int i = 0; i < n; i++) {
        const auto* p = info->getDeclaredPort(i);
        if (p->getContentType() == AAP_CONTENT_TYPE_AUDIO) {
            if (p->getPortDirection() == AAP_PORT_DIRECTION_INPUT)
                layout.audio_in.push_back((uint32_t)i);
            else
                layout.audio_out.push_back((uint32_t)i);
        } else {
            // MIDI or MIDI2
            if (p->getPortDirection() == AAP_PORT_DIRECTION_INPUT && layout.midi_in < 0)
                layout.midi_in = i;
            else if (p->getPortDirection() == AAP_PORT_DIRECTION_OUTPUT && layout.midi_out < 0)
                layout.midi_out = i;
        }
    }
    return layout;
}

// Copy CLAP audio input buses → AAP audio input ports (stereo-pair grouping).
inline void copy_clap_to_aap(const clap_process_t* p,
                               aap_buffer_t* aap,
                               const std::vector<uint32_t>& audio_in) {
    for (uint32_t bus = 0; bus < p->audio_inputs_count; bus++) {
        uint32_t first = bus * 2;
        for (uint32_t ch = 0; ch < p->audio_inputs[bus].channel_count; ch++) {
            uint32_t aap_idx = first + ch;
            if (aap_idx >= audio_in.size()) break;
            void* dst = aap->get_buffer(aap, (int32_t)audio_in[aap_idx]);
            const float* src = p->audio_inputs[bus].data32[ch];
            if (dst && src)
                memcpy(dst, src, p->frames_count * sizeof(float));
        }
    }
}

// Copy AAP audio output ports → CLAP audio output buses.
inline void copy_aap_to_clap(aap_buffer_t* aap,
                               const clap_process_t* p,
                               const std::vector<uint32_t>& audio_out) {
    for (uint32_t bus = 0; bus < p->audio_outputs_count; bus++) {
        uint32_t first = bus * 2;
        for (uint32_t ch = 0; ch < p->audio_outputs[bus].channel_count; ch++) {
            uint32_t aap_idx = first + ch;
            if (aap_idx >= audio_out.size()) break;
            const void* src = aap->get_buffer(aap, (int32_t)audio_out[aap_idx]);
            float* dst = p->audio_outputs[bus].data32[ch];
            if (src && dst)
                memcpy(dst, src, p->frames_count * sizeof(float));
        }
    }
}

} // namespace buffer_bridge
