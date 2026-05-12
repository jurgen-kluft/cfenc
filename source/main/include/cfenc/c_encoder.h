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
        struct encoder_t
        {
            u16 m_histogram_color[65536];  // RGB565 histogram, the color or index
            u32 m_histogram_count[65536];  // RGB565 histogram, the frequency of the color in the image
        };

        s32 encode_frame(encoder_t& encoder, frame_begin_t* out_begin, u8* out_data, u32 out_data_capacity, u32 const* current_img, u32 const* previous_img, u16 width, u16 height, u16 tile_size = 16);

    }  // namespace nframe
}  // namespace ncore

#endif  // __CFENC_FRAME_ENCODER_H__
