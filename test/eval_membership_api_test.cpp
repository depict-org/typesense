// This file deliberately uses only the pre-cache API, plus a test-only engine
// observation. It can run behaviorally against base with observation-only code.
#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>
#include "collection_manager.h"
#include "core_api.h"

extern thread_local size_t testing_eval_membership_builds;

class EvalMembershipAPITest : public ::testing::Test {
protected:
    Store* store = nullptr;
    CollectionManager& manager = CollectionManager::get_instance();
    std::atomic<bool> quit{false};
    std::string path;
    void SetUp() override {
        path = (std::filesystem::temp_directory_path() /
                ("typesense-eval-api-" + std::to_string(getpid()))).string();
        std::filesystem::remove_all(path); std::filesystem::create_directories(path);
        store = new Store(path); manager.init(store, 1.0, "auth_key", quit); manager.load(1, 1000);
        auto schema = R"({"name":"eval_api","fields":[{"name":"title","type":"string"},
            {"name":"n","type":"int64"},{"name":"group","type":"string","facet":true}]})"_json;
        auto created = manager.create_collection(schema);
        ASSERT_TRUE(created.ok()) << created.error();
        for (size_t i = 0; i < 32; ++i) {
            auto added = created.get()->add(nlohmann::json({{"id",std::to_string(i)}, {"title","alpha"},
                    {"n",i}, {"group","g"+std::to_string(i/2)}}).dump());
            ASSERT_TRUE(added.ok()) << added.error();
        }
    }
    void TearDown() override { manager.dispose(); delete store; std::filesystem::remove_all(path); }
};

TEST_F(EvalMembershipAPITest, RealMultiSearchAvoidsBuildsAndResetsForEachRequest) {
    std::string eval = "_eval([";
    for (size_t i = 0; i < 15; ++i) eval += (i ? "," : "") + std::string("(n:=") + std::to_string(i) + "):" + std::to_string(100-i);
    eval += "]):desc,n:desc";
    // Same materialization counts as the complete 2/2/5-leg captured shapes.
    for (const size_t shape : {size_t(0),size_t(1),size_t(2)}) {
        const size_t legs = shape == 2 ? 5 : 2;
        const size_t uncached_builds = shape == 2 ? 60 : 30;
        nlohmann::json body; body["searches"] = nlohmann::json::array();
        nlohmann::json expected = nlohmann::json::array();
        testing_eval_membership_builds = 0;
        for (size_t i = 0; i < legs; ++i) {
            std::map<std::string,std::string> request = {{"collection","eval_api"},{"q","*"},
                {"query_by","title"},{"sort_by",shape==2&&i==0?"n:desc":eval},{"per_page","32"}};
            if (i % 2) {request["group_by"]="group"; request["group_limit"]="2";}
            if (shape == 1) request["filter_by"] = i % 2 ? "n:<16" : "n:>=8";
            nlohmann::json embedded; std::string result;
            auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            auto op = manager.do_search(request, embedded, result, now);
            ASSERT_TRUE(op.ok()) << op.error();
            auto one = nlohmann::json::parse(result); one.erase("search_time_ms"); one.erase("search_time_us");
            expected.push_back(one); body["searches"].push_back(request);
        }
        ASSERT_EQ(uncached_builds, testing_eval_membership_builds);
        for (size_t repeat = 0; repeat < 2; ++repeat) {
            auto req=std::make_shared<http_req>(); auto res=std::make_shared<http_res>(nullptr);
            req->body=body.dump(); req->embedded_params_vec.resize(legs);
            testing_eval_membership_builds=0;
            ASSERT_TRUE(post_multi_search(req,res));
            // Fails if the real &eval_cache forwarding is removed. A global cache
            // also fails on the second invocation, which must build afresh.
            EXPECT_EQ(15,testing_eval_membership_builds) << "shape="<<shape<<", request="<<repeat;
            auto response=nlohmann::json::parse(res->body);
            ASSERT_EQ(legs,response["results"].size());
            for(size_t i=0;i<legs;++i) {
                response["results"][i].erase("search_time_ms");response["results"][i].erase("search_time_us");
                EXPECT_EQ(expected[i],response["results"][i]) << i;
            }
        }
    }
}
