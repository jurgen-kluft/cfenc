#ifndef __CFENC_SELECTIVE_RUN_LENGTH_ENTROPY_ENCODING_H__
#define __CFENC_SELECTIVE_RUN_LENGTH_ENTROPY_ENCODING_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "cfenc/c_bitstream.h"

namespace ncore
{
    namespace nsrlen
    {
        // ---- Encoder / decoder ----
        // This is 'selective run-length entropy encoding' (SRLEN) algorithm that encodes runs of repeated
        // symbols, and each symbol is assigned 'run bits' used to encode the run length.
        // Before encoding, the input data is analyzed to determine the optimal 'run bits' for
        // each symbol, which are then stored in the header of the encoded bitstream. One of the
        // main benefits of this approach is that the encoded result will never be larger than the
        // original data, as the 'run bits' for each symbol can be set to 0 (raw mode) if that
        // symbol does not have enough runs to benefit from run-length encoding.
        // During decoding, a header is needed to supply the symbol information and decode
        // the bitstream accordingly.

        struct out_t
        {
            u8* m_data;  // pointer to the output buffer
            u32 m_size;  // size of the output buffer in bytes
        };

        struct syminfo_t
        {
            // for each rb, we count the number of units (unit=symbol + run bits) that would be encoded
            // with that rb, which we can then use to calculate the total size in bits for that rb.
            u32 m_units[6];
        };

        // returns the size of the encoded stream in bytes, or a negative value on error
        // @analysis is used as temporary storage during the analysis phase to determine
        // the optimal 'run bits' for each symbol and needs to be passed on to encode_bits()
        // the size of the rb_table and syminfo arrays MUST be >= (1 << symbol_bits)
        s32 analyze_bits(const u8* data, u32 data_bits, u8 symbol_bits, u8* rb_table, syminfo_t* analysis);

        // returns the number of bits written to the output bitstream, or a negative value on error
        // symbol_bits can be 1 .. 8, and determines the size of each symbol in bits
        // note: caller is responsible for ensuring that the output buffer is large enough to hold
        //       the encoded data
        // note: do not encode to memory that is within the input data range, as the encoding process
        //       may overwrite data that has not yet been read. The writing output behaviour is far
        //       from linear and at the start can write a lot of data and at the end suddenly a lot of
        //       compression can happen.
        s32 encode_bits(const u8* data, u32 data_bits, u8 symbol_bits, const u8* rb_table, out_t& out_encoded);

        struct decoder_t
        {
            u8 const* m_stream;
            u32       m_pos;
            u32       m_accuRegister;
            u8        m_accuNumBits;
            u8        m_symbol_run;
            u8        m_symbol;
        };

        // Initialize the srlen decoder
        static inline void init(decoder_t* d, u8 const* stream)
        {
            d->m_stream       = stream;
            d->m_pos          = 0;
            d->m_accuNumBits  = 0;
            d->m_accuRegister = 0;
            d->m_symbol_run   = 0;
            d->m_symbol       = 0;
        }

        // Ensure at least 16 bits are available in the accumulator
        static inline void check_accumulator(decoder_t* d)
        {
            if (d->m_accuNumBits < 16)
            {
                d->m_accuRegister |= ((u32)d->m_stream[d->m_pos]) << d->m_accuNumBits;
                d->m_accuNumBits += 8;
                d->m_pos++;

                d->m_accuRegister |= ((u32)d->m_stream[d->m_pos]) << d->m_accuNumBits;
                d->m_accuNumBits += 8;
                d->m_pos++;
            }
        }

        static inline u32 read_bits(decoder_t* d, u8 n)
        {
            u32 v = d->m_accuRegister & ((1UL << n) - 1);
            d->m_accuRegister >>= n;
            d->m_accuNumBits -= n;
            return v;
        }

        static inline u8 read_symbol(decoder_t* d, const u8* symbol_rb, u8 symbol_bits)
        {
            if (d->m_symbol_run > 0)
            {
                d->m_symbol_run--;
                return d->m_symbol;
            }

            check_accumulator(d);

            const u8 symbol = (u8)read_bits(d, symbol_bits);
            const u8 rb     = symbol_rb[symbol];
            if (rb != 0)
            {
                d->m_symbol     = symbol;
                d->m_symbol_run = (u8)read_bits(d, rb);
            }
            return symbol;
        }

        s32 decode_all(decoder_t& decoder, const u8* symbol_rb, u32 symbol_bits, u32 decoded_size_in_bits, out_t& out);

    }  // namespace nrle
}  // namespace ncore

#endif  // __CFENC_SELECTIVE_RUN_LENGTH_ENTROPY_ENCODING_H__
