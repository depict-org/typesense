#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>
#include "collection_manager.h"
#include "collection.h"

class CollectionCuratedFilterTest : public ::testing::Test {
protected:
    Store* store = nullptr;
    CollectionManager& manager = CollectionManager::get_instance();
    std::atomic<bool> quit = false;
    std::string path;

    void SetUp() override {
        path = (std::filesystem::temp_directory_path() /
                ("typesense-curated-filter-" + std::to_string(getpid()))).string();
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        store = new Store(path);
        manager.init(store, 1.0, "auth_key", quit);
        manager.load(1, 1000);
        auto schema = R"({"name":"curated_filter","fields":[
            {"name":"group","type":"string","facet":true,"optional":true},
            {"name":"stock","type":"bool"},
            {"name":"market","type":"bool"},
            {"name":"rank","type":"int32"}]})"_json;
        auto created = manager.create_collection(schema);
        ASSERT_TRUE(created.ok()) << created.error();
        for (size_t i = 0; i < 64; ++i) {
            nlohmann::json doc = {{"id",std::to_string(i)}, {"group","g"+std::to_string(i/2)},
                {"stock",i%2==1}, {"market",true}, {"rank",64-i}};
            ASSERT_TRUE(created.get()->add(doc.dump()).ok());
        }
    }
    void TearDown() override {
        manager.dispose();
        delete store;
        std::filesystem::remove_all(path);
    }
    nlohmann::json search(std::map<std::string,std::string> params) {
        params["collection"] = "curated_filter";
        params["q"] = "*";
        params["sort_by"] = "rank:desc";
        nlohmann::json embedded;
        std::string response;
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto result = manager.do_search(params, embedded, response, now);
        EXPECT_TRUE(result.ok()) << result.error();
        return result.ok() ? nlohmann::json::parse(response) : nlohmann::json::object();
    }
};

TEST_F(CollectionCuratedFilterTest, SecondGroupedPassReceivesMaterializedFilter) {
    for (const auto& lazy : {"false","true"}) {
        SCOPED_TRACE(lazy);
        testing_curated_second_pass_entries = 0;
        testing_curated_second_pass_unmaterialized_entries = 0;
        auto result = search({{"group_by","group"},{"group_limit","1"},
            {"filter_by","stock:true && market:true"},{"filter_curated_hits","true"},
            {"pinned_hits","0:1,1:1,2:2,3:2,999:3,4:3,5:3"},
            {"per_page","3"},{"enable_lazy_filter",lazy}});
        ASSERT_TRUE(result.contains("grouped_hits"));
        ASSERT_FALSE(result["search_cutoff"].get<bool>());
        ASSERT_EQ(3,result["grouped_hits"].size());
        EXPECT_EQ("1",result["grouped_hits"][0]["hits"][0]["document"]["id"]);
        EXPECT_EQ("3",result["grouped_hits"][1]["hits"][0]["document"]["id"]);
        EXPECT_EQ("5",result["grouped_hits"][2]["hits"][0]["document"]["id"]);
        EXPECT_GT(testing_curated_second_pass_entries,0);
        EXPECT_EQ(0,testing_curated_second_pass_unmaterialized_entries);
    }
}

TEST_F(CollectionCuratedFilterTest, UnfilteredPinsKeepTheirOriginalVariants) {
    testing_curated_second_pass_entries = 0;
    auto result = search({{"group_by","group"},{"group_limit","1"},
        {"filter_by","stock:true"},{"filter_curated_hits","false"},
        {"pinned_hits","0:1,1:1,2:2,3:2"},{"per_page","2"}});
    ASSERT_EQ(2,result["grouped_hits"].size());
    EXPECT_EQ("0",result["grouped_hits"][0]["hits"][0]["document"]["id"]);
    EXPECT_EQ("2",result["grouped_hits"][1]["hits"][0]["document"]["id"]);
    EXPECT_EQ(0,testing_curated_second_pass_entries);
}

TEST_F(CollectionCuratedFilterTest, EmptyFilterDoesNotAdmitPins) {
    testing_curated_second_pass_entries = 0;
    auto result = search({{"group_by","group"},{"group_limit","1"},
        {"filter_by","stock:true && stock:false"},{"filter_curated_hits","true"},
        {"pinned_hits","0:1,1:1,2:2,3:2"},{"per_page","2"}});
    EXPECT_TRUE(result["grouped_hits"].empty());
    EXPECT_EQ(0,result["found"]);
    EXPECT_EQ(0,testing_curated_second_pass_entries);
}

TEST_F(CollectionCuratedFilterTest, UngroupedFilteringKeepsPinOrder) {
    testing_curated_second_pass_entries = 0;
    auto result = search({{"filter_by","stock:true && market:true"},
        {"filter_curated_hits","true"},{"pinned_hits","3:1,1:2"},{"per_page","2"}});
    ASSERT_EQ(2,result["hits"].size());
    EXPECT_EQ("3",result["hits"][0]["document"]["id"]);
    EXPECT_EQ("1",result["hits"][1]["document"]["id"]);
    EXPECT_EQ(0,testing_curated_second_pass_entries);
}
