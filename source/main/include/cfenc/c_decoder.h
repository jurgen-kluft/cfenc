#ifndef __CFENC_FRAME_DECODER_H__
#define __CFENC_FRAME_DECODER_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
    #pragma once
#endif

#include "cfenc/c_codec.h"
#include "cfenc/c_dirty_tiles.h"

namespace ncore
{
    namespace nfenc
    {
        void update_tiles(frame_begin_t& frame, frame_line_t const* line_msg, dirty_tiles_t& dirty_tiles);
        void decode_line(frame_begin_t& frame, u16 span_width, u16 spans_per_line, frame_line_t const* line_msg, u16* out_psram_ptr);

    }  // namespace nfenc
}  // namespace ncore

#endif  // __CFENC_FRAME_DECODER_H__
