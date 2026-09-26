#include <gtest/gtest.h>

#include "WireReader.hpp"
#include "WireWriter.hpp"

#include <cstdint>
#include <vector>

TEST(WireReaderTest, ReadsWhatTheWriterStored)
{
    process_manager::WireWriter writer{40};
    writer.PutU8(0, 7);
    writer.PutU16(2, 513);
    writer.PutI32(4, -42);
    writer.PutU64(8, 1ull << 40);
    writer.PutF64(16, 12.5);
    writer.PutText(24, 16, "hello");
    const std::vector<std::uint8_t> bytes = writer.TakeBytes();

    const process_manager::WireReader reader{bytes, 0, bytes.size()};
    EXPECT_EQ(reader.U8(0), 7);
    EXPECT_EQ(reader.U16(2), 513);
    EXPECT_EQ(reader.I32(4), -42);
    EXPECT_EQ(reader.U64(8), 1ull << 40);
    EXPECT_DOUBLE_EQ(reader.F64(16), 12.5);
    EXPECT_EQ(reader.Text(24, 16), "hello");
}

TEST(WireReaderTest, ReadsPastTheEndAsZero)
{
    const std::vector<std::uint8_t> bytes{1, 2, 3};
    const process_manager::WireReader reader{bytes, 0, 3};
    EXPECT_EQ(reader.U32(0), 0u);
    EXPECT_EQ(reader.U8(3), 0);
}

TEST(WireReaderTest, PartIsRelativeToItsBase)
{
    const std::vector<std::uint8_t> bytes{9, 9, 1, 0, 0, 0};
    const process_manager::WireReader reader{bytes, 2, 4};
    EXPECT_EQ(reader.U32(0), 1u);
    EXPECT_EQ(reader.Size(), 4u);
}

TEST(WireReaderTest, SizeIsClippedToTheBuffer)
{
    const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    const process_manager::WireReader reader{bytes, 2, 100};
    EXPECT_EQ(reader.Size(), 2u);
    const process_manager::WireReader outside{bytes, 10, 4};
    EXPECT_EQ(outside.Size(), 0u);
}

TEST(WireReaderTest, TextWithoutNulTakesTheWholeField)
{
    const std::vector<std::uint8_t> bytes{'a', 'b', 'c'};
    const process_manager::WireReader reader{bytes, 0, 3};
    EXPECT_EQ(reader.Text(0, 3), "abc");
    EXPECT_EQ(reader.Text(1, 1), "b");
}
