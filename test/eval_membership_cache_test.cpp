#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <functional>
#include "core_api.h"
#include <unistd.h>
#include "collection_manager.h"
#include "eval_membership_cache.h"
#include "filter_result_iterator.h"
#include "stemmer_manager.h"
#include "thread_local_vars.h"


#if defined(__linux__)
// Fail the actual disk-write boundary after in-memory indexing. The callback can
// observe that transient generation before Collection's real rollback runs.
static thread_local bool fail_eval_insert = false;
static thread_local bool fail_eval_batch = false;
static thread_local std::function<void()> before_eval_write_failure;
extern "C" bool __real__ZN5Store6insertERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_(
        Store*, const std::string&, const std::string&);
extern "C" bool __wrap__ZN5Store6insertERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_(
        Store* store, const std::string& key, const std::string& value) {
    if (fail_eval_insert) {
        fail_eval_insert = false;
        if (before_eval_write_failure) before_eval_write_failure();
        return false;
    }
    return __real__ZN5Store6insertERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES7_(store, key, value);
}
extern "C" bool __real__ZN5Store11batch_writeERN7rocksdb10WriteBatchE(Store*, rocksdb::WriteBatch&);
extern "C" bool __wrap__ZN5Store11batch_writeERN7rocksdb10WriteBatchE(Store* store, rocksdb::WriteBatch& batch) {
    if (fail_eval_batch) {
        fail_eval_batch = false;
        if (before_eval_write_failure) before_eval_write_failure();
        return false;
    }
    return __real__ZN5Store11batch_writeERN7rocksdb10WriteBatchE(store, batch);
}
#endif

TEST(EvalMembershipCache, CopiesAndAccountsWithoutAliasingLegs) {
    eval_membership_cache_t cache(64, 2);
    uint32_t original[] = {1, 3, 7};
    cache.admit(1, 0, "n:>0", original, 3);
    original[0] = 99;
    uint32_t* first = nullptr;
    uint32_t count = 0;
    ASSERT_TRUE(cache.copy(1, 0, "n:>0", first, count));
    std::unique_ptr<uint32_t[]> owned_first(first);
    ASSERT_EQ(3, count);
    EXPECT_EQ(1, first[0]);
    first[0] = 88;
    uint32_t* second = nullptr;
    ASSERT_TRUE(cache.copy(1, 0, "n:>0", second, count));
    std::unique_ptr<uint32_t[]> owned_second(second);
    EXPECT_NE(first, second);
    EXPECT_EQ(1, second[0]);
    EXPECT_EQ(17, cache.bytes());
    EXPECT_FALSE(cache.copy(2, 0, "n:>0", second, count));
    EXPECT_FALSE(cache.copy(1, 1, "n:>0", second, count));
    cache.prepare(1, 1);
    EXPECT_EQ(0, cache.size());
    EXPECT_EQ(0, cache.bytes());
    cache.admit(1, 1, "empty", nullptr, 0);
    uint32_t* empty = nullptr;
    ASSERT_TRUE(cache.copy(1, 1, "empty", empty, count));
    delete[] empty;
    EXPECT_EQ(0, count);
}

TEST(EvalMembershipCache, EntryAndPayloadBudgetsFallBack) {
    uint32_t ids[] = {1, 2, 3};
    eval_membership_cache_t cache(16, 1);
    cache.admit(1, 0, "12345", ids, 3);
    EXPECT_EQ(0, cache.size());
    cache.admit(1, 0, "1234", ids, 3);
    EXPECT_EQ(16, cache.bytes());
    cache.admit(2, 0, "x", nullptr, 0);
    EXPECT_EQ(1, cache.size());
    cache.prepare(1, 1);
    EXPECT_EQ(0, cache.bytes());
    cache.admit(2, 0, "x", nullptr, 0);
    EXPECT_EQ(1, cache.bytes());
    cache.prepare(1, 2);
    EXPECT_EQ(1, cache.size());
}

class EvalMembershipSearchTest : public ::testing::Test {
protected:
    Store* store = nullptr;
    CollectionManager& manager = CollectionManager::get_instance();
    Collection* coll = nullptr;
    std::atomic<bool> quit{false};
    std::string path;
    const std::string expression = "(n:>=4 && available:true) || tags:=red";

