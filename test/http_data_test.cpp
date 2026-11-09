#include <gtest/gtest.h>
#include "http_data.h"

TEST(HttpDataTest, StreamedUtf8SurvivesEveryByteBoundary) {
    // Includes two-, three- and four-byte code points and a JSONL boundary.
    const std::string body = u8"{\"id\":\"1\",\"text\":\"åäöéü€漢🙂\"}\n{\"id\":\"2\",\"text\":\"🚀\"}";
    for (size_t split = 1; split < body.size(); ++split) {
        SCOPED_TRACE(split);
        http_req assembled;
        assembled.body.clear();
        for (size_t part = 0; part < 2; ++part) {
            http_req request;
            request.first_chunk_aggregate = false;
            request.last_chunk_aggregate = (part == 1);
            request.body = part == 0 ? body.substr(0, split) : body.substr(split);

            const auto serialized = request.to_json();
            const auto envelope = nlohmann::json::parse(serialized);
            // The existing decoder used by older nodes must understand it.
            const std::string decoded_body = envelope["is_binary_body"].get<bool>()
                ? StringUtils::base64_decode(envelope["body"])
                : envelope["body"].get<std::string>();
            ASSERT_EQ(request.body, decoded_body);

            // Raft deserialization and the indexer's persisted second hop.
            http_req forwarded;
            forwarded.load_from_json(serialized);
            assembled.load_from_json(forwarded.to_json());
        }
        ASSERT_EQ(body, assembled.body);
        ASSERT_FALSE(nlohmann::json::parse(assembled.body.substr(0, assembled.body.find('\n'))).is_discarded());
    }
}

TEST(HttpDataTest, CompleteTextAndBinaryBodiesRemainCompatible) {
    http_req complete;
    complete.last_chunk_aggregate = true;
    complete.body = u8"{\"text\":\"å€🙂\"}";
    auto envelope = nlohmann::json::parse(complete.to_json());
    ASSERT_FALSE(envelope["is_binary_body"].get<bool>());
    ASSERT_EQ(complete.body, envelope["body"].get<std::string>());

    http_req decoded;
    decoded.load_from_json(complete.to_json());
    ASSERT_EQ(complete.body, decoded.body);

    complete.is_binary_body = true;
    complete.body = std::string("\0\xff\xc3", 3);
    envelope = nlohmann::json::parse(complete.to_json());
    ASSERT_TRUE(envelope["is_binary_body"].get<bool>());
    http_req binary;
    binary.load_from_json(complete.to_json());
    ASSERT_EQ(complete.body, binary.body);
}

TEST(HttpDataTest, LoadsLegacyPlainTextEnvelopes) {
    const std::string text = u8"{\"text\":\"åäöéü\"}";
    nlohmann::json legacy = {{"route_hash", 1}, {"params", nlohmann::json::object()}, {"body", text}};
    http_req decoded;
    decoded.body = "discarded legacy prefix";
    decoded.start_ts = 0;
    decoded.load_from_json(legacy.dump());
    ASSERT_EQ(text, decoded.body);
    ASSERT_FALSE(decoded.is_binary_body);
}
