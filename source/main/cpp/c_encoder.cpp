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

            u32 stream_symbol_sizes[6];
            stream_symbol_sizes[LF_STREAM_P16]  = 16;
            stream_symbol_sizes[LF_STREAM_P8]   = 8;
            stream_symbol_sizes[LF_STREAM_P4]   = 4;
            stream_symbol_sizes[LF_STREAM_P2]   = 2;
            stream_symbol_sizes[LF_STREAM_PS]   = 2;
            stream_symbol_sizes[LF_STREAM_SPAN] = 1;

            // Now we can calculate the size each stream needs in bytes
            u32 max_stream_size_in_bytes[6]          = {0, 0, 0, 0, 0, 0};
            max_stream_size_in_bytes[LF_STREAM_P16]  = (p16_pixel_count * 2);                                        // 16 bits per pixel = 2 bytes per pixel
            max_stream_size_in_bytes[LF_STREAM_P8]   = (p8_pixel_count * 1);                                         // 8 bits per pixel = 1 byte per pixel
            max_stream_size_in_bytes[LF_STREAM_P4]   = (p4_pixel_count + 1) >> 1;                                    //
            max_stream_size_in_bytes[LF_STREAM_P2]   = (p2_pixel_count + 3) >> 2;                                    //
            max_stream_size_in_bytes[LF_STREAM_PS]   = (total_pixel_count + 3) >> 2;                                 // 2 bits per pixel selector stream, so total pixels / 4
            max_stream_size_in_bytes[LF_STREAM_SPAN] = ((((width + tile_size - 1) / tile_size) * height) + 7) >> 3;  // 1 bit per span, so total spans / 8

            // Build RGB565 -> palette index map: 0..275 for palette entries, -1 for raw.
            for (u32 i = 0; i < 65536; ++i)
                encoder.m_histogram_color[i] = -1;

            for (u32 i = 0; i < 276; ++i)
            {
                if (encoder.m_histogram_count[out_begin->m_palette[i]] == 0)
                    break;
                encoder.m_histogram_color[out_begin->m_palette[i]] = (i16)i;
            }

            // Now that we have everything set up, we can start encoding the image by comparing it to the
            // previous image and filling the streams accordingly.

            nbitstream::writer_t stream_writer[6];
            u8*                  stream_data_ptr = out_data;
            for (u32 i = 0; i < 6; ++i)
            {
                nbitstream::init(&stream_writer[i], stream_data_ptr, max_stream_size_in_bytes[i] * 8);
                stream_data_ptr += max_stream_size_in_bytes[i];
            }

            // First we encode the full image to obtain the rb values for each stream, which are needed for
            // compressing the streams per line.
            const u32 span_count_per_line = (width + tile_size - 1) / tile_size;
            u8*       out_data_ptr        = out_data;
            for (u32 y = 0; y < height; ++y)
            {
                u32 const* current_img_row  = current_img + y * width;
                u32 const* previous_img_row = previous_img + y * width;

                u32 num_spans_changed = 0;
                for (u32 run = 0; run < span_count_per_line; ++run)
                {
                    const u32 x0 = run * tile_size;
                    const u32 x1 = (x0 + tile_size) < width ? (x0 + tile_size) : width;

                    bool span_changed = false;
                    for (u32 x = x0; x < x1 && !span_changed; ++x)
                    {
                        const u16 crgb565 = s_rgba888_to_rgb565(current_img_row[x]);
                        const u16 prgb565 = s_rgba888_to_rgb565(previous_img_row[x]);
                        span_changed      = (crgb565 != prgb565);
                    }
                    nbitstream::write_bits(&stream_writer[LF_STREAM_SPAN], span_changed ? 1u : 0u, 1);
                    if (!span_changed)
                        continue;

                    num_spans_changed++;

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
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P2, 2);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P2], (u32)palette_index, 2);
                        }
                        else if (palette_index < 20)
                        {
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P4, 2);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P4], (u32)(palette_index - 4), 4);
                        }
                        else if (palette_index < 276)
                        {
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P8, 2);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P8], (u32)(palette_index - 20), 8);
                        }
                        else
                        {
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P16, 2);
                            // no need to write p16 here since we don't need it because it is
                            // not going to be compressed.
                            // nbitstream::write_bits(&p16_writer, (u32)crgb565, 16);
                        }
                    }
                }
            }

            // Compress the streams to obtain the rb values for each stream, which are needed for the frame header,
            // and also to compress streams per line.

            u8* rb_arrays[6] = {nullptr, out_begin->m_p8_rb, out_begin->m_p4_rb, out_begin->m_p2_rb, out_begin->m_ps_rb, out_begin->m_span_rb};
            for (u32 i = 0; i < 6; ++i)
            {
                if (i == LF_STREAM_P16)
                    continue;  // We don't compress the P16 stream, so we don't need to calculate rb values for it

                const u32 stream_size_in_bits = nbitstream::finalize(&stream_writer[i]);
                if (stream_size_in_bits == 0)
                    continue;  // No data in this stream, so we can skip compression and rb calculation

                const s32 encoded_stream_size_in_bytes = s_compress((u8*)encoder.m_streams[i], stream_size_in_bits, stream_symbol_sizes[i], (u8*)encoder.m_enc_streams[i], rb_arrays[i]);
                ASSERT(encoded_stream_size_in_bytes >= 0);
            }

            // Now we are going to encode the image line by line, and for each line we will emit a frame_line_t
            // structure, followed by the compressed streams for that line. We will use the rb values obtained
            // above to encode the streams for each line.

            const u32 span_count_per_line = (width + tile_size - 1) / tile_size;
            u8*       out_data_ptr        = out_data;
            for (u32 y = 0; y < height; ++y)
            {
                u32 const* current_img_row  = current_img + y * width;
                u32 const* previous_img_row = previous_img + y * width;

                // Setup the bitstream writers for this line
                for (u32 i = 0; i < 6; ++i)
                {
                    nbitstream::init(&stream_writer[i], (u8*)encoder.m_streams[i], sizeof(encoder.m_streams[i]) * 2 * 8);
                }

                u32 num_spans_changed = 0;
                for (u32 run = 0; run < span_count_per_line; ++run)
                {
                    const u32 x0 = run * tile_size;
                    const u32 x1 = (x0 + tile_size) < width ? (x0 + tile_size) : width;

                    bool span_changed = false;
                    for (u32 x = x0; x < x1 && !span_changed; ++x)
                    {
                        const u16 crgb565 = s_rgba888_to_rgb565(current_img_row[x]);
                        const u16 prgb565 = s_rgba888_to_rgb565(previous_img_row[x]);
                        span_changed      = (crgb565 != prgb565);
                    }
                    nbitstream::write_bits(&stream_writer[LF_STREAM_SPAN], span_changed ? 1u : 0u, 1);
                    if (!span_changed)
                        continue;
                    num_spans_changed++;

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
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P2, stream_symbol_sizes[LF_STREAM_PS]);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P2], (u32)palette_index, stream_symbol_sizes[LF_STREAM_P2]);
                        }
                        else if (palette_index < 20)
                        {
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P4, stream_symbol_sizes[LF_STREAM_PS]);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P4], (u32)(palette_index - 4), stream_symbol_sizes[LF_STREAM_P4]);
                        }
                        else if (palette_index < 276)
                        {
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P8, stream_symbol_sizes[LF_STREAM_PS]);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P8], (u32)(palette_index - 20), stream_symbol_sizes[LF_STREAM_P8]);
                        }
                        else
                        {
                            nbitstream::write_bits(&stream_writer[LF_STREAM_PS], SELECTOR_P16, stream_symbol_sizes[LF_STREAM_PS]);
                            nbitstream::write_bits(&stream_writer[LF_STREAM_P16], (u32)crgb565, stream_symbol_sizes[LF_STREAM_P16]);
                        }
                    }
                }

                if (num_spans_changed > 0)
                {
                    // Emit a frame_line_t structure for this line
                    u32 stream_written_bytes[6];
                    for (i32 i = 0; i < 6; ++i)
                    {
                        stream_written_bytes[i] = (nbitstream::finalize(&stream_writer[i]) + 7) / 8;
                    }

                    frame_line_t* fl = (frame_line_t*)out_data_ptr;
                    fl->m_msg_id     = 0x464c;  // 'FL' in ASCII
                    fl->m_index      = (u16)y;

                    fl->m_flags = 0;
                    for (i32 i = 0; i < 6; ++i)
                        fl->m_flags |= (stream_written_bytes[i] > 0) ? (1 << i) : 0;

                    // Compress some of the streams
                    fl->m_num_streams       = 0;
                    u32 stream_enc_sizes[6] = {0};
                    for (i32 i = 0; i < 6; ++i)
                    {
                        if (stream_written_bytes[i] > 0)
                        {
                            fl->m_num_streams++;

                            if (i == LF_STREAM_P16)
                            {
                                stream_enc_sizes[LF_STREAM_P16] = stream_written_bytes[LF_STREAM_P16] * 2;
                                g_memcpy(encoder.m_enc_streams[i], encoder.m_streams[i], stream_enc_sizes[i]);
                            }
                            else
                            {
                                stream_enc_sizes[i] = s_compress((u8*)encoder.m_streams[i], stream_written_bytes[i] * stream_symbol_sizes[i], stream_symbol_sizes[i], (u8*)encoder.m_enc_streams[i], rb_arrays[i]);
                                ASSERT(stream_enc_sizes[i] >= 0);
                            }
                        }
                    }

                    // Emit the dynamic size array of stream sizes, in the order of p16, p8, p4, p2, selector, span
                    u16* stream_sizes = (u16*)(fl + 1);
                    u32  si           = 0;
                    for (i32 i = 0; i < 6; ++i)
                    {
                        if (stream_written_bytes[i] > 0)
                            *stream_sizes++ = (u16)stream_enc_sizes[i];
                    }

                    // Now we emit the per stream data for this line, in the order of p16, p8, p4, p2, selector, span
                    u8* stream_data_ptr = (u8*)stream_sizes;
                    for (i32 i = 0; i < 6; ++i)
                    {
                        if (stream_written_bytes[i] > 0)
                        {
                            g_memcpy(stream_data_ptr, encoder.m_enc_streams[i], stream_enc_sizes[i]);
                            stream_data_ptr += stream_enc_sizes[i];
                        }
                    }

                    // Move the output data pointer to the end of the data we just wrote, and align it to 2 bytes for the next line
                    stream_data_ptr = g_ptr_align(stream_data_ptr, (u32)2);

                    // Set the message length for this line, which is:
                    //    sizeof(frame_line_t) + stream sizes array + all stream data
                    fl->m_msg_len = (u16)(stream_data_ptr - out_data_ptr); 

                    // Update the out data pointer for the next line
                    out_data_ptr = stream_data_ptr;
                }

                const u32 encoded_size = 0;
                return encoded_size <= out_data_capacity ? (s32)encoded_size : -1;
            }

        }  // namespace nfenc
    }  // namespace ncore