    void SetUp() override {
        path = (std::filesystem::temp_directory_path() /
                ("typesense-eval-cache-" + std::to_string(getpid()))).string();
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        store = new Store(path);
        manager.init(store, 1.0, "auth_key", quit);
        manager.load(1, 1000);
        auto schema = R"({"name":"eval_cache","enable_nested_fields":true,"fields":[
            {"name":"title","type":"string"}, {"name":"n","type":"int64"},
            {"name":"available","type":"bool"}, {"name":"tags","type":"string[]"},
            {"name":"group","type":"string","facet":true},
            {"name":"embedding","type":"float[]","num_dim":2},
            {"name":"f","type":"float"}, {"name":"numbers","type":"int32[]"},
            {"name":"hidden","type":"bool","index":false},
            {"name":"items","type":"object[]"}, {"name":"items.tags","type":"string[]"}]})"_json;
        auto created = manager.create_collection(schema);
        ASSERT_TRUE(created.ok()) << created.error();
        coll = created.get();
        for (size_t i = 0; i < 32; ++i) {
            nlohmann::json doc = {{"id", std::to_string(i)}, {"title", "alpha beta"},
                {"n", i}, {"available", i % 2 == 0}, {"tags", {i % 3 == 0 ? "red" : "blue"}},
                {"group", "g" + std::to_string(i / 2)}, {"embedding", {1.0, double(i + 1) / 32}},
                {"f", 1.5}, {"numbers", {1, 2}}, {"hidden", true},
                {"items", nlohmann::json::array({{{"tags", {"red"}}}})}};
            auto added = coll->add(doc.dump());
            ASSERT_TRUE(added.ok()) << added.error();
        }
    }

    void TearDown() override {
#if defined(__linux__)
        fail_eval_insert = false;
        fail_eval_batch = false;
        before_eval_write_failure = {};
#endif
        manager.dispose();
        delete store;
        std::filesystem::remove_all(path);
    }

    std::map<std::string, std::string> params(const std::string& predicate = "") {
        return {{"collection", "eval_cache"}, {"q", "*"}, {"query_by", "title"},
            {"sort_by", "_eval(" + (predicate.empty() ? expression : predicate) + "):desc,n:desc"},
            {"per_page", "32"}};
    }

    Option<bool> search(std::map<std::string, std::string> request, eval_membership_cache_t* cache,
                        nlohmann::json& result, const nlohmann::json& scoped = nlohmann::json()) {
        auto embedded = scoped;
        std::string text;
        auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        auto op = manager.do_search(request, embedded, text, now, cache);
        if (op.ok()) {
            result = nlohmann::json::parse(text);
            result.erase("search_time_ms");
            result.erase("search_time_us");
        }
        return op;
    }

    void expect_parity(const std::map<std::string, std::string>& request, eval_membership_cache_t& cache,
                       const nlohmann::json& scoped = nlohmann::json()) {
        nlohmann::json plain, cached;
        auto off = search(request, nullptr, plain, scoped);
        auto on = search(request, &cache, cached, scoped);
        ASSERT_EQ(off.ok(), on.ok()) << off.error() << "; " << on.error();
        if (off.ok()) {
            EXPECT_EQ(plain, cached);
        } else {
            EXPECT_EQ(off.code(), on.code());
            EXPECT_EQ(off.error(), on.error());
        }
    }

    void expect_invalidated(eval_membership_cache_t& cache) {
        size_t before = cache.admissions();
        size_t hits = cache.hits();
        expect_parity(params(), cache);
        EXPECT_EQ(before + 1, cache.admissions());
        EXPECT_EQ(hits, cache.hits());
        expect_parity(params(), cache);
        EXPECT_EQ(hits + 1, cache.hits());
        EXPECT_EQ(1, cache.size());
    }
};

TEST_F(EvalMembershipSearchTest, PreservesWholeResponsesAcrossLegContextsAndGroupedPasses) {
    eval_membership_cache_t cache;
    auto request = params();
    expect_parity(request, cache);
    EXPECT_EQ(1, cache.admissions());
    EXPECT_EQ(0, cache.hits());
    request["filter_by"] = "n:<12";
    request["group_by"] = "group";
    request["group_limit"] = "2";
    request["facet_by"] = "group";
    expect_parity(request, cache);
    EXPECT_EQ(1, cache.hits());
    request["sort_by"] = "_eval([(" + expression + "):3,(n:<8):8]):asc,n:desc";
    expect_parity(request, cache, {{"filter_by", "n:>=2"}});
    EXPECT_EQ(2, cache.admissions());
    EXPECT_EQ(2, cache.hits());
    request["sort_by"] = "_eval([(n:<8):99,(" + expression + "):1]):desc,n:desc";
    expect_parity(request, cache);
    EXPECT_EQ(4, cache.hits());
}

