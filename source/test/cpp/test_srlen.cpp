#include "ccore/c_target.h"

#include "cfenc/c_srlen.h"

#include "cunittest/cunittest.h"

using namespace ncore;

UNITTEST_SUITE_BEGIN(srlen)
{
    static u32 pack_symbols(const u8* symbols, u32 count, u8 symbol_bits, u8* buffer, u32 buffer_size)
    {
        nbitstream::writer_t writer;
        nbitstream::init(&writer, buffer, buffer_size * 8);
        for (u32 i = 0; i < count; ++i)
            nbitstream::write_bits(&writer, symbols[i], symbol_bits);
        return nbitstream::finalize(&writer);
    }

    static bool check_buffers_equal(const u8* expected, const u8* actual, u32 num_bytes)
    {
        for (u32 i = 0; i < num_bytes; ++i)
            if (expected[i] != actual[i])
                return false;
        return true;
    }

    UNITTEST_FIXTURE(encode)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(rejects_invalid_symbol_sizes)
        {
            const u8          data[1]     = {0};
            u8                rb[2]       = {0};
            nsrlen::syminfo_t analysis[2] = {};

            CHECK_EQUAL(-1, nsrlen::analyze_bits(data, 8, 0, rb, analysis));
            CHECK_EQUAL(-1, nsrlen::analyze_bits(data, 8, 3, rb, analysis));
            CHECK_EQUAL(-1, nsrlen::analyze_bits(data, 8, 7, rb, analysis));
        }

        UNITTEST_TEST(analyze_selects_run_bits_for_repeated_symbols)
        {
            const u8          symbols[]   = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
            u8                source[8]   = {0};
            const u32         data_bits   = pack_symbols(symbols, (u32)(sizeof(symbols) / sizeof(symbols[0])), 2, source, sizeof(source));
            u8                rb[4]       = {0};
            nsrlen::syminfo_t analysis[4] = {};

            CHECK_EQUAL(1, nsrlen::analyze_bits(source, data_bits, 2, rb, analysis));
            CHECK_EQUAL(0, rb[0]);
            CHECK_EQUAL(0, rb[1]);
            CHECK_EQUAL(0, rb[2]);
            CHECK_EQUAL(4, rb[3]);
        }

        UNITTEST_TEST(roundtrips_alternating_symbols_in_raw_mode)
        {
            const u8          symbols[]   = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
            u8                source[8]   = {0};
            const u32         data_bits   = pack_symbols(symbols, (u32)(sizeof(symbols) / sizeof(symbols[0])), 2, source, sizeof(source));
            u8                rb[4]       = {0};
            nsrlen::syminfo_t analysis[4] = {};

            CHECK_EQUAL((sizeof(symbols) * 2), data_bits);

            CHECK_EQUAL(3, nsrlen::analyze_bits(source, data_bits, 2, rb, analysis));
            CHECK_EQUAL(0, rb[0]);
            CHECK_EQUAL(0, rb[1]);
            CHECK_EQUAL(0, rb[2]);
            CHECK_EQUAL(0, rb[3]);

            u8               encoded[8]   = {0};
            nsrlen::out_t    encoded_out  = {encoded, (u32)sizeof(encoded)};
            const s32        encoded_bits = nsrlen::encode_bits(source, data_bits, 2, rb, encoded_out);

            CHECK_EQUAL((s32)data_bits, encoded_bits);

            nsrlen::decoder_t decoder = {};
            nsrlen::init(&decoder, source);

            u8            decoded[8]  = {0};
            nsrlen::out_t decoded_out = {decoded, (u32)sizeof(decoded)};
            CHECK_EQUAL((s32)data_bits, nsrlen::decode_all(decoder, rb, 2, data_bits, decoded_out));
            CHECK_TRUE(check_buffers_equal(source, decoded, (data_bits + 7) >> 3));
        }

        UNITTEST_TEST(roundtrips_long_runs_with_chunking)
        {
            u8 symbols[44] = {0};
            for (u32 i = 0; i < 40; ++i)
                symbols[i] = 2;
            symbols[40] = 3;
            symbols[41] = 1;
            symbols[42] = 0;
            symbols[43] = 2;

            u8        source[16] = {0};
            const u32 data_bits  = pack_symbols(symbols, DARRAYSIZE(symbols), 2, source, sizeof(source));
            u8       rb[4]      = {0};
            nsrlen::syminfo_t analysis[4] = {};

            const u32         expected_encoded_size = ((2 + 5) + (2 + 5) + 2 + 2 + 2 + (2 + 5) + 7) / 8;
            CHECK_EQUAL(expected_encoded_size, (u32)nsrlen::analyze_bits(source, data_bits, 2, rb, analysis));
            CHECK_EQUAL(0, rb[0]);
            CHECK_EQUAL(0, rb[1]);
            CHECK_EQUAL(5, rb[2]);
            CHECK_EQUAL(0, rb[3]);

            u8               encoded[16]  = {0};
            nsrlen::out_t    encoded_out  = {encoded, (u32)sizeof(encoded)};
            const s32        encoded_bits = nsrlen::encode_bits(source, data_bits, 2, rb, encoded_out);

            CHECK_TRUE(encoded_bits > 0);
            CHECK_TRUE(encoded_bits < (s32)data_bits);
            CHECK_EQUAL(5, rb[2]);
            CHECK_EQUAL(0, rb[0]);

            nsrlen::decoder_t decoder = {};
            nsrlen::init(&decoder, source);

            u8            decoded[16] = {0};
            nsrlen::out_t decoded_out = {decoded, (u32)sizeof(decoded)};
            CHECK_EQUAL((s32)data_bits, nsrlen::decode_all(decoder, rb, 2, data_bits, decoded_out));
            check_buffers_equal(source, decoded, (data_bits + 7) >> 3);
        }
    }
}
UNITTEST_SUITE_END
