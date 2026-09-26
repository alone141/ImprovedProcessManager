#include "WireWriter.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace process_manager
{

WireWriter::WireWriter(std::size_t size)
    : bytes{}
{
    bytes.resize(size, 0);
}

void WireWriter::PutU8(std::size_t offset, std::uint8_t value)
{
    PutBytes(offset, value, 1);
}

void WireWriter::PutU16(std::size_t offset, std::uint16_t value)
{
    PutBytes(offset, value, 2);
}

void WireWriter::PutU32(std::size_t offset, std::uint32_t value)
{
    PutBytes(offset, value, 4);
}

void WireWriter::PutU64(std::size_t offset, std::uint64_t value)
{
    PutBytes(offset, value, 8);
}

void WireWriter::PutI32(std::size_t offset, std::int32_t value)
{
    PutBytes(offset, static_cast<std::uint32_t>(value), 4);
}

void WireWriter::PutI64(std::size_t offset, std::int64_t value)
{
    PutBytes(offset, static_cast<std::uint64_t>(value), 8);
}

void WireWriter::PutF64(std::size_t offset, double value)
{
    std::uint64_t raw{0};
    std::memcpy(&raw, &value, sizeof(raw));
    PutBytes(offset, raw, 8);
}

void WireWriter::PutText(std::size_t offset, std::size_t fieldSize, std::string_view text)
{
    if (fieldSize == 0 || offset > bytes.size() || fieldSize > bytes.size() - offset)
    {
        return;
    }

    const std::string_view fitted = FitUtf8(text, fieldSize - 1);
    std::memset(bytes.data() + offset, 0, fieldSize);
    std::memcpy(bytes.data() + offset, fitted.data(), fitted.size());
}

std::span<const std::uint8_t> WireWriter::Bytes() const
{
    return bytes;
}

std::vector<std::uint8_t> WireWriter::TakeBytes()
{
    return std::exchange(bytes, std::vector<std::uint8_t>{});
}

void WireWriter::PutBytes(std::size_t offset, std::uint64_t value, std::size_t count)
{
    if (offset > bytes.size() || count > bytes.size() - offset)
    {
        return;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::string_view FitUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
    {
        return text;
    }

    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
    {
        --end;
    }
    return text.substr(0, end);
}

} // namespace process_manager
