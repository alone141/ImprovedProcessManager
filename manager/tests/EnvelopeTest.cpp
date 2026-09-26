#include <gtest/gtest.h>

#include "Envelope.hpp"
#include "ZmqSocket.hpp"

#include <string>

namespace
{

process_manager::Frame Text(const std::string& text)
{
    return process_manager::MakeFrame(text);
}

} // namespace

TEST(EnvelopeTest, ParsesWhatADealerSends)
{
    // The ROUTER prepended "auth"; the dealer sent ["ledger", "hello"].
    const process_manager::Message frames{Text("auth"), Text("ledger"), Text("hello")};
    process_manager::Envelope envelope{};
    ASSERT_EQ(process_manager::ParseEnvelope(frames, envelope), process_manager::EnvelopeCode::Ok);
    EXPECT_TRUE(process_manager::FrameIs(envelope.source, "auth"));
    EXPECT_TRUE(process_manager::FrameIs(envelope.destination, "ledger"));
    ASSERT_EQ(envelope.payload.size(), 1u);
    EXPECT_TRUE(process_manager::FrameIs(envelope.payload[0], "hello"));
}

TEST(EnvelopeTest, SkipsARequestStyleDelimiter)
{
    const process_manager::Message frames{Text("auth"), process_manager::Frame{}, Text("ledger"), Text("hello")};
    process_manager::Envelope envelope{};
    ASSERT_EQ(process_manager::ParseEnvelope(frames, envelope), process_manager::EnvelopeCode::Ok);
    EXPECT_TRUE(process_manager::FrameIs(envelope.destination, "ledger"));
    ASSERT_EQ(envelope.payload.size(), 1u);
    EXPECT_TRUE(process_manager::FrameIs(envelope.payload[0], "hello"));
}

TEST(EnvelopeTest, KeepsEveryPayloadFrameInOrder)
{
    const process_manager::Message frames{Text("A"), Text("B"), Text("p1"), process_manager::Frame{}, Text("p3")};
    process_manager::Envelope envelope{};
    ASSERT_EQ(process_manager::ParseEnvelope(frames, envelope), process_manager::EnvelopeCode::Ok);
    ASSERT_EQ(envelope.payload.size(), 3u);
    EXPECT_TRUE(process_manager::FrameIs(envelope.payload[0], "p1"));
    EXPECT_TRUE(envelope.payload[1].empty());
    EXPECT_TRUE(process_manager::FrameIs(envelope.payload[2], "p3"));
}

TEST(EnvelopeTest, AllowsAnEmptyPayload)
{
    process_manager::Envelope envelope{};
    ASSERT_EQ(process_manager::ParseEnvelope(process_manager::Message{Text("A"), Text("B")}, envelope),
              process_manager::EnvelopeCode::Ok);
    EXPECT_TRUE(envelope.payload.empty());
}

TEST(EnvelopeTest, RejectsAMissingOrEmptyDestination)
{
    process_manager::Envelope envelope{};
    EXPECT_EQ(process_manager::ParseEnvelope(process_manager::Message{Text("A")}, envelope),
              process_manager::EnvelopeCode::MissingDestination);
    EXPECT_TRUE(process_manager::FrameIs(envelope.source, "A")); // known, for the log line
    EXPECT_EQ(process_manager::ParseEnvelope(process_manager::Message{Text("A"), process_manager::Frame{}}, envelope),
              process_manager::EnvelopeCode::MissingDestination);
    EXPECT_EQ(process_manager::ParseEnvelope(
                  process_manager::Message{Text("A"), process_manager::Frame{}, process_manager::Frame{}}, envelope),
              process_manager::EnvelopeCode::MissingDestination);
}

TEST(EnvelopeTest, RejectsAnEmptySource)
{
    process_manager::Envelope envelope{};
    EXPECT_EQ(process_manager::ParseEnvelope(process_manager::Message{}, envelope),
              process_manager::EnvelopeCode::EmptySource);
    EXPECT_EQ(process_manager::ParseEnvelope(process_manager::Message{process_manager::Frame{}, Text("B"), Text("x")},
                                             envelope),
              process_manager::EnvelopeCode::EmptySource);
}

TEST(EnvelopeTest, BuildsDestinationThenSourceThenPayload)
{
    const process_manager::Message payload{Text("p1"), Text("p2")};
    const process_manager::Message outbound = process_manager::BuildEnvelope(Text("ledger"), Text("auth"), payload);
    ASSERT_EQ(outbound.size(), 4u);
    EXPECT_TRUE(process_manager::FrameIs(outbound[0], "ledger"));
    EXPECT_TRUE(process_manager::FrameIs(outbound[1], "auth"));
    EXPECT_TRUE(process_manager::FrameIs(outbound[2], "p1"));
    EXPECT_TRUE(process_manager::FrameIs(outbound[3], "p2"));

    const process_manager::Message bare = process_manager::BuildEnvelope(Text("B"), Text("A"), process_manager::Message{});
    ASSERT_EQ(bare.size(), 2u);
}
