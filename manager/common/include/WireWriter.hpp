#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace process_manager
{

class WireWriter
{
public:
    /**
     * @brief Start a zero-filled buffer. Values are stored little-endian at fixed offsets.
     * @param size Buffer size in bytes.
     */
    explicit WireWriter(std::size_t size);

    /**
     * @brief Store one byte.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutU8(std::size_t offset, std::uint8_t value);

    /**
     * @brief Store an unsigned 16-bit value.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutU16(std::size_t offset, std::uint16_t value);

    /**
     * @brief Store an unsigned 32-bit value.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutU32(std::size_t offset, std::uint32_t value);

    /**
     * @brief Store an unsigned 64-bit value.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutU64(std::size_t offset, std::uint64_t value);

    /**
     * @brief Store a signed 32-bit value in two's complement.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutI32(std::size_t offset, std::int32_t value);

    /**
     * @brief Store a signed 64-bit value in two's complement.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutI64(std::size_t offset, std::int64_t value);

    /**
     * @brief Store an IEEE 754 double.
     * @param offset Byte offset. A write past the end is dropped.
     * @param value Value to store.
     */
    void PutF64(std::size_t offset, double value);

    /**
     * @brief Store text in a fixed, NUL-padded field.
     * @param offset Byte offset of the field.
     * @param fieldSize Field size in bytes. The last byte always stays NUL.
     * @param text UTF-8 text. It is cut at a character boundary to fit.
     */
    void PutText(std::size_t offset, std::size_t fieldSize, std::string_view text);

    /**
     * @brief The buffer written so far.
     * @return The bytes, one per offset; the view is valid until the writer changes or goes away.
     */
    std::span<const std::uint8_t> Bytes() const;

    /**
     * @brief Hand the buffer over. The writer is empty afterwards.
     * @return The bytes, one per offset.
     */
    std::vector<std::uint8_t> TakeBytes();

private:
    void PutBytes(std::size_t offset, std::uint64_t value, std::size_t count);

    std::vector<std::uint8_t> bytes;
};

/**
 * @brief Cut UTF-8 text to a byte budget without splitting a character.
 * @param text UTF-8 text.
 * @param maxBytes Largest allowed size in bytes.
 * @return The longest prefix of @p text that fits and ends on a character boundary.
 */
std::string_view FitUtf8(std::string_view text, std::size_t maxBytes);

} // namespace process_manager
