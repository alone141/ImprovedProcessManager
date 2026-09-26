#pragma once

#include "ZmqSocket.hpp"

#include <span>

namespace process_manager
{

// The router's framing. A peer sends [destination][payload...]; the router's
// ROUTER socket prepends the sender, so the router reads
// [source][destination][payload...] (an empty delimiter after the source, as
// REQ sockets add one, is skipped) and sends [destination][source][payload...],
// which the destination's DEALER reads as [source][payload...].
struct Envelope
{
    Frame source;
    Frame destination;
    Message payload;
};

enum class EnvelopeCode
{
    Ok,
    EmptySource,
    MissingDestination,
};

/**
 * @brief Split a message the router received into its envelope.
 * @param frames [source]([empty])[destination][payload...]; the payload may be empty.
 * @param out Receives the envelope on success; on failure only the source is set.
 * @return EnvelopeCode::Ok, or what is missing.
 */
EnvelopeCode ParseEnvelope(const Message& frames, Envelope& out);

/**
 * @brief Build the message the router sends on.
 * @param destination Identity of the receiving peer.
 * @param source Identity of the sending peer, which the receiver sees first.
 * @param payload Frames delivered unchanged.
 * @return [destination][source][payload...].
 */
Message BuildEnvelope(const Frame& destination, const Frame& source, std::span<const Frame> payload);

} // namespace process_manager
