#include "ccore/c_target.h"

#include "cfenc/c_bitstream.h"

#include "cunittest/cunittest.h"

using namespace ncore;

UNITTEST_SUITE_BEGIN(bitstream)
{
    UNITTEST_FIXTURE(writer)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(rejects_invalid_num_bits)
        {
            u8                   buffer[8] = {0};
            nbitstream::writer_t writer;
            nbitstream::init(&writer, buffer, 64);

            CHECK_EQUAL(-1, nbitstream::write_bits(&writer, 1, 0));
            CHECK_EQUAL(-1, nbitstream::write_bits(&writer, 1, 31));
            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 1, 1));
        }

        UNITTEST_TEST(respects_capacity_and_finalize_state)
        {
            u8                   buffer[8] = {0};
            nbitstream::writer_t writer;
            nbitstream::init(&writer, buffer, 10);

            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0xAB, 8));
            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0x1, 2));
            CHECK_EQUAL(-1, nbitstream::write_bits(&writer, 0x1, 1));

            CHECK_EQUAL((u32)10, nbitstream::finalize(&writer));
            CHECK_EQUAL(-1, nbitstream::write_bits(&writer, 0x1, 1));
        }

        UNITTEST_TEST(packs_bits_lsb_first_across_bytes)
        {
            u8                   buffer[8] = {0};
            nbitstream::writer_t writer;
            nbitstream::init(&writer, buffer, 64);

            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0b101, 3));
            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0b11, 2));
            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0xA5, 8));

            CHECK_EQUAL((u32)13, nbitstream::finalize(&writer));

            CHECK_EQUAL((u8)0xBD, buffer[0]);
            CHECK_EQUAL((u8)0x14, buffer[1]);
        }

        UNITTEST_TEST(flushes_on_32_bit_boundary)
        {
            u8                   buffer[8] = {0};
            nbitstream::writer_t writer;
            nbitstream::init(&writer, buffer, 64);

            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0x3FFFFFFF, 30));
            CHECK_EQUAL(0, nbitstream::write_bits(&writer, 0x2, 2));
            CHECK_EQUAL((u32)32, nbitstream::finalize(&writer));

            CHECK_EQUAL((u8)0xFF, buffer[0]);
            CHECK_EQUAL((u8)0xFF, buffer[1]);
            CHECK_EQUAL((u8)0xFF, buffer[2]);
            CHECK_EQUAL((u8)0xBF, buffer[3]);
        }
    }


    UNITTEST_FIXTURE(reader)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(rejects_invalid_num_bits)
        {
            const u8              buffer[2] = {0xAA, 0x55};
            nbitstream::reader_t  reader;
            nbitstream::init(&reader, buffer, 16);

            CHECK_EQUAL(-1, nbitstream::read_bits(&reader, 0));
            CHECK_EQUAL(-1, nbitstream::read_bits(&reader, 31));

            CHECK_EQUAL(-1, nbitstream::peek_bits(&reader, 0));
            CHECK_EQUAL(-1, nbitstream::peek_bits(&reader, 31));

            CHECK_EQUAL(-1, nbitstream::skip_bits(&reader, 0));
            CHECK_EQUAL(-1, nbitstream::skip_bits(&reader, 31));
        }

        UNITTEST_TEST(reads_lsb_first_across_bytes)
        {
            const u8             buffer[2] = {0xBD, 0x14};
            nbitstream::reader_t reader;
            nbitstream::init(&reader, buffer, 13);

            CHECK_EQUAL(0b101, nbitstream::read_bits(&reader, 3));
            CHECK_EQUAL(0b11, nbitstream::read_bits(&reader, 2));
            CHECK_EQUAL(0xA5, nbitstream::read_bits(&reader, 8));
            CHECK_TRUE(nbitstream::is_end(&reader, 1));
        }

        UNITTEST_TEST(peek_does_not_advance_and_skip_advances)
        {
            const u8             buffer[2] = {0xBD, 0x14};
            nbitstream::reader_t reader;
            nbitstream::init(&reader, buffer, 13);

            CHECK_EQUAL(0b101, nbitstream::peek_bits(&reader, 3));
            CHECK_EQUAL((u32)0, reader.read_bits);

            CHECK_EQUAL(0, nbitstream::skip_bits(&reader, 3));
            CHECK_EQUAL((u32)3, reader.read_bits);

            CHECK_EQUAL(0b11, nbitstream::peek_bits(&reader, 2));
            CHECK_EQUAL((u32)3, reader.read_bits);
            CHECK_EQUAL(0b11, nbitstream::read_bits(&reader, 2));
            CHECK_EQUAL((u32)5, reader.read_bits);
        }

        UNITTEST_TEST(rejects_reads_beyond_stream_end)
        {
            const u8             buffer[1] = {0xFF};
            nbitstream::reader_t reader;
            nbitstream::init(&reader, buffer, 5);

            CHECK_EQUAL(0x1F, nbitstream::read_bits(&reader, 5));

            CHECK_TRUE(nbitstream::is_end(&reader, 1));
        }

        UNITTEST_TEST(reset_rewinds_reader_state)
        {
            const u8             buffer[2] = {0xBD, 0x14};
            nbitstream::reader_t reader;
            nbitstream::init(&reader, buffer, 13);

            CHECK_EQUAL(0b101, nbitstream::read_bits(&reader, 3));
            CHECK_EQUAL(0b11, nbitstream::read_bits(&reader, 2));

            nbitstream::reset(&reader);

            CHECK_EQUAL((u32)0, reader.read_bits);
            CHECK_EQUAL((u32)0, reader.pos);
            CHECK_EQUAL((u32)0, reader.accu_num_bits);

            CHECK_EQUAL(0b101, nbitstream::read_bits(&reader, 3));
            CHECK_EQUAL(0b11, nbitstream::read_bits(&reader, 2));
            CHECK_EQUAL(0xA5, nbitstream::read_bits(&reader, 8));
        }

        UNITTEST_TEST(is_end_respects_symbol_size)
        {
            const u8             buffer[1] = {0x00};
            nbitstream::reader_t reader;
            nbitstream::init(&reader, buffer, 7);

            CHECK_FALSE(nbitstream::is_end(&reader, 4));
            CHECK_EQUAL(0, nbitstream::skip_bits(&reader, 4));

            CHECK_TRUE(nbitstream::is_end(&reader, 4));
            CHECK_FALSE(nbitstream::is_end(&reader, 3));
            CHECK_EQUAL(0, nbitstream::skip_bits(&reader, 3));
            CHECK_TRUE(nbitstream::is_end(&reader, 1));
        }

    }
}
UNITTEST_SUITE_END
