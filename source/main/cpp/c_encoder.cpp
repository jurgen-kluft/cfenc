#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_memory.h"
#include "ccore/c_qsort.h"

#include "cfenc/c_bitstream.h"
#include "cfenc/c_srlen.h"
#include "cfenc/c_encoder.h"

namespace ncore
{
    namespace nfenc
    {
        // 6 output streams:
        // - P2 (2-bit symbols)
        // - P4 (4-bit symbols)
        // - P8 (8-bit symbols)
        // - P16 (16-bit symbols)
        // - Run‑change (1‑bit symbols)
        // - Selector (2‑bit symbols)

        // 1  Build color histogram from RGBA8888 image, as a RGB565 palette counting the colors, then sort by frequency
        //    With the information in the histogram we are able to determine the sizes of the following streams:
        //    - P2 stream
        //    - P4 stream
        //    - P8 stream
        //    - P16 stream
        //    - Selector stream
        // 2. We now have a palette of up to 276 colors (4 in P2, 16 in P4, 256 in P8) with the most frequent colors
        //    Any color that is not in the palete is encoded as SELECTOR_RAW in the selector stream
        // 3. Run change stream size = (width / run_length) * height bits
        // 4. Compare image to previous image to build all the streams

        void init_frame_begin(frame_begin_t& f, u16 img_width, u16 img_height, u16 tile_size)
        {
            f.m_msg_id  = 0x4642;  // 'FB' in ASCII
            f.m_msg_len = 0;

            for (i32 i = 0; i < 4; ++i)
                f.m_ps_rb[i] = 0;

            for (i32 i = 0; i < 2; ++i)
                f.m_span_rb[i] = 0;

            for (i32 i = 0; i < 4; ++i)
                f.m_p2_rb[i] = 0;

            for (i32 i = 0; i < 16; ++i)
                f.m_p4_rb[i] = 0;

            for (i32 i = 0; i < 256; ++i)
                f.m_p8_rb[i] = 0;

            for (i32 i = 0; i < 276; ++i)
                f.m_palette[i] = 0;

            // Set width, height, and run length in the header
            f.m_img_width   = img_width;
            f.m_img_height  = img_height;
            f.m_tile_width  = tile_size;
            f.m_tile_height = tile_size;
        }

        static s8 s_histogram_cmp_fn(const void* a, const void* b, const void* user_data)
        {
            u16 const  ai              = *(const u16*)a;
            u16 const  bi              = *(const u16*)b;
            u32 const* histogram_count = (const u32*)user_data;
            u32 const  ac              = histogram_count[ai];
            u32 const  bc              = histogram_count[bi];
            if (ac > bc)
                return -1;  // Sort in descending order
            if (ac < bc)
                return 1;  // Sort in descending order
            return 0;
        }

        static inline u32 s_units_to_bytes(u32 units, u32 bits_per_unit) { return ((units * bits_per_unit) + 7) >> 3; }
        static inline u16 s_rgba888_to_rgb565(u32 rgba) { return ((rgba >> 8) & 0xf800) | ((rgba >> 5) & 0x07e0) | ((rgba >> 3) & 0x001f); }

        static s32 s_compress(const u8* stream, u32 stream_size_in_bits, u8 symbol_bits, u8* out_stream, u8* out_symbol_rb)
        {
            // size in bytes of the encoded stream, or a negative value on error
            nsrlen::syminfo_t* syminfo_array = nullptr;
            const s32          encoded_size  = nsrlen::analyze_bits(stream, stream_size_in_bits, symbol_bits, out_symbol_rb, syminfo_array);

            nsrlen::out_t out_stream_info;
            out_stream_info.m_data = out_stream;
            out_stream_info.m_size = (stream_size_in_bits + 7) >> 3;  // We use the uncompressed size as the upper bound for the compressed size

            ASSERT(stream_size_in_bits <= (u32)(encoded_size * 8));  // Compression should not increase the size

            const s32 encoded_num_bits = nsrlen::encode_bits(stream, stream_size_in_bits, symbol_bits, out_symbol_rb, out_stream_info);

            ASSERT(encoded_num_bits == encoded_size * 8);  // The encoded size in bits should match the size calculated during analysis

            return encoded_size;
        }

