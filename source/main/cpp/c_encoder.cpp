#include "ccore/c_target.h"
#include "ccore/c_math.h"
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

        // A histogram of the colors in the image, used for palette building and encoding
        struct histogram_t
        {
            struct item_t
            {
                i32 m_color;  // RGB565 color
                u32 m_count;  // Frequency of the color in the image
            };
            item_t m_items[65536];  // RGB565 histogram, the color or index
        };

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

            // Set width, height, and span length in the header
            f.m_img_width   = img_width;
            f.m_img_height  = img_height;
            f.m_tile_width  = tile_size;
            f.m_tile_height = tile_size;
        }

        static s8 s_histogram_cmp_fn(const void* a, const void* b, const void* user_data)
        {
            u16 const                  ai    = *(const u16*)a;
            u16 const                  bi    = *(const u16*)b;
            histogram_t::item_t const* items = (const histogram_t::item_t*)user_data;
            u32 const                  ac    = items[ai].m_count;
            u32 const                  bc    = items[bi].m_count;
            if (ac > bc)
                return -1;  // Sort in descending order
            if (ac < bc)
                return 1;  // Sort in descending order
            return 0;
        }

        static inline u32 s_units_to_bytes(u32 units, u32 bits_per_unit) { return ((units * bits_per_unit) + 7) >> 3; }
        static inline u16 s_rgba888_to_rgb565(u32 rgba) { return ((rgba >> 8) & 0xf800) | ((rgba >> 5) & 0x07e0) | ((rgba >> 3) & 0x001f); }

        static inline void s_update_tile(u8* tile_change_data, u32 line_index, u32 line_span_index, u32 spans_per_line)
        {
            const u32 line_offset = line_index * ((spans_per_line + 7) / 8);
            const u32 bit_index   = line_span_index & 7;   // bit index within the byte
            const u32 byte_index  = line_span_index >> 3;  // byte index within the line's tile change data
            tile_change_data[line_offset + byte_index] |= (1 << bit_index);
        }

        static s32 s_compress(const u8* stream, u32 stream_size_in_bits, u8 symbol_bits, u8* out_stream, u8* out_symbol_rb)
        {
            // size in bytes of the encoded stream, or a negative value on error
            nsrlen::syminfo_t* syminfo_array = nullptr;
            const s32          encoded_size  = nsrlen::analyze_bits(stream, stream_size_in_bits, symbol_bits, out_symbol_rb, syminfo_array);
            if (encoded_size < 0)
                return -1;  // error analyzing bits

            nsrlen::out_t out_stream_info;
            out_stream_info.m_data = out_stream;
            out_stream_info.m_size = (stream_size_in_bits + 7) >> 3;  // We use the uncompressed size as the upper bound for the compressed size

            ASSERT(stream_size_in_bits <= (u32)(encoded_size * 8));  // Compression should not increase the size

            const s32 encoded_num_bits = nsrlen::encode_bits(stream, stream_size_in_bits, symbol_bits, out_symbol_rb, out_stream_info);

            ASSERT(encoded_num_bits == encoded_size * 8);  // The encoded size in bits should match the size calculated during analysis
            return encoded_size;
        }

        encoded_frame_t* encode_frame(arena_t* arena, u32 const* current_img, u32 const* previous_img, u16 width, u16 height, u16 tile_size)
        {
            ASSERT(math::alignUp4(sizeof(frame_begin_t)) == sizeof(frame_begin_t));  // Ensure frame_begin_t is already aligned to 4 bytes!

            // Initialize histogram and palette
            histogram_t* histogram = g_allocate<histogram_t>(arena);
            for (i32 i = 0; i < 65536; ++i)
            {
                histogram->m_items[i].m_color = i;  // Initialize histogram color (index)
                histogram->m_items[i].m_count = 0;  // Initialize histogram count
            }

            // Used for computing the full encoded data size
            void* begin_memory_used = narena::current_address(arena);

            // 1. Build color histogram and palette
            for (u32 y = 0; y < height; ++y)
            {
                // u32 const* previous_img_row    = previous_img + y * width;
                u32 const* current_img_row     = current_img + y * width;
                u32 const* current_img_row_end = current_img_row + width;
                while (current_img_row < current_img_row_end)
                {
                    const u16 crgb565 = s_rgba888_to_rgb565(*current_img_row++);
                    histogram->m_items[crgb565].m_count++;
                }
            }

            // Sort colors by frequency and build palette and histogram index
            nsort::sort(histogram->m_items, 65536, s_histogram_cmp_fn, (const void*)histogram->m_items);

            // Compute the required size for frame_begin_t that includes a dynamically sized tile change
            // data array based on the image dimensions and tile size, and allocate memory for it from the arena.
            const u32      span_count_per_line   = (width + tile_size - 1) / tile_size;
            const u32      tile_change_data_size = math::alignUp4((span_count_per_line + 7) / 8 * (u32)height);  // size of tile change data in bytes, rounded up to the nearest byte
            const u32      frame_begin_size      = sizeof(frame_begin_t) + tile_change_data_size;                // total size of frame_begin_t including tile change data
            frame_begin_t* frame_begin           = g_allocate_memory<frame_begin_t>(arena, frame_begin_size);
            u8*            tile_change_data      = (u8*)(frame_begin + 1);  // tile change data starts immediately after the frame_begin_t structure
            g_memory_fill(tile_change_data, 0, tile_change_data_size);      // Initialize tile change data to 0

            // Build RGB565 -> palette index map: 0..275 for palette entries, -1 for raw.
            for (i32 i = 0; i < 276; ++i)
            {
                const i32 color                   = histogram->m_items[i].m_color;
                frame_begin->m_palette[i]         = color;
                histogram->m_items[color].m_color = i;  // Use histogram color as palette index for lookup during encoding
            }
            for (i32 i = 276; i < 65536; ++i)
            {
                histogram->m_items[i].m_color = -1;
            }

            // For P2, P4 and P8 we can calculate the number of pixels that are used
            u32 p2_pixel_count = 0;
            for (u32 i = 0; i < 4; ++i)
            {
                u32 const count = histogram->m_items[i].m_count;
                p2_pixel_count += count;
            }
            u32 p4_pixel_count = 0;
            for (u32 i = 4; i < 20; ++i)
            {
                u32 const count = histogram->m_items[i].m_count;
                p4_pixel_count += count;
            }
            u32 p8_pixel_count = 0;
            for (u32 i = 20; i < 276; ++i)
            {
                u32 const count = histogram->m_items[i].m_count;
                p8_pixel_count += count;
            }
            const u32 total_pixel_count = width * height;
            const u32 p16_pixel_count   = total_pixel_count - (p2_pixel_count + p4_pixel_count + p8_pixel_count);
            ASSERT(p16_pixel_count + p8_pixel_count + p4_pixel_count + p2_pixel_count == total_pixel_count);

            u8 stream_symbol_sizes[STREAM_COUNT];
            stream_symbol_sizes[STREAM_P16]  = SYMBOL_SIZE_P16;
            stream_symbol_sizes[STREAM_P8]   = SYMBOL_SIZE_P8;
            stream_symbol_sizes[STREAM_P4]   = SYMBOL_SIZE_P4;
            stream_symbol_sizes[STREAM_P2]   = SYMBOL_SIZE_P2;
            stream_symbol_sizes[STREAM_PS]   = SYMBOL_SIZE_PS;
            stream_symbol_sizes[STREAM_SPAN] = SYMBOL_SIZE_SPAN;

            // Now we can calculate the size each stream needs in bytes
            u32 max_stream_size_in_bytes[STREAM_COUNT] = {0, 0, 0, 0, 0, 0};
            max_stream_size_in_bytes[STREAM_P16]       = (p16_pixel_count * 2);                                        // 16 bits per pixel = 2 bytes per pixel
            max_stream_size_in_bytes[STREAM_P8]        = (p8_pixel_count * 1);                                         // 8 bits per pixel = 1 byte per pixel
            max_stream_size_in_bytes[STREAM_P4]        = (p4_pixel_count + 1) >> 1;                                    //
            max_stream_size_in_bytes[STREAM_P2]        = (p2_pixel_count + 3) >> 2;                                    //
            max_stream_size_in_bytes[STREAM_PS]        = (total_pixel_count + 3) >> 2;                                 // 2 bits per pixel selector stream, so total pixels / 4
            max_stream_size_in_bytes[STREAM_SPAN]      = ((((width + tile_size - 1) / tile_size) * height) + 7) >> 3;  // 1 bit per span, so total spans / 8

            // Now that we have everything set up, we can start encoding the image by comparing it to the
            // previous image and filling the streams accordingly.

            // Obtain an arena scratch point, since the allocated data streams below are temporary
            arena_point_t scratch_point = save_point(arena);

            nbitstream::writer_t stream_writer[STREAM_COUNT];
            u32                  stream_data_sizes[STREAM_COUNT] = {0, 0, 0, 0, 0, 0};
            u8*                  stream_data_ptrs[STREAM_COUNT];

            u8* rb_arrays[STREAM_COUNT];
            rb_arrays[STREAM_P16]  = nullptr;
            rb_arrays[STREAM_P8]   = frame_begin->m_p8_rb;
            rb_arrays[STREAM_P4]   = frame_begin->m_p4_rb;
            rb_arrays[STREAM_P2]   = frame_begin->m_p2_rb;
            rb_arrays[STREAM_PS]   = frame_begin->m_ps_rb;
            rb_arrays[STREAM_SPAN] = frame_begin->m_span_rb;

            {
                for (u32 i = 0; i < STREAM_COUNT; ++i)
                {
                    u8* stream_data_ptr  = (u8*)g_allocate_memory<u8>(arena, max_stream_size_in_bytes[i]);
                    stream_data_sizes[i] = max_stream_size_in_bytes[i];
                    stream_data_ptrs[i]  = stream_data_ptr;
                    nbitstream::init(&stream_writer[i], stream_data_ptr, max_stream_size_in_bytes[i] * 8);
                }

                // First we encode the full image to obtain the rb values for each stream, which are needed for
                // compressing the streams per line.
                for (u32 y = 0; y < height; ++y)
                {
                    u32 const* current_img_row  = current_img + y * width;
                    u32 const* previous_img_row = previous_img + y * width;

                    u32 num_spans_changed = 0;
                    for (u32 span = 0; span < span_count_per_line; ++span)
                    {
                        const u32 x0 = span * tile_size;
                        const u32 x1 = (x0 + tile_size) < width ? (x0 + tile_size) : width;

                        bool span_changed = false;
                        for (u32 x = x0; x < x1 && !span_changed; ++x)
                        {
                            const u16 crgb565 = s_rgba888_to_rgb565(current_img_row[x]);
                            const u16 prgb565 = s_rgba888_to_rgb565(previous_img_row[x]);
                            span_changed      = (crgb565 != prgb565);
                        }
                        nbitstream::write_bits(&stream_writer[STREAM_SPAN], span_changed ? 1u : 0u, 1);
                        if (!span_changed)
                            continue;
                        num_spans_changed++;

                        // Update this tile as changed in the tile change data
                        s_update_tile(tile_change_data, y, span, span_count_per_line);

                        for (u32 x = x0; x < x1; ++x)
                        {
                            // Note:
                            // Below we are ignoring the return value of write_bits for performance reasons
                            // We know that the stream have enough capacity because we calculated and reserved
                            // the correct amount of memory for each stream at the start of this function,
                            // and we are filling the streams in a way that matches the calculated sizes.

                            const u16 crgb565       = s_rgba888_to_rgb565(current_img_row[x]);
                            const u16 palette_index = histogram->m_items[crgb565].m_color;
                            if (palette_index < 4)
                            {
                                nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P2, SYMBOL_SIZE_PS);
                                nbitstream::write_bits(&stream_writer[STREAM_P2], (u32)palette_index, SYMBOL_SIZE_P2);
                            }
                            else if (palette_index < 20)
                            {
                                nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P4, SYMBOL_SIZE_PS);
                                nbitstream::write_bits(&stream_writer[STREAM_P4], (u32)(palette_index - 4), SYMBOL_SIZE_P4);
                            }
                            else if (palette_index < 276)
                            {
                                nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P8, SYMBOL_SIZE_PS);
                                nbitstream::write_bits(&stream_writer[STREAM_P8], (u32)(palette_index - 20), SYMBOL_SIZE_P8);
                            }
                            else
                            {
                                nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P16, SYMBOL_SIZE_PS);
                                // no need to write p16 here since we don't need it because it is
                                // not going to be compressed.
                                // nbitstream::write_bits(&p16_writer, (u32)crgb565, SYMBOL_SIZE_P16);
                            }
                        }
                    }
                }

                // Compress the streams to obtain the rb values for each stream, which are needed for the frame header,
                // and also to compress streams per line.
                for (u32 i = 0; i < STREAM_COUNT; ++i)
                {
                    if (i == STREAM_P16)
                        continue;

                    const u32 stream_size_in_bits = nbitstream::finalize(&stream_writer[i]);
                    if (stream_size_in_bits == 0)
                        continue;  // No data in this stream, so we can skip compression and rb calculation

                    arena_point_t encoding_point = save_point(arena);
                    {
                        u8*       stream_enc_data_ptr          = (u8*)g_allocate_memory<u8>(arena, s_units_to_bytes(stream_size_in_bits, 8));
                        const s32 encoded_stream_size_in_bytes = s_compress(stream_data_ptrs[i], stream_size_in_bits, stream_symbol_sizes[i], stream_enc_data_ptr, rb_arrays[i]);
                        ASSERT(encoded_stream_size_in_bytes >= 0);
                    }
                    restore_point(encoding_point);
                }
            }
            // Now that we have the rb-arrays, release the memory used for encoding the streams
            restore_point(scratch_point);

            // Now we are going to encode the image line by line, and for each line we will emit a frame_line_t
            // structure, followed by the compressed streams for that line. We will use the rb values obtained
            // above to encode the streams for each line.

            // Allocate data for each stream, this time only on a per line size
            u32 enc_stream_data_sizes[STREAM_COUNT] = {0, 0, 0, 0, 0, 0};
            u8* enc_stream_data_ptrs[STREAM_COUNT];
            for (u32 i = 0; i < STREAM_COUNT; ++i)
            {
                // normal stream
                stream_data_sizes[i] = ((width * stream_symbol_sizes[i] + 7) / 8) + 32;
                u8* stream_data_ptr  = (u8*)g_allocate_memory<u8>(arena, stream_data_sizes[i]);
                stream_data_ptrs[i]  = stream_data_ptr;

                // encoded stream
                enc_stream_data_sizes[i] = stream_data_sizes[i];  // In the worst case, the compressed stream can be larger than the uncompressed stream, so we use the uncompressed size as the upper bound for the compressed size.
                u8* enc_stream_data_ptr  = (u8*)g_allocate_memory<u8>(arena, enc_stream_data_sizes[i]);
                enc_stream_data_ptrs[i]  = enc_stream_data_ptr;
            }

            arena_point_t line_memory_start       = save_point(arena);
            u32           number_of_lines_emitted = 0;

            for (u32 y = 0; y < height; ++y)
            {
                u32 const* current_img_row  = current_img + y * width;
                u32 const* previous_img_row = previous_img + y * width;

                // Setup the bitstream writers for this line
                for (u32 i = 0; i < STREAM_COUNT; ++i)
                {
                    nbitstream::init(&stream_writer[i], stream_data_ptrs[i], stream_data_sizes[i] * 8);
                }

                u32 num_spans_changed = 0;
                for (u32 span = 0; span < span_count_per_line; ++span)
                {
                    const u32 x0 = span * tile_size;
                    const u32 x1 = (x0 + tile_size) < width ? (x0 + tile_size) : width;

                    bool span_changed = false;
                    for (u32 x = x0; x < x1 && !span_changed; ++x)
                    {
                        const u16 crgb565 = s_rgba888_to_rgb565(current_img_row[x]);
                        const u16 prgb565 = s_rgba888_to_rgb565(previous_img_row[x]);
                        span_changed      = (crgb565 != prgb565);
                    }
                    nbitstream::write_bits(&stream_writer[STREAM_SPAN], span_changed ? 1u : 0u, SYMBOL_SIZE_SPAN);
                    if (!span_changed)
                        continue;
                    num_spans_changed++;

                    for (u32 x = x0; x < x1; ++x)
                    {
                        // Note:
                        // Below we are ignoring the return value of write_bits for performance reasons
                        // We know that the streams have enough capacity because we calculated and reserved
                        // the correct amount of memory for each stream at the start of this function,
                        // and we are filling the streams in a way that matches the calculated sizes.

                        const u16 crgb565       = s_rgba888_to_rgb565(current_img_row[x]);
                        const u16 palette_index = histogram->m_items[crgb565].m_color;
                        if (palette_index < 4)
                        {
                            nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P2, SYMBOL_SIZE_PS);
                            nbitstream::write_bits(&stream_writer[STREAM_P2], (u32)palette_index, SYMBOL_SIZE_P2);
                        }
                        else if (palette_index < 20)
                        {
                            nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P4, SYMBOL_SIZE_PS);
                            nbitstream::write_bits(&stream_writer[STREAM_P4], (u32)(palette_index - 4), SYMBOL_SIZE_P4);
                        }
                        else if (palette_index < 276)
                        {
                            nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P8, SYMBOL_SIZE_PS);
                            nbitstream::write_bits(&stream_writer[STREAM_P8], (u32)(palette_index - 20), SYMBOL_SIZE_P8);
                        }
                        else
                        {
                            nbitstream::write_bits(&stream_writer[STREAM_PS], SELECTOR_P16, SYMBOL_SIZE_PS);
                            nbitstream::write_bits(&stream_writer[STREAM_P16], (u32)crgb565, SYMBOL_SIZE_P16);
                        }
                    }
                }

                if (num_spans_changed > 0)
                {
                    // Emit a frame_line_t structure for this line
                    u32 stream_written_bits[STREAM_COUNT];
                    for (i32 i = 0; i < STREAM_COUNT; ++i)
                    {
                        stream_written_bits[i] = nbitstream::finalize(&stream_writer[i]);
                    }

                    number_of_lines_emitted++;
                    arena_point_t line_start = save_point(arena);
                    frame_line_t* fl         = g_allocate<frame_line_t>(arena, 4);
                    fl->m_msg_id             = 0x464c;  // 'FL' in ASCII
                    fl->m_index              = (u16)y;

                    fl->m_flags = 0;
                    for (i32 i = 0; i < STREAM_COUNT; ++i)
                        fl->m_flags |= (stream_written_bits[i] > 0) ? (1 << i) : 0;

                    // Compress some of the streams
                    fl->m_num_streams                  = 0;
                    u32 stream_enc_sizes[STREAM_COUNT] = {0};
                    for (i32 i = 0; i < STREAM_COUNT; ++i)
                    {
                        if (stream_written_bits[i] > 0)
                        {
                            fl->m_num_streams++;

                            if (i == STREAM_P16)
                            {
                                stream_enc_sizes[STREAM_P16] = stream_written_bits[STREAM_P16] / 8;
                                g_memcpy(enc_stream_data_ptrs[STREAM_P16], stream_data_ptrs[STREAM_P16], stream_written_bits[STREAM_P16] / 8);
                            }
                            else
                            {
                                stream_enc_sizes[i] = s_compress(stream_data_ptrs[i], stream_written_bits[i], stream_symbol_sizes[i], enc_stream_data_ptrs[i], rb_arrays[i]);
                                ASSERT(stream_enc_sizes[i] >= 0);
                            }
                        }
                        else
                        {
                            stream_enc_sizes[i] = 0;
                        }
                    }

                    // Emit the dynamic size array of stream sizes, in the order of p16, p8, p4, p2, selector, span
                    u16* stream_sizes = g_allocate_memory<u16>(arena, fl->m_num_streams, 2);
                    u32  si           = 0;
                    for (i32 i = 0; i < STREAM_COUNT; ++i)
                    {
                        if (stream_written_bits[i] > 0)
                            *stream_sizes++ = (u16)stream_enc_sizes[i];
                    }

                    // Now we emit the per stream data for this line, in the order of p16, p8, p4, p2, selector, span
                    for (i32 i = 0; i < STREAM_COUNT; ++i)
                    {
                        if (stream_written_bits[i] > 0)
                        {
                            u8* stream_data_ptr = g_allocate_memory<u8>(arena, stream_enc_sizes[i], 1);
                            g_memcpy(stream_data_ptr, enc_stream_data_ptrs[i], stream_enc_sizes[i]);
                            stream_data_ptr += stream_enc_sizes[i];
                        }
                    }

                    // Set the message length for this line, which is:
                    //    sizeof(frame_line_t) + stream sizes array + all stream data
                    arena_point_t line_end = save_point(arena);
                    fl->m_msg_len          = diff_point<u16>(line_start, line_end);
                }
            }

            void*     end_memory_used = narena::current_address(arena);
            const u32 encoded_size    = g_ptr_diff_in_bytes<u32>(begin_memory_used, end_memory_used);

            arena_point_t line_memory_end = save_point(arena);
            const u32     lines_data_size = diff_point<u32>(line_memory_start, line_memory_end);

            encoded_frame_t* result    = g_allocate<encoded_frame_t>(arena);
            result->m_frame_begin      = frame_begin;
            result->m_frame_begin_size = frame_begin_size;
            result->m_line_data        = (frame_line_t*)line_memory_start.m_address;
            result->m_line_data_size   = (u32)lines_data_size;
            result->m_num_lines        = number_of_lines_emitted;

            result->m_lines = g_allocate<frame_line_t*>(arena, number_of_lines_emitted);
            {
                frame_line_t* line = (frame_line_t*)line_memory_start.m_address;
                for (u32 i = 0; i < number_of_lines_emitted; ++i)
                {
                    result->m_lines[i] = line;
                    line               = g_ptr_advance(line, line->m_msg_len);
                }
            }

            return result;
        }
    }  // namespace nfenc
}  // namespace ncore
