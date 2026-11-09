#include <gtest/gtest.h>
#include "http_data.h"
#include "batched_indexer.h"
#include <filesystem>
#include <cstdlib>
#include <thread>

namespace {
// Lets the same byte-loss regression run against the pre-fix header, whose
// producer performed no UTF-8 preparation before serializing each chunk.
template<typename Request>
auto prepare_body(Request& request, int) -> decltype(request.prepare_body_for_serialization()) {
    return request.prepare_body_for_serialization();
}
template<typename Request>
bool prepare_body(Request&, long) {
    return true;
}
}

TEST(HttpDataTest, StreamedUtf8SurvivesEveryByteBoundary) {
    // Includes two-, three- and four-byte code points and a JSONL boundary.
    const std::string body = u8"{\"id\":\"1\",\"text\":\"åäöéü€漢🙂\"}\n{\"id\":\"2\",\"text\":\"🚀\"}";
    for (size_t split = 1; split < body.size(); ++split) {
        SCOPED_TRACE(split);
        http_req assembled;
        assembled.body.clear();
        http_req request;
        for (size_t part = 0; part < 2; ++part) {
            request.first_chunk_aggregate = false;
            request.last_chunk_aggregate = (part == 1);
            request.body = part == 0 ? body.substr(0, split) : body.substr(split);
            ASSERT_TRUE(prepare_body(request, 0));

            const auto serialized = request.to_json();
            const auto envelope = nlohmann::json::parse(serialized);
            // The existing decoder used by older nodes must understand it.
            const std::string decoded_body = envelope["is_binary_body"].get<bool>()
                ? StringUtils::base64_decode(envelope["body"])
                : envelope["body"].get<std::string>();
            ASSERT_EQ(request.body, decoded_body);

            // The old snapshot field stores the pending record as plain JSON.
            nlohmann::json snapshot = {{"prev_req_body", request.body}};
            ASSERT_NO_THROW(snapshot.dump());

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
    ASSERT_TRUE(prepare_body(complete, 0));
    auto envelope = nlohmann::json::parse(complete.to_json());
    ASSERT_FALSE(envelope["is_binary_body"].get<bool>());
    ASSERT_EQ(complete.body, envelope["body"].get<std::string>());

    http_req decoded;
    decoded.load_from_json(complete.to_json());
    ASSERT_EQ(complete.body, decoded.body);

    complete.is_binary_body = true;
    complete.body = std::string("\0\xff\xc3", 3);
    ASSERT_TRUE(prepare_body(complete, 0));
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

TEST(HttpDataTest, AllMultibyteBodyWithOneByteChunks) {
    const std::string body = u8"åäöéü€漢🙂🚀";
    http_req producer;
    http_req decoded;
    for (size_t offset = 0; offset < body.size(); ++offset) {
        producer.first_chunk_aggregate = false;
        producer.last_chunk_aggregate = (offset + 1 == body.size());
        producer.body = body.substr(offset, 1);
        ASSERT_TRUE(prepare_body(producer, 0));
        decoded.load_from_json(producer.to_json());
        ASSERT_LE(offset + 1 - decoded.body.size(), 3);
    }
    ASSERT_EQ(body, decoded.body);
}

TEST(HttpDataTest, IncompleteFinalCodePointIsRejected) {
    for (const std::string character : {u8"å", u8"€", u8"🙂"}) {
        for (size_t split = 1; split < character.size(); ++split) {
            http_req producer;
            producer.body = character.substr(0, split);
            producer.last_chunk_aggregate = false;
            ASSERT_TRUE(prepare_body(producer, 0));
            producer.body.clear();
            producer.last_chunk_aggregate = true;
            ASSERT_FALSE(prepare_body(producer, 0));
        }
    }
}

TEST(HttpDataTest, InvalidInteriorUtf8KeepsExistingSerializationBehavior) {
    http_req request;
    request.body = std::string("{\"text\":\"a") + char(0xff) + "b\"}";
    request.last_chunk_aggregate = true;
    const auto original_body = request.body;
    ASSERT_TRUE(prepare_body(request, 0));
    ASSERT_EQ(original_body, request.body);
    http_req decoded;
    decoded.load_from_json(request.to_json());
    ASSERT_EQ("{\"text\":\"ab\"}", decoded.body);
}

TEST(HttpDataTest, SnapshotStateAndLegacyReaderPreserveSplitCharacters) {
    struct Scratch {
        std::filesystem::path path;
        explicit Scratch(const std::string& suffix) {
            const char* configured = std::getenv("TSUTF8_TEST_ROOT");
            path = (configured ? std::filesystem::path(configured) :
                    std::filesystem::temp_directory_path() / "typesense_utf8_snapshot") /
                   (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + suffix);
            std::filesystem::create_directories(path);
        }
        ~Scratch() { std::filesystem::remove_all(path); }
    };
    struct SnapshotIndexer {
        Scratch scratch;
        Store store;
        Store meta;
        std::atomic<bool> skip{false};
        BatchedIndexer indexer;
        std::thread runner;
        explicit SnapshotIndexer(const std::string& suffix): scratch(suffix),
            store((scratch.path / "store").string()), meta((scratch.path / "meta").string()),
            indexer(nullptr, &store, &meta, 0, Config::get_instance(), skip),
            runner([this] { indexer.run(); }) {}
        ~SnapshotIndexer() { indexer.stop(); runner.join(); }
    };

    SnapshotIndexer before("before"), restored("restored");
    nlohmann::json state = {{"queued_writes", 0}, {"req_res_map", nlohmann::json::object()}};
    std::vector<std::pair<std::string, std::string>> continuation_and_expected;
    uint64_t id = 1;
    for (const std::string character : {u8"å", u8"€", u8"🙂"}) {
        for (size_t split = 1; split < character.size(); ++split) {
            const std::string prefix = "{\"id\":\"1\",\"text\":\"";
            const std::string suffix = "\"}";
            http_req producer;
            producer.start_ts = id;
            producer.first_chunk_aggregate = false;
            producer.last_chunk_aggregate = false;
            producer.body = prefix + character.substr(0, split);
            ASSERT_TRUE(prepare_body(producer, 0));
            // Seed the partial-record boundary through the actual state loader.
            // No indexing workers run while the snapshot is saved/restored.
            state["req_res_map"][std::to_string(id)] = {
                {"start_ts", id}, {"last_updated", 1}, {"num_chunks", 1},
                {"next_chunk_index", 1}, {"is_complete", false},
                {"req", producer.to_json()}, {"prev_req_body", producer.body}};
            producer.body = character.substr(split) + suffix;
            producer.last_chunk_aggregate = true;
            ASSERT_TRUE(prepare_body(producer, 0));
            continuation_and_expected.emplace_back(producer.to_json(), prefix + character + suffix);
            ++id;
        }
    }

    before.indexer.load_state(state);
    nlohmann::json saved;
    before.indexer.serialize_state(saved);
    std::string serialized;
    ASSERT_NO_THROW(serialized = saved.dump());
    restored.indexer.load_state(nlohmann::json::parse(serialized));
    nlohmann::json saved_again;
    restored.indexer.serialize_state(saved_again);
    ASSERT_EQ(saved, saved_again);

    for (size_t i = 0; i < continuation_and_expected.size(); ++i) {
        // This is also how the unchanged old snapshot reader resumes: the
        // pending record is literal text, followed by the next log entry.
        http_req resumed;
        resumed.body = saved_again["req_res_map"][std::to_string(i + 1)]["prev_req_body"].get<std::string>();
        resumed.load_from_json(continuation_and_expected[i].first);
        ASSERT_EQ(continuation_and_expected[i].second, resumed.body);
        ASSERT_EQ("1", nlohmann::json::parse(resumed.body)["id"].get<std::string>());
    }
}
