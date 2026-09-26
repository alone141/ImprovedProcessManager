#include <gtest/gtest.h>

#include "WireWriter.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

TEST(WireWriterTest, StartsZeroFilled)
{
    const process_manager::WireWriter writer{8};
    ASSERT_EQ(writer.Bytes().size(), 8u);
    for (const std::uint8_t byte : writer.Bytes())
    {
        EXPECT_EQ(byte, 0);
    }
}

TEST(WireWriterTest, StoresLittleEndian)
{
    process_manager::WireWriter writer{16};
    writer.PutU32(0, 0x11223344u);
    writer.PutU16(4, 0xAABBu);
    writer.PutI64(8, -2);
    const std::span<const std::uint8_t> bytes = writer.Bytes();
    EXPECT_EQ(bytes[0], 0x44);
    EXPECT_EQ(bytes[3], 0x11);
    EXPECT_EQ(bytes[4], 0xBB);
    EXPECT_EQ(bytes[5], 0xAA);
    EXPECT_EQ(bytes[8], 0xFE);
    EXPECT_EQ(bytes[15], 0xFF);
}

TEST(WireWriterTest, DropsWritesPastTheEnd)
{
    process_manager::WireWriter writer{4};
    writer.PutU64(0, 0xFFFFFFFFFFFFFFFFull);
    writer.PutU32(2, 0xFFFFFFFFu);
    for (const std::uint8_t byte : writer.Bytes())
    {
        EXPECT_EQ(byte, 0);
    }
}

TEST(WireWriterTest, TextKeepsTheLastByteNul)
{
    process_manager::WireWriter writer{4};
    writer.PutText(0, 4, "abcdef");
    const std::span<const std::uint8_t> bytes = writer.Bytes();
    EXPECT_EQ(bytes[0], 'a');
    EXPECT_EQ(bytes[2], 'c');
    EXPECT_EQ(bytes[3], 0);
}

TEST(WireWriterTest, TextOverwritesAnEarlierValue)
{
    process_manager::WireWriter writer{8};
    writer.PutText(0, 8, "longer");
    writer.PutText(0, 8, "ab");
    const std::span<const std::uint8_t> bytes = writer.Bytes();
    EXPECT_EQ(bytes[1], 'b');
    EXPECT_EQ(bytes[2], 0);
    EXPECT_EQ(bytes[5], 0);
}

TEST(WireWriterTest, FitUtf8KeepsShortText)
{
    EXPECT_EQ(process_manager::FitUtf8("abc", 5), "abc");
}

TEST(WireWriterTest, FitUtf8NeverSplitsACharacter)
{
    // "é" is two bytes, "€" three: a five-byte budget fits "aé" plus nothing of "€".
    const std::string text = "a\xC3\xA9\xE2\x82\xAC";
    EXPECT_EQ(process_manager::FitUtf8(text, 5), "a\xC3\xA9");
    EXPECT_EQ(process_manager::FitUtf8(text, 2), "a");
    EXPECT_EQ(process_manager::FitUtf8(text, 0), "");
}

TEST(WireWriterTest, TakingTheBytesEmptiesTheWriter)
{
    process_manager::WireWriter writer{4};
    writer.PutU32(0, 0x04030201u);
    const std::vector<std::uint8_t> taken = writer.TakeBytes();
    EXPECT_EQ(taken, (std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_TRUE(writer.Bytes().empty());
}
