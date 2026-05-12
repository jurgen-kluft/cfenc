#ifndef __CFENC_FRAME_DECODER_H__
#define __CFENC_FRAME_DECODER_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
    #pragma once
#endif

namespace ncore
{
    namespace nfenc
    {
        // Encoded Frame Header Structure
        struct frame_begin_t
        {
            u16 m_msg_id;       // 'FB' in ASCII
            u16 m_msg_len;      // Total length of this message, including this header, in bytes
            u16 m_img_width;    // Width of the image
            u16 m_img_height;   // Height of the image
            u8  m_tile_width;   // Width of the image (span is the same size as tile width)
            u8  m_tile_height;  // Height of the image
            u16 m_tile_stride;  // Number of bytes per row of tiles (for tile line memory alignment)

            u16 m_palette[276];  // RGB565 palette (4 colors for P2, 16 colors for P4, 256 colors for P8)
            u8  m_p8_rb[256];    // SRLE run-bits for P8 stream
            u8  m_p4_rb[16];     // SRLE run-bits for P4 stream
            u8  m_p2_rb[4];      // SRLE run-bits for P2 stream
            u8  m_ps_rb[4];      // SRLE run-bits for pixel selector stream
            u8  m_span_rb[2];    // SRLE run-bits for span change stream
            // u8 m_tile_change_data[];  // the (non-encoded) tile change data
        };

        enum selector_e
        {
            SELECTOR_P2  = 0,
            SELECTOR_P4  = 1,
            SELECTOR_P8  = 3,
            SELECTOR_P16 = 2
        };

        struct frame_end_t
        {
            u16 m_msg_id;   // 'FE' in ASCII
            u16 m_msg_len;  // Total length of the encoded frame data, including this header, in bytes
        };

        enum frame_line_flags_e
        {
            LF_STREAM_P16      = 1,
            LF_STREAM_P8       = 2,
            LF_STREAM_P4       = 4,
            LF_STREAM_P2       = 8,
            LF_STREAM_SELECTOR = 16,
            LF_STREAM_SPAN     = 32,
        };

        struct frame_line_t
        {
            u16 m_msg_id;       // 'FL' in ASCII
            u16 m_msg_len;      // Total length of the encoded frame data, including this header, in bytes
            u16 m_index;        // line index in the image (y)
            u8  m_flags;        // flags(line_flags_e)
            u8  m_num_streams;  // number of streams included in this line, which is the number of bits set in m_flags
            // u16 m_stream_len[]; // depending on the flags, the length of each stream data in bytes, in the order of span, selector, p2, p4, p8, p16
            // u8  m_stream_data[]; // data stream for this line, the order of each stream data is span, selector, p2, p4, p8, p16, and the length of each stream data is determined by the flags and the corresponding m_stream_len value
            // alignment to 2 bytes
        };

        void decode_line(frame_begin_t& frame, u16 span_width, u16 spans_per_line, frame_line_t const* line_msg, u16* out_psram_ptr);

    }  // namespace nmui
}  // namespace ncore

#endif  // __CFENC_FRAME_DECODER_H__
