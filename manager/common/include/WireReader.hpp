#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace process_manager
{

enum class DecodeCode
{
    Ok,
    BadLength,
    BadMagic,
    UnsupportedVersion,
    BadLayout,
};

class WireReader
{
public:
    /**
     * @brief Read little-endian fields from part of a buffer.
     * @param bytes Buffer to read. It must outlive the reader.
     * @param base Offset of the part within @p bytes.
     * @param size Size of the part. Reads past it return zero.
     */
    WireReader(std::span<const std::uint8_t> bytes, std::size_t base, std::size_t size);

    /**
     * @brief Size of the part being read.
     * @return Size in bytes, clipped to the buffer.
     */
    std::size_t Size() const;

    /**
     * @brief Read one byte.
     * @param offset Offset within the part.
     * @return The byte, or zero past the end.
     */
    std::uint8_t U8(std::size_t offset) const;

    /**
     * @brief Read an unsigned 16-bit value.
     * @param offset Offset within the part.
     * @return The value, or zero past the end.
     */
    std::uint16_t U16(std::size_t offset) const;

    /**
     * @brief Read an unsigned 32-bit value.
     * @param offset Offset within the part.
     * @return The value, or zero past the end.
     */
    std::uint32_t U32(std::size_t offset) const;

    /**
     * @brief Read an unsigned 64-bit value.
     * @param offset Offset within the part.
     * @return The value, or zero past the end.
     */
    std::uint64_t U64(std::size_t offset) const;

    /**
     * @brief Read a signed 32-bit value.
     * @param offset Offset within the part.
     * @return The value, or zero past the end.
     */
    std::int32_t I32(std::size_t offset) const;

    /**
     * @brief Read a signed 64-bit value.
     * @param offset Offset within the part.
     * @return The value, or zero past the end.
     */
    std::int64_t I64(std::size_t offset) const;

    /**
     * @brief Read an IEEE 754 double.
     * @param offset Offset within the part.
     * @return The value, or zero past the end.
     */
    double F64(std::size_t offset) const;

    /**
     * @brief Read text from a fixed, NUL-padded field.
     * @param offset Offset of the field within the part.
     * @param fieldSize Field size in bytes.
     * @return The bytes before the first NUL, or the whole field when it has none.
     */
    std::string Text(std::size_t offset, std::size_t fieldSize) const;

private:
    std::uint64_t GetBytes(std::size_t offset, std::size_t count) const;

    std::span<const std::uint8_t> bytes;
    std::size_t base;
    std::size_t size;
};

} // namespace process_manager