TEST_F(EvalMembershipSearchTest, VectorParityUsesOneFrozenIndexAndStableAA) {
    auto request = params();
    request["vector_query"] = "embedding:([1,0.5],k:20,flat_search_cutoff:0)";
    request["sort_by"] = "_eval(" + expression + "):desc,_vector_distance:asc";
    request["group_by"] = "group";
    request["group_limit"] = "2";
    nlohmann::json first, second;
    ASSERT_TRUE(search(request, nullptr, first).ok());
    ASSERT_TRUE(search(request, nullptr, second).ok());
    ASSERT_EQ(first, second);
    eval_membership_cache_t cache;
    expect_parity(request, cache);
    expect_parity(request, cache);
    EXPECT_EQ(1, cache.hits());
}

TEST_F(EvalMembershipSearchTest, CompleteEmptyIsReusable) {
    eval_membership_cache_t cache;
    expect_parity(params("n:>9999"), cache);
    expect_parity(params("n:>9999"), cache);
    EXPECT_EQ(1, cache.admissions());
    EXPECT_EQ(1, cache.hits());
    expect_parity(params("n:>9999 && available:true"), cache);
    expect_parity(params("n:>9999 && available:true"), cache);
    EXPECT_EQ(2, cache.admissions());
    EXPECT_EQ(2, cache.hits());
}

TEST_F(EvalMembershipSearchTest, RealInsertUpdateDeleteAndBulkWritesInvalidate) {
    eval_membership_cache_t cache;
    expect_parity(params(), cache);
    auto added = coll->add(R"({"id":"new","title":"alpha","n":999,"available":true,"tags":["red"],"group":"new","embedding":[1,0]})");
    ASSERT_TRUE(added.ok()) << added.error();
    expect_invalidated(cache);
    auto updated = coll->add(R"({"id":"new","available":false,"tags":["blue"]})", UPDATE);
    ASSERT_TRUE(updated.ok()) << updated.error();
    expect_invalidated(cache);
    auto removed = coll->remove("new");
    ASSERT_TRUE(removed.ok()) << removed.error();
    expect_invalidated(cache);
    std::vector<std::string> lines = {R"({"id":"1","available":true,"tags":["red"]})",
                                      R"({"id":"2","n":-1,"tags":["blue"]})"};
    nlohmann::json import_document;
    auto imported = coll->add_many(lines, import_document, UPDATE);
    ASSERT_EQ(2, imported["num_imported"]);
    expect_invalidated(cache);
    // One row fails validation; the successful row still changes membership.
    lines = {R"({"id":"1","available":false,"tags":["blue"]})", R"({"id":"2","n":"invalid"})"};
    imported = coll->add_many(lines, import_document, UPDATE);
    ASSERT_EQ(1, imported["num_imported"]);
    ASSERT_TRUE(nlohmann::json::parse(lines[0])["success"].get<bool>());
    ASSERT_FALSE(nlohmann::json::parse(lines[1])["success"].get<bool>());
    expect_invalidated(cache);
}

TEST_F(EvalMembershipSearchTest, SchemaChangesInvalidateAndKeepPerLegValidation) {
    eval_membership_cache_t cache;
    expect_parity(params(), cache);
    auto alter = R"({"fields":[{"name":"extra","type":"int64","optional":true}]})"_json;
    auto changed = coll->alter(alter);
    ASSERT_TRUE(changed.ok()) << changed.error();
    expect_invalidated(cache);
    alter = R"({"fields":[{"name":"tags","drop":true}]})"_json;
    changed = coll->alter(alter);
    ASSERT_TRUE(changed.ok()) << changed.error();
    nlohmann::json result;
    auto invalid = search(params(), &cache, result);
    ASSERT_FALSE(invalid.ok());
    expect_parity(params(), cache);
}

