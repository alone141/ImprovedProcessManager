#include "WireReader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace process_manager
{

WireReader::WireReader(std::span<const std::uint8_t> bytes, std::size_t base, std::size_t size)
    : bytes{bytes}, base{base}, size{size}
{
}

std::size_t WireReader::Size() const
{
    if (base >= bytes.size())
    {
        return 0;
    }

    return std::min(size, bytes.size() - base);
}

std::uint8_t WireReader::U8(std::size_t offset) const
{
    return static_cast<std::uint8_t>(GetBytes(offset, 1));
}

std::uint16_t WireReader::U16(std::size_t offset) const
{
    return static_cast<std::uint16_t>(GetBytes(offset, 2));
}

std::uint32_t WireReader::U32(std::size_t offset) const
{
    return static_cast<std::uint32_t>(GetBytes(offset, 4));
}

std::uint64_t WireReader::U64(std::size_t offset) const
{
    return GetBytes(offset, 8);
}

std::int32_t WireReader::I32(std::size_t offset) const
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(GetBytes(offset, 4)));
}

std::int64_t WireReader::I64(std::size_t offset) const
{
    return static_cast<std::int64_t>(GetBytes(offset, 8));
}

double WireReader::F64(std::size_t offset) const
{
    const std::uint64_t raw = GetBytes(offset, 8);
    double value{0.0};
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::string WireReader::Text(std::size_t offset, std::size_t fieldSize) const
{
    std::string text{};
    const std::size_t available = Size();
    for (std::size_t i = 0; i < fieldSize && offset + i < available; ++i)
    {
        const std::uint8_t byte = bytes[base + offset + i];
        if (byte == 0)
        {
            break;
        }
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

std::uint64_t WireReader::GetBytes(std::size_t offset, std::size_t count) const
{
    const std::size_t available = Size();
    if (offset > available || count > available - offset)
    {
        return 0;
    }

    std::uint64_t value{0};
    for (std::size_t i = 0; i < count; ++i)
    {
        value |= static_cast<std::uint64_t>(bytes[base + offset + i]) << (8 * i);
    }
    return value;
}

} // namespace process_manager
