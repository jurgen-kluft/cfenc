#ifndef __CFENC_FRAME_ENCODER_H__
#define __CFENC_FRAME_ENCODER_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "cfenc/c_codec.h"

namespace ncore
{
    namespace nfenc
    {
        static const i32 c_max_pixels_per_line = 2048;
        static const i32 c_max_spans_per_line  = 128;

        // A histogram of the colors in the image, used for palette building and encoding
        struct histogram_t
        {
            struct item_t
            {
                i32 m_color;      // RGB565 color
                u32 m_count;      // Frequency of the color in the image
            };
            item_t m_items[65536];  // RGB565 histogram, the color or index
        };

        struct encoder_t
        {
            histogram_t m_histogram; // storage usage = 65536 * 8 bytes = 512 KiB
            
            // Some streams here might be totally oversized, but organizing them in this manner makes
            // the encoder logic simpler. (storage usage ~= 2 * 48 KiB = 96 KiB)
            u16 m_streams[STREAM_COUNT][c_max_pixels_per_line];
            u16 m_enc_streams[STREAM_COUNT][c_max_pixels_per_line];
        };

        // the size in bytes of out_mem_capacity should be roughly the size of an image * 1.5, so if your
        // image is 1024x1024, then out_mem_capacity should be ~ 3 MiB (1024 * 1024 * sizeof(u16) * 1.5) = 3 MiB)
        s32 encode_frame(encoder_t& encoder, frame_begin_t* out_begin, u8* out_mem, u32 out_mem_capacity, u32 const* current_img, u32 const* previous_img, u16 width, u16 height, u16 tile_size = 16);

    }  // namespace nfenc
}  // namespace ncore

#endif  // __CFENC_FRAME_ENCODER_H__