TEST_F(EvalMembershipSearchTest, AliasRetargetAndDropRecreateUseDistinctIncarnations) {
    eval_membership_cache_t cache;
    ASSERT_TRUE(manager.upsert_symlink("eval_alias", "eval_cache").ok());
    auto request = params(); request["collection"] = "eval_alias";
    expect_parity(request, cache);
    auto schema = coll->get_summary_json();
    schema["name"] = "other_cache";
    auto other = manager.create_collection(schema);
    ASSERT_TRUE(other.ok()) << other.error();
    ASSERT_TRUE(manager.upsert_symlink("eval_alias", "other_cache").ok());
    expect_parity(request, cache);
    EXPECT_EQ(2, cache.admissions());
    ASSERT_TRUE(manager.drop_collection("eval_cache").ok());
    schema["name"] = "eval_cache";
    auto replacement = manager.create_collection(schema);
    ASSERT_TRUE(replacement.ok()) << replacement.error(); coll = replacement.get();
    expect_parity(params(), cache);
    EXPECT_EQ(3, cache.admissions());
}

TEST_F(EvalMembershipSearchTest, UnsupportedLeavesAndCustomCutoffsBypass) {
    for (const auto& predicate : {"id:1", "title:=alpha beta", "tags:red", "tags:=r*", "f:>1", "numbers:1",
                                  "n:[1..9]", "items.tags:=red", "items.{tags:=red}", "hidden:true"}) {
        SCOPED_TRACE(predicate);
        eval_membership_cache_t cache;
        expect_parity(params(predicate), cache);
        expect_parity(params(predicate), cache);
        EXPECT_EQ(0, cache.admissions());
        EXPECT_EQ(0, cache.hits());
    }
    eval_membership_cache_t cache;
    auto request = params("missing:true"); request["validate_field_names"] = "false";
    expect_parity(request, cache);
    EXPECT_EQ(0, cache.admissions());
    request = params(); request["search_cutoff_ms"] = "30000";
    expect_parity(request, cache);
    EXPECT_EQ(0, cache.admissions());
    request.erase("search_cutoff_ms");
    expect_parity(request, cache);
    EXPECT_EQ(1, cache.admissions());
    request["search_cutoff_ms"] = "30000";
    expect_parity(request, cache);
    EXPECT_EQ(0, cache.hits());
}

TEST_F(EvalMembershipSearchTest, DictionaryBypassesAfterMutationAndTokenizerCacheEviction) {
    auto& stems = StemmerManager::get_instance();
    auto dictionary = stems.upsert_stemming_dictionary("eval_dictionary", {R"({"word":"scarlet","root":"red"})"});
    ASSERT_TRUE(dictionary.ok()) << dictionary.error();
    auto schema = R"({"name":"dictionary_cache","fields":[{"name":"tags","type":"string[]","stem_dictionary":"eval_dictionary"}]})"_json;
    auto created = manager.create_collection(schema);
    ASSERT_TRUE(created.ok()) << created.error();
    ASSERT_TRUE(created.get()->add(R"({"id":"0","tags":["red"]})").ok());
    eval_membership_cache_t cache;
    auto request = params("tags:=scarlet"); request["collection"] = "dictionary_cache";
    request["query_by"] = "tags"; request["sort_by"] = "_eval(tags:=scarlet):desc";
    expect_parity(request, cache);
    ASSERT_TRUE(stems.del_stemming_dictionary("eval_dictionary").ok());
    ASSERT_TRUE(stems.upsert_stemming_dictionary("eval_dictionary", {R"({"word":"scarlet","root":"blue"})"}).ok());
    auto stemmer = stems.get_stemmer("", "eval_dictionary");
    for (size_t i = 0; i < 64; ++i) {
        std::vector<std::string> tokens;
        const auto word = "intervening" + std::to_string(i);
        Tokenizer tokenizer(word, true, false, "", {}, {}, stemmer);
        tokenizer.tokenize(tokens);
    }
    expect_parity(request, cache);
    EXPECT_EQ(0, cache.admissions());
    EXPECT_EQ(0, cache.hits());
}

