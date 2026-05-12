#ifndef __CFENC_BIT_STREAM_H__
#define __CFENC_BIT_STREAM_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    namespace nbitstream
    {
        struct writer_t
        {
            u8*  buf;            // byte buffer
            u32  capacity_bits;  // capacity in bits
            u32  num_bits;       // total bits written
            u32  pos;            // byte position
            u32  accu_num_bits;  // bits in accumulator
            u64  accu_register;  // bit accumulator
            bool finalized;
        };

        struct reader_t
        {
            const u8* buf;
            u32       num_bits;
            u32       read_bits;
            u32       pos;
            u32       accu_num_bits;
            u64       accu_register;
        };

        // Constraints:
        // - maximum symbol size is 30 bits
        // - maximum bitstream size is 2^32 bits (4 GiB)
        // - write_bits() and read_bits() return -1 on error,
        //   otherwise 0 for write_bits() and the value for read_bits()

        //
        // Writer API
        //
        void init(writer_t* bs, u8* buffer, u32 capacity_bits);
        s8   write_bits(writer_t* bs, u32 value, u8 num_bits);
        s8   write_bits_repeat(writer_t* bs, u32 value, u8 num_bits, s32 n);
        u32  finalize(writer_t* bs);

        //
        // Reader API
        //
        void init(reader_t* bs, const u8* buffer, u32 num_bits);
        void reset(reader_t* bs);
        s32  read_bits(reader_t* bs, u8 num_bits);
        s32  peek_bits(reader_t* bs, u8 num_bits);
        s8   skip_bits(reader_t* bs, u8 num_bits);

        inline bool is_end(const reader_t* bs, u8 sizeof_symbol_bits) { return (bs->read_bits + sizeof_symbol_bits) > bs->num_bits; }

        inline void update_acummulator(reader_t* bs)
        {
            while (bs->accu_num_bits < 32 && (bs->pos * 8) < bs->num_bits)
            {
                bs->accu_register |= u64(bs->buf[bs->pos]) << bs->accu_num_bits;
                bs->accu_num_bits += 8;
                bs->pos++;
            }
        }

        // read bits function that requires you to do the validation. It is faster but
        // should be used with care.
        // note: this function is usefull if you know the exact amount of 'read_bits' you
        //       need to call and want to avoid the overhead of validation in read_bits().
        inline u32 read_bits_unguarded(reader_t* bs, u8 num_bits)
        {
            update_acummulator(bs);
            const u32 mask = (1u << num_bits) - 1u;
            const u32 v    = (u32)(bs->accu_register & mask);
            bs->accu_register >>= num_bits;
            bs->accu_num_bits -= num_bits;
            bs->read_bits += num_bits;
            return v;
        }

        inline u32 peek_bits_unguarded(reader_t* bs, u8 num_bits)
        {
            update_acummulator(bs);
            const u32 mask = (1u << num_bits) - 1u;
            return (u32)(bs->accu_register & mask);
        }

        inline void skip_bits_unguarded(reader_t* bs, u8 num_bits)
        {
            update_acummulator(bs);
            bs->accu_register >>= num_bits;
            bs->accu_num_bits -= num_bits;
            bs->read_bits += num_bits;
        }

    }  // namespace nbitstream
}  // namespace ncore

#endif  // __CFENC_BIT_STREAM_H__
