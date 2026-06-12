#pragma once

#include <clap/events.h>
#include <aap/ext/parameters.h>
#include <aap/ext/midi.h>
#include <cstdint>
#include <vector>

namespace midi2_bridge {

// UMP packet sizes in words (4 bytes each) by message type nibble.
inline int ump_word_count(uint32_t first_word) {
    switch ((first_word >> 28) & 0xF) {
        case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
        case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
        case 0xB: case 0xC:                                return 3;
        default:                                           return 4;
    }
}

// Convert one CLAP input event to MIDI2 UMP words written into buf.
// Returns the number of bytes written (always a multiple of 4, 0 if skipped).
// buf must have at least 16 bytes available.
// param_cache provides min/max ranges for normalising CLAP_EVENT_PARAM_VALUE.
size_t clap_event_to_ump(const clap_event_header_t* ev,
                          uint8_t* buf,
                          size_t   buf_remaining,
                          const std::vector<aap_parameter_info_t>& param_cache);

// Parse a MIDI2 UMP buffer produced by an AAP plugin and push recognised
// events into the CLAP output event list.
// param_cache provides min/max for de-normalising AAP parameter SysEx8.
void ump_to_clap_events(const uint8_t*                buf,
                         uint32_t                      length,
                         const clap_output_events_t*   out,
                         const std::vector<aap_parameter_info_t>& param_cache);

} // namespace midi2_bridge