TEST_F(EvalMembershipSearchTest, BudgetErrorsAndIndependentRequestsKeepLifetimes) {
    eval_membership_cache_t exhausted(1, 1);
    expect_parity(params(), exhausted);
    expect_parity(params(), exhausted);
    EXPECT_EQ(0, exhausted.size());
    eval_membership_cache_t cache;
    expect_parity(params(), cache);
    auto invalid = params("missing:1");
    expect_parity(invalid, cache);
    expect_parity(params(), cache);
    EXPECT_EQ(1, cache.hits());
    auto run = [this]() {
        eval_membership_cache_t independent;
        nlohmann::json first, second;
        auto a = search(params(), &independent, first);
        auto b = search(params(), &independent, second);
        return a.ok() && b.ok() && first == second && independent.admissions() == 1 && independent.hits() == 1;
    };
    auto one = std::async(std::launch::async, run);
    auto two = std::async(std::launch::async, run);
    EXPECT_TRUE(one.get()); EXPECT_TRUE(two.get());
}

TEST_F(EvalMembershipSearchTest, IteratorCompletionRejectsTimeoutAndUninitialized) {
    filter_result_iterator_t uninitialized("", coll->_get_index(), nullptr, false);
    EXPECT_FALSE(uninitialized.materialization_complete());
    filter_node_t* tree = nullptr;
    auto parsed = filter::parse_filter_query("n:>=0 && tags:=red", coll->get_schema(), store, "", tree);
    ASSERT_TRUE(parsed.ok()) << parsed.error();
    std::unique_ptr<filter_node_t> owned(tree);
    filter_result_iterator_t expired("", coll->_get_index(), tree, false, DEFAULT_FILTER_BY_CANDIDATES, 0, 0);
    expired.compute_iterators();
    EXPECT_FALSE(expired.materialization_complete());
    filter_result_iterator_t complete("", coll->_get_index(), tree, false);
    complete.compute_iterators();
    EXPECT_TRUE(complete.materialization_complete());
}

#if defined(__linux__)
TEST_F(EvalMembershipSearchTest, DiskFailureRollbackInvalidatesTransientMembership) {
    eval_membership_cache_t cache;
    expect_parity(params(), cache);
    nlohmann::json original;
    ASSERT_TRUE(search(params(), nullptr, original).ok());
    size_t callback_count = 0;
    before_eval_write_failure = [&]() {
        ++callback_count;
        expect_invalidated(cache); // Observe the actual changed in-memory state.
    };
    fail_eval_insert = true;
    auto updated = coll->add(R"({"id":"31","available":true,"tags":["red"]})", UPDATE);
    ASSERT_FALSE(updated.ok());
    ASSERT_EQ(1, callback_count);
    expect_invalidated(cache); // The rollback must invalidate the transient entry.
    nlohmann::json restored;
    ASSERT_TRUE(search(params(), nullptr, restored).ok());
    EXPECT_EQ(original, restored);
    fail_eval_batch = true;
    auto inserted = coll->add(R"({"id":"rollback","title":"alpha","n":999,"available":true,"tags":["red"],"group":"rollback","embedding":[1,0]})");
    ASSERT_FALSE(inserted.ok());
    ASSERT_EQ(2, callback_count);
    expect_invalidated(cache);
    ASSERT_TRUE(search(params(), nullptr, restored).ok());
    EXPECT_EQ(original, restored);
}
#endif

TEST_F(EvalMembershipSearchTest, OrdinaryMultiSearchKeepsLegResponsesAndErrorIsolation) {
    nlohmann::json body;
    body["searches"] = nlohmann::json::array();
    nlohmann::json expected = nlohmann::json::array();
    for (size_t i = 0; i < 5; ++i) {
        auto request = params();
        if (i == 1) request["filter_by"] = "n:<8";
        if (i == 2) request["sort_by"] = "_eval(missing:1):desc";
        if (i == 3) { request["group_by"] = "group"; request["group_limit"] = "2"; }
        nlohmann::json one;
        auto op = search(request, nullptr, one);
        if (!op.ok()) one = {{"error", op.error()}, {"code", op.code()}};
        expected.push_back(one);
        body["searches"].push_back(request);
    }
    // Each call owns and destroys its own real stack context in post_multi_search.
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        auto req = std::make_shared<http_req>();
        auto res = std::make_shared<http_res>(nullptr);
        req->body = body.dump();
        req->embedded_params_vec.resize(5);
        ASSERT_TRUE(post_multi_search(req, res));
        auto response = nlohmann::json::parse(res->body);
        ASSERT_EQ(5, response["results"].size());
        for (size_t i = 0; i < 5; ++i) {
            auto& one = response["results"][i];
            one.erase("search_time_ms"); one.erase("search_time_us");
            EXPECT_EQ(expected[i], one) << i;
        }
    }
}
