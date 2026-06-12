#include "midi2_event_bridge.h"
#include <clap/events.h>
#include <cstring>
#include <cstdio>

namespace midi2_bridge {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void write_words(uint8_t* buf, const uint32_t* words, int count) {
    memcpy(buf, words, (size_t)count * sizeof(uint32_t));
}

static const aap_parameter_info_t* find_param(
        const std::vector<aap_parameter_info_t>& cache, clap_id id) {
    for (auto& p : cache)
        if ((clap_id)p.stable_id == id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// CLAP event → MIDI2 UMP
// ---------------------------------------------------------------------------

size_t clap_event_to_ump(const clap_event_header_t*               ev,
                          uint8_t*                                  buf,
                          size_t                                    remaining,
                          const std::vector<aap_parameter_info_t>& param_cache) {
    if (!ev || !buf) return 0;

    switch (ev->type) {

    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF: {
        if (remaining < 8) return 0;
        const auto* n = reinterpret_cast<const clap_event_note_t*>(ev);
        uint8_t status = (ev->type == CLAP_EVENT_NOTE_ON) ? 0x9u : 0x8u;
        uint8_t ch     = (n->channel >= 0) ? (uint8_t)(n->channel & 0xF) : 0;
        uint8_t key    = (uint8_t)(n->key & 0x7F);
        // MIDI 2.0 velocity: 0.0-1.0 → 0-0xFFFF
        double vel_norm = (ev->type == CLAP_EVENT_NOTE_ON) ? n->velocity : 0.0;
        uint16_t vel16  = (uint16_t)(vel_norm * 65535.0);

        uint32_t words[2];
        words[0] = (4u << 28) | (status << 20) | ((uint32_t)ch << 16)
                 | ((uint32_t)key << 8);
        words[1] = ((uint32_t)vel16 << 16);
        write_words(buf, words, 2);
        return 8;
    }

    case CLAP_EVENT_PARAM_VALUE: {
        if (remaining < 16) return 0;
        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
        const aap_parameter_info_t* pi = find_param(param_cache, pv->param_id);
        if (!pi) return 0;

        double normalized = aapParameterPlainToNormalized(
                pi->min_value, pi->max_value, pv->value);
        uint32_t transport = aapParameterNormalizedToUint32(normalized);

        uint32_t words[4];
        aapMidi2ParameterSysex8(&words[0], &words[1], &words[2], &words[3],
                                 /*group=*/0, /*channel=*/0, /*key=*/0,
                                 /*extra=*/0,
                                 (uint16_t)(pi->stable_id & 0x3FFF),
                                 transport);
        write_words(buf, words, 4);
        return 16;
    }

    case CLAP_EVENT_MIDI: {
        // Convert MIDI 1.0 bytes to a MIDI 1.0 Channel Voice UMP (type 2, 1 word).
        if (remaining < 4) return 0;
        const auto* m = reinterpret_cast<const clap_event_midi_t*>(ev);
        uint32_t word = (2u << 28)
                      | ((uint32_t)m->data[0] << 16)
                      | ((uint32_t)m->data[1] << 8)
                      | (uint32_t)m->data[2];
        write_words(buf, &word, 1);
        return 4;
    }

    case CLAP_EVENT_MIDI2: {
        // Pass MIDI 2.0 UMP through as-is.
        const auto* m   = reinterpret_cast<const clap_event_midi2_t*>(ev);
        const uint32_t* w = reinterpret_cast<const uint32_t*>(m->data);
        int wcount = ump_word_count(w[0]);
        size_t bytes = (size_t)wcount * 4;
        if (remaining < bytes) return 0;
        write_words(buf, w, wcount);
        return bytes;
    }

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// MIDI2 UMP → CLAP events
// ---------------------------------------------------------------------------

void ump_to_clap_events(const uint8_t*                           buf,
                         uint32_t                                 length,
                         const clap_output_events_t*              out,
                         const std::vector<aap_parameter_info_t>& param_cache) {
    if (!buf || !out || length == 0) return;

    const uint32_t* ptr = reinterpret_cast<const uint32_t*>(buf);
    uint32_t remaining  = length / 4; // in words

    while (remaining > 0) {
        uint32_t w0    = ptr[0];
        int wcount     = ump_word_count(w0);
        if ((uint32_t)wcount > remaining) break;

        uint8_t msg_type = (w0 >> 28) & 0xF;

        if (msg_type == 0x4 && wcount >= 2) {
            // MIDI 2.0 Channel Voice Message
            uint8_t status  = (w0 >> 20) & 0xF;
            uint8_t channel = (w0 >> 16) & 0xF;
            uint8_t note    = (w0 >>  8) & 0x7F;
            uint32_t w1     = ptr[1];
            uint16_t vel16  = (uint16_t)(w1 >> 16);

            if (status == 0x9 || status == 0x8) {
                clap_event_note_t ev{};
                ev.header.size     = sizeof(ev);
                ev.header.time     = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type     = (status == 0x9) ? CLAP_EVENT_NOTE_ON
                                                      : CLAP_EVENT_NOTE_OFF;
                ev.header.flags    = 0;
                ev.note_id  = -1;
                ev.port_index = 0;
                ev.channel  = channel;
                ev.key      = note;
                ev.velocity = vel16 / 65535.0;
                out->try_push(out, &ev.header);
            } else {
                // Other MIDI2 channel voice: forward as CLAP_EVENT_MIDI2
                clap_event_midi2_t ev{};
                ev.header.size     = sizeof(ev);
                ev.header.time     = 0;
                ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                ev.header.type     = CLAP_EVENT_MIDI2;
                ev.header.flags    = 0;
                ev.port_index = 0;
                memcpy(ev.data, ptr, 8);
                out->try_push(out, &ev.header);
            }

        } else if (msg_type == 0x5 && wcount >= 4) {
            // SysEx8 — check for AAP parameter change format
            uint8_t group, channel, key, extra;
            uint16_t param_idx;
            uint32_t transport;
            if (aapReadMidi2ParameterSysex8(&group, &channel, &key, &extra,
                                             &param_idx, &transport,
                                             ptr[0], ptr[1], ptr[2], ptr[3])) {
                // Find the parameter by stable_id to get min/max
                const aap_parameter_info_t* pi = nullptr;
                for (auto& p : param_cache)
                    if (p.stable_id == (int16_t)param_idx) { pi = &p; break; }

                if (pi) {
                    double normalized = aapParameterUint32ToNormalized(transport);
                    double plain      = aapParameterNormalizedToPlain(
                            pi->min_value, pi->max_value, normalized);

                    clap_event_param_value_t ev{};
                    ev.header.size     = sizeof(ev);
                    ev.header.time     = 0;
                    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    ev.header.type     = CLAP_EVENT_PARAM_VALUE;
                    ev.header.flags    = 0;
                    ev.param_id = (clap_id)pi->stable_id;
                    ev.cookie   = nullptr;
                    ev.note_id  = -1;
                    ev.port_index = -1;
                    ev.channel  = -1;
                    ev.key      = -1;
                    ev.value    = plain;
                    out->try_push(out, &ev.header);
                }
            }
        }

        ptr       += wcount;
        remaining -= wcount;
    }
}

} // namespace midi2_bridge
