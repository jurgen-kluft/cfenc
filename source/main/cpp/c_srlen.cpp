#include "ccore/c_target.h"
#include "ccore/c_math.h"
#include "ccore/c_memory.h"

#include "cfenc/c_srlen.h"
#include "cfenc/c_bitstream.h"

namespace ncore
{
    namespace nsrlen
    {
        // bitstream format:
        //  - u32      decoded_size_in_bits;  // size of the decoded bitstream in bits
        //  - u8       symbol_bits;           // number of bits used to encode each symbol (1, 2, 4 or 8)
        //  - u8       run_bits[];            // array of run-bits per symbols (size = 2^symbol_bits)
        //  - u8       encoded_data[];        // the encoded bitstream data

        s32 analyze_bits(const u8* data, u32 data_bits, u8 symbol_bits, u8* rb_table, syminfo_t* analysis)
        {
            // only allowed symbol_bits; 1, 2, 4 or 8
            if (symbol_bits != 1 && symbol_bits != 2 && symbol_bits != 4 && symbol_bits != 8)
                return -1;

            // First figure out the optimal 'run_bits' for each symbol, which determines how the run lengths are encoded in the bitstream.
            // Here we are using the output buffer to store the temporary symbol information
            // (size in bits for each rb) during the analysis phase, and then we will write
            // the header and encoded data to the same buffer after we have determined the
            // optimal run_bits for each symbol.
            syminfo_t* symbol_info = analysis;

            // Initialize the symbol info
            const u32 num_symbols      = 1U << symbol_bits;
            const u32 symbol_info_size = sizeof(syminfo_t) * num_symbols;
            g_memclr(symbol_info, symbol_info_size);

            nbitstream::reader_t bitreader;
            nbitstream::init(&bitreader, data, data_bits);
            i32 num_reads = data_bits / symbol_bits;
            while (num_reads > 0)
            {
                const u32 symbol = nbitstream::read_bits_unguarded(&bitreader, symbol_bits);
                u32       count  = num_reads;
                --num_reads;
                while (num_reads > 0)
                {
                    const u32 next_symbol = nbitstream::peek_bits_unguarded(&bitreader, symbol_bits);
                    if (next_symbol != symbol)
                        break;
                    nbitstream::skip_bits_unguarded(&bitreader, symbol_bits);
                    --num_reads;
                }
                count = count - num_reads;

                // here for each rb we calculate the size of the encoding and add to
                // the total size for that rb
                syminfo_t& info = symbol_info[symbol];
                info.m_units[0] += count;
                info.m_units[1] += (count + 1) >> 1;
                info.m_units[2] += (count + 3) >> 2;
                info.m_units[3] += (count + 7) >> 3;
                info.m_units[4] += (count + 15) >> 4;
                info.m_units[5] += (count + 31) >> 5;
            }

            // Note:
            // During reading we only have been collecting the number of units per rb,
            // but we need to multiply that with the size of each unit (symbol_bits + rb)
            // to get the total size in bits for each rb.

            u32 final_decoded_size_in_bits = 0;
            for (u32 symbol = 0; symbol < num_symbols; ++symbol)
            {
                u8  best_rb           = 0;
                u32 best_size_in_bits = symbol_info[symbol].m_units[0] * symbol_bits;
                for (u8 rb = 1; rb <= 5; ++rb)
                {
                    const u32 size_in_bits = symbol_info[symbol].m_units[rb] * (symbol_bits + rb);
                    if (size_in_bits < best_size_in_bits)
                    {
                        best_rb           = rb;
                        best_size_in_bits = size_in_bits;
                    }
                }
                rb_table[symbol] = best_rb;
                final_decoded_size_in_bits += best_size_in_bits;
            }

            // return the size of the encoded bitstream in bytes (rounded up)
            return (s32)((final_decoded_size_in_bits + 7) >> 3);
        }

        s32 encode_bits(const u8* data, u32 data_bits, u8 symbol_bits, const u8* rb_table, out_t& out_encoded)
        {
            // Now we have the optimal rb for each symbol, we can encode the bitstream accordingly.
            nbitstream::reader_t bitreader;
            nbitstream::init(&bitreader, data, data_bits);

            nbitstream::writer_t bitwriter;
            nbitstream::init(&bitwriter, out_encoded.m_data, out_encoded.m_size * 8);

            u32 num_reads = data_bits / symbol_bits;
            while (num_reads > 0)
            {
                const u32 symbol = nbitstream::read_bits_unguarded(&bitreader, symbol_bits);
                u32       count  = num_reads;  // number of sequential occurrences of this symbol
                --num_reads;
                while (num_reads > 0)
                {
                    const u32 next_symbol = nbitstream::peek_bits_unguarded(&bitreader, symbol_bits);
                    if (next_symbol != symbol)
                        break;
                    nbitstream::skip_bits_unguarded(&bitreader, symbol_bits);
                    --num_reads;
                }
                count = count - num_reads;

                const u8 rb = rb_table[symbol];
                if (rb == 0)
                {
                    // raw mode, just write the symbols sequentially without RLE encoding
                    for (u32 i = 0; i < count; ++i)
                    {
                        if (nbitstream::write_bits(&bitwriter, symbol, symbol_bits) < 0)
                            return -1;  // error writing bits
                    }
                }
                else
                {
                    const u32 max_chunk = (1U << rb);
                    u32       remain    = count;
                    while (remain > 0)
                    {
                        const u32 chunk = math::min(remain, max_chunk);
                        if (nbitstream::write_bits(&bitwriter, symbol, symbol_bits) < 0)
                            return -1;  // error writing bits
                        if (nbitstream::write_bits(&bitwriter, chunk - 1, rb) < 0)
                            return -1;  // error writing bits
                        remain -= chunk;
                    }
                }
            }

            const u32 total_bits = nbitstream::finalize(&bitwriter);
            return (s32)(total_bits);
        }

        s32 decode_all(decoder_t& decoder, const u8* symbol_rb, u32 symbol_bits, u32 decoded_size_in_bits, out_t& out)
        {
            nbitstream::writer_t bitwriter;
            nbitstream::init(&bitwriter, out.m_data, out.m_size * 8);

            while (bitwriter.num_bits < decoded_size_in_bits)
            {
                const u32 symbol = read_symbol(&decoder, symbol_rb, symbol_bits);
                if (symbol >= (1U << symbol_bits))
                    return -1;  // invalid symbol

                const u8 rb = symbol_rb[symbol];
                if (rb == 0)
                {
                    if ((bitwriter.num_bits + symbol_bits) > decoded_size_in_bits)
                        return -1;  // malformed stream

                    // raw mode, just write one symbol
                    if (nbitstream::write_bits(&bitwriter, symbol, symbol_bits) < 0)
                        return -1;  // error writing bits
                }
                else
                {
                    const u32 chunk = read_bits(&decoder, rb) + 1;
                    const u32 remaining_symbols = (decoded_size_in_bits - bitwriter.num_bits) / symbol_bits;
                    if (chunk > remaining_symbols)
                        return -1;  // malformed stream
                    if (nbitstream::write_bits_repeat(&bitwriter, symbol, symbol_bits, chunk) < 0)
                        return -1;  // error writing bits
                }
            }

            const u32 total_bits = nbitstream::finalize(&bitwriter);
            return (s32)total_bits;
        }

    }  // namespace nrle
}  // namespace ncore
