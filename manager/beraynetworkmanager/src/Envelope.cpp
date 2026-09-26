#include "Envelope.hpp"
#include "ZmqSocket.hpp"

#include <cstddef>
#include <span>

namespace process_manager
{

EnvelopeCode ParseEnvelope(const Message& frames, Envelope& out)
{
    out = Envelope{};
    if (frames.empty() || frames[0].empty())
    {
        return EnvelopeCode::EmptySource;
    }
    out.source = frames[0];

    std::size_t index = 1;
    if (index < frames.size() && frames[index].empty())
    {
        ++index;
    }
    if (index >= frames.size() || frames[index].empty())
    {
        return EnvelopeCode::MissingDestination;
    }
    out.destination = frames[index];
    ++index;
    out.payload.assign(frames.begin() + static_cast<std::ptrdiff_t>(index), frames.end());
    return EnvelopeCode::Ok;
}

Message BuildEnvelope(const Frame& destination, const Frame& source, std::span<const Frame> payload)
{
    Message message{};
    message.reserve(payload.size() + 2);
    message.push_back(destination);
    message.push_back(source);
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

} // namespace process_manager
