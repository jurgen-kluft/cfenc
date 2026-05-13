#ifndef __CFENC_FRAME_ENCODER_H__
#define __CFENC_FRAME_ENCODER_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "ccore/c_arena.h"
#include "cfenc/c_codec.h"

namespace ncore
{
    namespace nfenc
    {
        // Encoded Frame Structure
        struct encoded_frame_t
        {
            frame_begin_t* m_frame_begin;
            frame_line_t*  m_line_data;
            frame_line_t** m_lines;
            u32            m_frame_begin_size;
            u32            m_line_data_size;
            u32            m_num_lines;
        };

        // All data that is returned is allocated from the arena, and the caller is responsible for freeing the arena after use. 
        // The returned encoded_frame_t structure itself is also allocated from the arena as well as all the data it points to, 
        // so the caller should not free any of the data in the encoded_frame_t structure individually, but should free the entire 
        // arena after use.
        encoded_frame_t* encode_frame(arena_t* arena, u32 const* current_img, u32 const* previous_img, u16 width, u16 height, u16 tile_size = 16);

    }  // namespace nfenc
}  // namespace ncore

#endif  // __CFENC_FRAME_ENCODER_H__