        s32 encode_frame(encoder_t& encoder, frame_begin_t* out_begin, u8* out_data, u32 out_data_capacity, u32 const* current_img, u32 const* previous_img, u16 width, u16 height, u16 tile_size)
        {
            // Initialize histogram and palette
            g_memory_fill(encoder.m_histogram_count, 0, sizeof(encoder.m_histogram_count));

            for (u32 i = 0; i < 65536; ++i)
                encoder.m_histogram_color[i] = (i16)i;  // Initialize histogram color (index)

            // 1. Build color histogram and palette

            for (u32 y = 0; y < height; ++y)
            {
                // u32 const* previous_img_row    = previous_img + y * width;
                u32 const* current_img_row     = current_img + y * width;
                u32 const* current_img_row_end = current_img_row + width;
                while (current_img_row < current_img_row_end)
                {
                    const u16 crgb565 = s_rgba888_to_rgb565(*current_img_row++);
                    encoder.m_histogram_count[crgb565]++;
                }
            }

            // Sort colors by frequency and build palette and histogram index
            nsort::sort(encoder.m_histogram_color, 65536, s_histogram_cmp_fn, (const void*)encoder.m_histogram_count);

            for (u32 i = 0; i < 276; ++i)
            {
                u16 const color         = encoder.m_histogram_color[i];
                out_begin->m_palette[i] = color;
                if (encoder.m_histogram_count[color] == 0)
                    break;  // No more colors in the image
            }

            // For P2, P4 and P8 we can calculate the number of pixels that are used
            const u32 total_pixel_count = width * height;
            u32       p2_pixel_count    = 0;
            u32       p4_pixel_count    = 0;
            u32       p8_pixel_count    = 0;
            for (u32 i = 0; i < 4; ++i)
            {
                u16 const color_index = encoder.m_histogram_color[i];
                u32 const count       = encoder.m_histogram_count[color_index];
                if (count == 0)
                    break;  // No more colors in the image
                p2_pixel_count += count;
            }
            for (u32 i = 4; i < 20; ++i)
            {
                u16 const color_index = encoder.m_histogram_color[i];
                u32 const count       = encoder.m_histogram_count[color_index];
                if (count == 0)
                    break;  // No more colors in the image
                p4_pixel_count += count;
            }
            for (u32 i = 20; i < 276; ++i)
            {
                u16 const color_index = encoder.m_histogram_color[i];
                u32 const count       = encoder.m_histogram_count[color_index];
                if (count == 0)
                    break;  // No more colors in the image
                p8_pixel_count += count;
            }
            const u32 p16_pixel_count = total_pixel_count - (p2_pixel_count + p4_pixel_count + p8_pixel_count);

            // Now we can calculate the size of each stream:
            const u32 p16_stream_size_in_units        = p16_pixel_count;                                 // 16 bits per pixel (raw color)
            const u32 p8_stream_size_in_units         = p8_pixel_count;                                  // 8 bits per pixel
            const u32 p4_stream_size_in_units         = p4_pixel_count;                                  // 4 bits per pixel
            const u32 p2_stream_size_in_units         = p2_pixel_count;                                  // 2 bits per pixel
            const u32 selector_stream_size_in_units   = total_pixel_count;                               // 2 bits per pixel (SELECTOR_RAW)
            const u32 run_change_stream_size_in_units = ((width + tile_size - 1) / tile_size) * height;  // 1 bit per run, rounded up to the nearest byte

            // Which one of the compressing streams is the largest, we will use this to introduce a gap into
            // the output buffer here, so that once we start compressing we can make sure we are not overwriting
            // any data that we still need to read.
            u32 max_stream_size_in_bytes = p16_stream_size_in_units * 2;
            if (p8_stream_size_in_units > max_stream_size_in_bytes)
                max_stream_size_in_bytes = p8_stream_size_in_units;
            if (p4_stream_size_in_units * 4 > max_stream_size_in_bytes)
                max_stream_size_in_bytes = p4_stream_size_in_units * 4;
            if (p2_stream_size_in_units * 2 > max_stream_size_in_bytes)
                max_stream_size_in_bytes = p2_stream_size_in_units * 2;
            if (selector_stream_size_in_units * 2 > max_stream_size_in_bytes)
                max_stream_size_in_bytes = selector_stream_size_in_units * 2;
            if (run_change_stream_size_in_units > max_stream_size_in_bytes)
                max_stream_size_in_bytes = run_change_stream_size_in_units;

            // Setup pointers for each stream, using out_data as a contiguous block of memory for all streams
            u8* p16_stream_ptr = out_data;
            u8* p8_stream_ptr  = p16_stream_ptr + s_units_to_bytes(p16_stream_size_in_units, 16);
            p8_stream_ptr += max_stream_size_in_bytes;
            u8* p4_stream_ptr         = p8_stream_ptr + s_units_to_bytes(p8_stream_size_in_units, 8);
            u8* p2_stream_ptr         = p4_stream_ptr + s_units_to_bytes(p4_stream_size_in_units, 4);
            u8* selector_stream_ptr   = p2_stream_ptr + s_units_to_bytes(p2_stream_size_in_units, 2);
            u8* run_change_stream_ptr = selector_stream_ptr + s_units_to_bytes(selector_stream_size_in_units, 2);

            // Verify output capacity for the worst-case layout.
            u8* const out_data_end = run_change_stream_ptr + s_units_to_bytes(run_change_stream_size_in_units, 1);
            if ((u32)(out_data_end - out_data) > out_data_capacity)
                return -1;

            // Clear all stream buffers so trailing alignment bits are deterministic.
            g_memory_fill(p2_stream_ptr, 0, (u32)(out_data_end - p2_stream_ptr));

            // Build RGB565 -> palette index map: 0..275 for palette entries, -1 for raw.
            for (u32 i = 0; i < 65536; ++i)
                encoder.m_histogram_color[i] = -1;

            for (u32 i = 0; i < 276; ++i)
            {
                if (encoder.m_histogram_count[out_begin->m_palette[i]] == 0)
                    break;
                encoder.m_histogram_color[out_begin->m_palette[i]] = (i16)i;
            }

            nbitstream::writer_t p16_writer;
            nbitstream::writer_t p8_writer;
            nbitstream::writer_t p4_writer;
            nbitstream::writer_t p2_writer;
            nbitstream::writer_t selector_writer;
            nbitstream::writer_t run_change_writer;

            nbitstream::init(&p16_writer, (u8*)p16_stream_ptr, p16_stream_size_in_units * 16);
            nbitstream::init(&p8_writer, p8_stream_ptr, p8_stream_size_in_units * 8);
            nbitstream::init(&p4_writer, p4_stream_ptr, p4_stream_size_in_units * 4);
            nbitstream::init(&p2_writer, p2_stream_ptr, p2_stream_size_in_units * 2);
            nbitstream::init(&selector_writer, selector_stream_ptr, selector_stream_size_in_units * 2);
            nbitstream::init(&run_change_writer, run_change_stream_ptr, run_change_stream_size_in_units * 1);

            // Now that we have everything set up, we can start encoding the image by comparing it to the
            // previous image and filling the streams accordingly.

            const u32 span_count_per_line = (width + tile_size - 1) / tile_size;
            for (u32 y = 0; y < height; ++y)
            {
                u32 const* current_img_row  = current_img + y * width;
                u32 const* previous_img_row = previous_img + y * width;

                for (u32 run = 0; run < span_count_per_line; ++run)
                {
                    const u32 x0 = run * tile_size;
                    const u32 x1 = (x0 + tile_size) < width ? (x0 + tile_size) : width;

                    bool run_changed = false;
                    for (u32 x = x0; x < x1 && !run_changed; ++x)
                    {
                        const u16 crgb565 = s_rgba888_to_rgb565(current_img_row[x]);
                        const u16 prgb565 = s_rgba888_to_rgb565(previous_img_row[x]);
                        run_changed       = (crgb565 != prgb565);
                    }
                    nbitstream::write_bits(&run_change_writer, run_changed ? 1u : 0u, 1);
                    if (!run_changed)
                        continue;

                    for (u32 x = x0; x < x1; ++x)
                    {
                        const u16 crgb565 = s_rgba888_to_rgb565(current_img_row[x]);

                        // Note:
                        // Below we are ignoring the return value of write_bits for performance reasons
                        // We know that the stream have enough capacity because we calculated and reserved
                        // the correct amount of memory for each stream at the start of this function,
                        // and we are filling the streams in a way that matches the calculated sizes.

                        const u16 palette_index = encoder.m_histogram_color[crgb565];
                        if (palette_index < 4)
                        {
                            nbitstream::write_bits(&selector_writer, SELECTOR_P2, 2);
                            nbitstream::write_bits(&p2_writer, (u32)palette_index, 2);
                        }
                        else if (palette_index < 20)
                        {
                            nbitstream::write_bits(&selector_writer, SELECTOR_P4, 2);
                            nbitstream::write_bits(&p4_writer, (u32)(palette_index - 4), 4);
                        }
                        else if (palette_index < 276)
                        {
                            nbitstream::write_bits(&selector_writer, SELECTOR_P8, 2);
                            nbitstream::write_bits(&p8_writer, (u32)(palette_index - 20), 8);
                        }
                        else
                        {
                            nbitstream::write_bits(&selector_writer, SELECTOR_P16, 2);
                            nbitstream::write_bits(&p16_writer, (u32)crgb565, 16);
                        }
                    }
                }
            }

            const u32 p16_units        = nbitstream::finalize(&p16_writer) / 16;
            const u32 p8_units         = nbitstream::finalize(&p8_writer) / 8;
            const u32 p4_units         = nbitstream::finalize(&p4_writer) / 4;
            const u32 p2_units         = nbitstream::finalize(&p2_writer) / 2;
            const u32 selector_units   = nbitstream::finalize(&selector_writer) / 2;
            const u32 run_change_units = nbitstream::finalize(&run_change_writer) / 1;

            ASSERT(p16_units == p16_stream_size_in_units);
            ASSERT(p8_units == p8_stream_size_in_units);
            ASSERT(p4_units == p4_stream_size_in_units);
            ASSERT(p2_units == p2_stream_size_in_units);
            ASSERT(selector_units == selector_stream_size_in_units);
            ASSERT(run_change_units == run_change_stream_size_in_units);

            // Some of the stream we are going to apply SRLE to it and this will result in all of the streams either
            // being the same size or smaller.
            // So we will start to re-compute the location of all the streams one by one.
            u8* p16_stream_ptr_srle         = out_data;
            u32 p16_stream_srle_size        = p16_units;  // p16 already exists here and we are not compressing it
            u8* p8_stream_ptr_srle          = p16_stream_ptr_srle + p16_stream_srle_size;
            u32 p8_stream_srle_size         = s_compress(p8_stream_ptr, p8_units * 8, 8, p8_stream_ptr_srle, out_begin->m_p8_rb);
            u8* p4_stream_ptr_srle          = p8_stream_ptr_srle + p8_stream_srle_size;
            u32 p4_stream_srle_size         = s_compress(p4_stream_ptr, p4_units * 4, 4, p4_stream_ptr_srle, out_begin->m_p4_rb);
            u8* p2_stream_ptr_srle          = p4_stream_ptr_srle + p4_stream_srle_size;
            u32 p2_stream_srle_size         = s_compress(p2_stream_ptr, p2_units * 2, 2, p2_stream_ptr_srle, out_begin->m_p2_rb);
            u8* selector_stream_ptr_srle    = p2_stream_ptr_srle + p2_stream_srle_size;
            u32 selector_stream_srle_size   = s_compress(selector_stream_ptr, selector_units, 2, selector_stream_ptr_srle, out_begin->m_ps_rb);
            u8* run_change_stream_ptr_srle  = selector_stream_ptr_srle + selector_stream_srle_size;
            u32 run_change_stream_srle_size = s_compress(run_change_stream_ptr, run_change_units, 1, run_change_stream_ptr_srle, out_begin->m_span_rb);

            const u32 encoded_size = p16_stream_srle_size + p8_stream_srle_size + p4_stream_srle_size + p2_stream_srle_size + selector_stream_srle_size + run_change_stream_srle_size;
            return encoded_size <= out_data_capacity ? (s32)encoded_size : -1;
        }

    }  // namespace nfenc
}  // namespace ncore
