#include "ccore/c_memory.h"
#include "cfenc/c_decoder.h"
#include "cfenc/c_srlen.h"

namespace ncore
{
    namespace nfenc
    {
        static u16 s_span_line_buffer[128];

        void decode_line(frame_begin_t& frame, u16 span_width, u16 spans_per_line, frame_line_t const* line_msg, u16* out_psram_ptr)
        {
            // line data stream decoders
            nsrlen::decoder_t span_decoder;
            nsrlen::decoder_t ps_decoder;
            nsrlen::decoder_t p2_decoder;
            nsrlen::decoder_t p4_decoder;
            nsrlen::decoder_t p8_decoder;

            nsrlen::init(&span_decoder, nullptr);
            nsrlen::init(&ps_decoder, nullptr);
            nsrlen::init(&p2_decoder, nullptr);
            nsrlen::init(&p4_decoder, nullptr);
            nsrlen::init(&p8_decoder, nullptr);

            u16*              p16_data = nullptr;
            u16               p16_pos  = 0;

            u16 const* stream_lengths = (u16 const*)((u8 const*)line_msg + sizeof(frame_line_t));
            u8 const*  data_ptr       = (u8 const*)(stream_lengths + line_msg->m_num_streams);

            i32 i = 0;
            if (line_msg->m_flags & LF_STREAM_P16)
            {
                p16_data = (u16*)data_ptr;
                data_ptr += stream_lengths[i++];
            }
            if (line_msg->m_flags & LF_STREAM_P8)
            {
                p8_decoder.m_stream = data_ptr;
                data_ptr += stream_lengths[i++];
            }
            if (line_msg->m_flags & LF_STREAM_P4)
            {
                p4_decoder.m_stream = data_ptr;
                data_ptr += stream_lengths[i++];
            }
            if (line_msg->m_flags & LF_STREAM_P2)
            {
                p2_decoder.m_stream = data_ptr;
                data_ptr += stream_lengths[i++];
            }
            if (line_msg->m_flags & LF_STREAM_SELECTOR)
            {
                ps_decoder.m_stream = data_ptr;
                data_ptr += stream_lengths[i++];
            }
            if (line_msg->m_flags & LF_STREAM_SPAN)
            {
                span_decoder.m_stream = data_ptr;
                data_ptr += stream_lengths[i++];
            }

            // TODO, track contigues dirty spans and fill the s_span_line_buffer to
            // minimize the number of memcpy calls to PSRAM, as they are expensive.
            for (u16 s = 0; s < spans_per_line; s++)
            {
                const u8 dirty_span = nsrlen::read_symbol(&span_decoder, frame.m_span_rb, 1);
                if (dirty_span == 1)
                {
                    for (u16 x = 0; x < span_width; x++)
                    {
                        const u8 selector = nsrlen::read_symbol(&ps_decoder, frame.m_ps_rb, 2);
                        u16      color;
                        switch (selector)
                        {
                            case SELECTOR_P2: color = frame.m_palette[nsrlen::read_symbol(&p2_decoder, frame.m_p2_rb, 1)]; break;
                            case SELECTOR_P4: color = frame.m_palette[4 + nsrlen::read_symbol(&p4_decoder, frame.m_p4_rb, 2)]; break;
                            case SELECTOR_P8: color = frame.m_palette[20 + nsrlen::read_symbol(&p8_decoder, frame.m_p8_rb, 8)]; break;
                            case SELECTOR_P16: color = p16_data[p16_pos++]; break;
                        }
                        s_span_line_buffer[x] = color;
                    }

                    // memcpy to PSRAM at the correct position for this span
                    const u16 tile_x = s * span_width;
                    g_memcpy(out_psram_ptr + tile_x, s_span_line_buffer, span_width * sizeof(u16));
                }
            }
        }

    }  // namespace nfenc
}  // namespace ncore
