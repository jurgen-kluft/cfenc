#include "ccore/c_target.h"
#include "ccore/c_memory.h"

#include "cfenc/c_dirty_tiles.h"

namespace ncore
{
    namespace nfenc
    {
        // Grid of bits indicating which tiles are dirty, 1 bit per tile, row-major order, padded to
        // 32 bits per row (tiles_stride), with the first bit of the first byte corresponding to tile (0,0)
        void clear(dirty_tiles_t& tiles)
        {
            if (tiles.m_tiles_data != nullptr)
            {
                u32 const row_words = tiles.m_tiles_stride;
                for (u16 y = 0; y < tiles.m_tiles_rows; y++)
                {
                    u32* row = tiles.m_tiles_data + y * row_words;
                    for (u16 x = 0; x < row_words; x++)
                        row[x] = 0;
                }
            }
        }


    }  // namespace nfenc
}  // namespace ncore
