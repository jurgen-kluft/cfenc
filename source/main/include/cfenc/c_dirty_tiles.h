#ifndef __CFENC_DIRTY_TILES_H__
#define __CFENC_DIRTY_TILES_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "cfenc/c_srlen.h"

namespace ncore
{
    namespace nfenc
    {
        // Grid of bits indicating which tiles are dirty, 1 bit per tile, row-major order, padded to
        // 32 bits per row (tiles_stride), with the first bit of the first byte corresponding to tile (0,0)
        struct dirty_tiles_t
        {
            u8   m_tile_width_shift;   // Width of the tile in pixels (4 = 16 pixels, 5 = 32 pixels, etc.)
            u8   m_tile_height_shift;  // Height of the tile in pixels (4 = 16 pixels, 5 = 32 pixels, etc.)
            u8   m_tiles_stride;       // Number of words per row of tiles (for tile line memory alignment)
            u8   m_reserved;           // alignment to 4 bytes
            u16  m_tiles_cols;         // Number of tile columns
            u16  m_tiles_rows;         // Number of tile rows
            u32* m_tiles_data;         //
        };

        void clear(dirty_tiles_t& tiles);

        static inline u16 img_x_to_tile_x(dirty_tiles_t const& tiles, u16 img_x) { return img_x >> tiles.m_tile_width_shift; }
        static inline u16 img_y_to_tile_y(dirty_tiles_t const& tiles, u16 img_y) { return img_y >> tiles.m_tile_height_shift; }

        static inline u32* get_tile_row_ptr(const dirty_tiles_t& tiles, u16 tile_y) { return tiles.m_tiles_data + tile_y * tiles.m_tiles_stride; }
        static inline void mark_tile_on_row(dirty_tiles_t& tiles, u32* tile_row, u16 tile_x) { tile_row[tile_x >> 5] |= (1 << (tile_x & 31)); }
        static inline bool is_tile_marked_on_row(const dirty_tiles_t& tiles, const u32* tile_row, u16 tile_x) { return (tile_row[tile_x >> 5] & (1 << (tile_x & 31))) != 0; }

        static inline void mark_dirty(dirty_tiles_t& tiles, u16 tile_x, u16 tile_y)
        {
            u32* tile_row = get_tile_row_ptr(tiles, tile_y);
            mark_tile_on_row(tiles, tile_row, tile_x);
        }

        // Example of iterating over all dirty tiles:
        // Copy/Paste this function to iterate over all tiles and check if they are dirty
        static inline void iteration_example(dirty_tiles_t& tiles)
        {
            for (u16 y = 0; y < tiles.m_tiles_rows; y++)
            {
                const u32* row = get_tile_row_ptr(tiles, y);
                for (u16 x = 0; x < tiles.m_tiles_cols; x++)
                {
                    if (is_tile_marked_on_row(tiles, row, x))
                    {
                        const u16 img_x = x << tiles.m_tile_width_shift;   // Convert tile_x to img_x
                        const u16 img_y = y << tiles.m_tile_height_shift;  // Convert tile_y to img_y

                        //
                        // Process the tile at (x, y)
                        //
                    }
                }
            }
        }

    }  // namespace nfenc
}  // namespace ncore

#endif  // __CFENC_DIRTY_TILES_H__
