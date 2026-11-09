// Linux native targets wrap the clock and array allocation symbols.
// The seam is idle outside these tests and crosses the unchanged 30s deadline.
#include <gtest/gtest.h>
#include <filesystem>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include "collection_manager.h"
#include "collection.h"
#include "filter.h"
#include "thread_local_vars.h"

using Clock = std::chrono::system_clock;
#if defined(__linux__)
static thread_local bool scripted_clock = false;
static thread_local bool eval_cache_timeout_probe = false;
static thread_local size_t eval_cache_clock_calls = 0;
static thread_local size_t eval_cache_expire_after = 0;
static thread_local size_t clock_calls = 0;
static thread_local bool advance_clock = false;
static thread_local bool full_search_probe = false;
static thread_local uint64_t clock_base_us = 1000000;
static void arm_boundary_clock() {
    scripted_clock=true;advance_clock=true;clock_calls=0;
}
static thread_local bool track_arrays = false;
static thread_local bool observe_array_lifetimes = false;
static thread_local std::vector<std::pair<void*,size_t>> arrays;
extern "C" void* __real__Znam(size_t);
extern "C" void __real__ZdaPv(void*);
extern "C" void __real__ZdaPvm(void*,size_t);
extern "C" void* __wrap__Znam(size_t n) {
    void* p=__real__Znam(n);
    if(track_arrays)arrays.emplace_back(p,n);
    return p;
}
static void forget_array(void* p) {
    arrays.erase(std::remove_if(arrays.begin(),arrays.end(),[p](const auto& a){return a.first==p;}),arrays.end());
}
extern "C" void __wrap__ZdaPv(void* p) {if(observe_array_lifetimes)forget_array(p);__real__ZdaPv(p);}
extern "C" void __wrap__ZdaPvm(void* p,size_t n) {if(observe_array_lifetimes)forget_array(p);__real__ZdaPvm(p,n);}
extern "C" Clock::time_point __real__ZNSt6chrono3_V212system_clock3nowEv();
extern "C" Clock::time_point __wrap__ZNSt6chrono3_V212system_clock3nowEv() {
    if (eval_cache_timeout_probe) {
        const bool expired = testing_eval_membership_builds > 0 &&
                             eval_cache_clock_calls++ >= eval_cache_expire_after;
        return Clock::time_point(std::chrono::microseconds(clock_base_us + (expired ? 30000001 : 1)));
    }
    if(full_search_probe && !scripted_clock && testing_curated_second_pass_entries>0)arm_boundary_clock();
    if (!scripted_clock) return __real__ZNSt6chrono3_V212system_clock3nowEv();
    // Initialization stays below cutoff. Once armed, the unchanged 30,000ms
    // deadline has already crossed before the first clock checkpoint.
    if (advance_clock) ++clock_calls;
    const uint64_t us = clock_base_us + (advance_clock ? 30000001 : 1);
    return Clock::time_point(std::chrono::microseconds(us));
}

class CuratedFilterDeadlineTest : public ::testing::Test {
protected:
    Store* store = nullptr;
    CollectionManager& manager = CollectionManager::get_instance();
    std::atomic<bool> quit = false;
    Collection* coll = nullptr;
    std::string path;
    nlohmann::json document_7;
    void SetUp() override {
        // Direct iterator probes have their own timeout info. Prefix matching
        // also consults these query-local globals, which a prior search sets.
        search_begin_us=0;search_stop_us=UINT64_MAX;search_cutoff=false;
        path = (std::filesystem::temp_directory_path() /
                ("typesense-curated-deadline-" + std::to_string(getpid()))).string();
        std::filesystem::create_directories(path);
        store = new Store(path);
        manager.init(store, 1.0, "auth_key", quit);
        manager.load(1,1000);
        auto schema = R"({"name":"review_probe","enable_nested_fields":true,"fields":[
            {"name":"group","type":"string","facet":true},
            {"name":"n","type":"int32"},{"name":"f","type":"float"},
            {"name":"b","type":"bool"},{"name":"c","type":"bool","facet":true},
            {"name":"s","type":"string"},{"name":"tags","type":"string[]"},
            {"name":"geo","type":"geopoint"},
            {"name":"items","type":"object[]"},
            {"name":"items.color","type":"string[]"},{"name":"items.size","type":"int32[]"},
            {"name":"outer","type":"object"},{"name":"outer.items","type":"object[]"},
            {"name":"outer.items.color","type":"string[]"},{"name":"outer.items.size","type":"int32[]"}]})"_json;
        auto op = manager.create_collection(schema);
        ASSERT_TRUE(op.ok()) << op.error(); coll=op.get();
        for(uint32_t i=0;i<2048;++i) {
            nlohmann::json d={{"id",std::to_string(i)},{"group","g"+std::to_string(i/2)},
                {"n",i%16},{"f",double(i%16)+0.5},{"b",true},{"c",true},
                {"s",i%3==0?"alpha beta":"alpha gamma"},
                {"tags",{"alpha beta",i%2?"red":"blue"}},
                {"geo",{51.5+double(i%2),-0.1}},
                {"items",nlohmann::json::array({{{"color","red"},{"size",i%2?1:2}},{{"color","blue"},{"size",i%2?2:1}}})}};
            d["outer"]={{"items",d["items"]}};
            if (i == 7) document_7 = d;
            auto add=coll->add(d.dump());ASSERT_TRUE(add.ok())<<add.error();
        }
    }
    void TearDown() override {
        track_arrays=false;observe_array_lifetimes=false;
        search_begin_us=0;search_stop_us=UINT64_MAX;search_cutoff=false;
        scripted_clock=false;full_search_probe=false;eval_cache_timeout_probe=false;
        auto* pool=manager.get_thread_pool();manager.dispose();pool->shutdown();delete pool;delete store;std::filesystem::remove_all(path);
    }
    std::unique_ptr<filter_node_t> parse(const std::string& q) {
        filter_node_t* root=nullptr;
        auto op=filter::parse_filter_query(q,coll->get_schema(),store,
            std::to_string(coll->get_collection_id())+"_"+Collection::DOC_ID_PREFIX+"_",root);
        EXPECT_TRUE(op.ok())<<q<<": "<<op.error();return std::unique_ptr<filter_node_t>(root);
    }
};

TEST_F(CuratedFilterDeadlineTest, PredicateMembershipParity) {
    const std::vector<std::string> predicates={
        "n:>=4","n:[2..9]","n:!=4","n:[!=4,7]","n:![2..9]",
        "f:>=4.5","f:!=4.5","f:![2.5..9.5]",
        "b:true","b:!=false","b:[true,false]",
        "s:alpha","s:=alpha beta","s:!=alpha beta","s:!alpha","s:=alpha b*",
        "tags:=red","tags:!=red","tags:[red,blue]",
        "geo:(51.5,-0.1,20 km)",
        "(n:>=4 && s:alpha) || (n:<2 && b:true)",
        "items.{color:=red && size:=1}","items.{color:=red || size:=1}",
        "outer.items.{color:=red && size:=1}","outer.items.{color:=red || size:=1}",
        "n:>999","n:>999 || b:true","n:>999 && b:true","id:*"};
    for(const auto& q:predicates) {
        SCOPED_TRACE(q);auto root=parse(q);ASSERT_NE(nullptr,root);
        filter_result_iterator_t lazy(coll->get_name(),coll->_get_index(),root.get(),true);
        filter_result_iterator_t computed(coll->get_name(),coll->_get_index(),root.get(),true);
        filter_result_iterator_t curated(coll->get_name(),coll->_get_index(),root.get(),true);
        ASSERT_TRUE(lazy.init_status().ok());ASSERT_TRUE(computed.init_status().ok());
        ASSERT_TRUE(curated.init_status().ok());
        computed.compute_iterators();
        EXPECT_FALSE(curated.compute_for_curation());
        std::vector<uint32_t> a,b,c;
        for(uint32_t i=0;i<2048;++i) {
            if(lazy.is_valid(i)==1)a.push_back(i);
            if(computed.is_valid(i)==1)b.push_back(i);
            if (curated.is_valid(i)==1) c.push_back(i);
        }
        EXPECT_EQ(a,b);
        EXPECT_EQ(a,c);
    }
}

TEST_F(CuratedFilterDeadlineTest, DeadlineMaterializationPreservesPinAdmission) {
    clock_base_us=1000000;
    std::vector<std::pair<uint32_t,uint32_t>> pins;
    for(uint32_t i=0;i<2048;++i)pins.emplace_back(i,i+1);
    auto root=parse("b:true && c:true");
    size_t admitted[2]={};int final_validity[2]={};
    std::vector<uint32_t> admitted_ids[2];
    std::map<size_t,std::map<size_t,uint32_t>> admitted_positions[2];
    for(int arm=0;arm<2;++arm) {
        // Freeze time during initialization, then expire before the first
        // actual clock check, with production's 30,000ms duration.
        clock_calls=0;advance_clock=false;scripted_clock=true;
        filter_result_iterator_t it(coll->get_name(),coll->_get_index(),root.get(),true,
            DEFAULT_FILTER_BY_CANDIDATES,1000000,30000000);
        scripted_clock=false;
        ASSERT_TRUE(it.init_status().ok());ASSERT_EQ(filter_result_iterator_t::valid,it.validity);
        clock_calls=0;advance_clock=true;scripted_clock=true;
        if (arm) EXPECT_TRUE(it.compute_for_curation());
        std::set<uint32_t> curated,missing;
        std::map<size_t,std::map<size_t,uint32_t>> positions;
        std::vector<uint32_t> ids;
        coll->_get_index()->process_curated_ids(pins,{}, {"group"},1,true,true,&it,
            curated,positions,ids,false,missing);
        scripted_clock=false;
        admitted_ids[arm] = ids;
        admitted_positions[arm] = positions;
        admitted[arm]=ids.size();final_validity[arm]=it.validity;
    }
    EXPECT_EQ(9, admitted[0]);EXPECT_EQ(admitted[0], admitted[1]);
    EXPECT_EQ(admitted_ids[0], admitted_ids[1]);
    EXPECT_EQ(admitted_positions[0], admitted_positions[1]);
    EXPECT_EQ(filter_result_iterator_t::timed_out,final_validity[0]);
    EXPECT_EQ(filter_result_iterator_t::timed_out,final_validity[1]);
}

TEST_F(CuratedFilterDeadlineTest, FullGroupedResponseAtDeadline) {
    std::string pins;
    for(uint32_t i=0;i<2048;++i) {
        if(!pins.empty())pins+=",";
        pins+=std::to_string(2047-i)+":"+std::to_string(i+1);
    }
    std::map<std::string,std::string> params={{"collection",coll->get_name()},{"q","*"},
        {"group_by","c"},{"group_limit","1"},{"per_page","1"},{"sort_by","n:desc"},
        {"filter_by","b:true"},{"filter_curated_hits","true"},{"pinned_hits",pins},
        {"enable_lazy_filter","true"},{"search_cutoff_ms","30000"}};
    nlohmann::json embedded;std::string response;
    scripted_clock=false;testing_curated_second_pass_entries=0;
    testing_curated_second_pass_unmaterialized_entries=0;
    clock_base_us=std::chrono::duration_cast<std::chrono::microseconds>(
        __real__ZNSt6chrono3_V212system_clock3nowEv().time_since_epoch()).count();
    full_search_probe=true;
    auto op=manager.do_search(params,embedded,response,clock_base_us);
    full_search_probe=false;scripted_clock=false;
    ASSERT_TRUE(op.ok())<<op.error();auto result=nlohmann::json::parse(response);
    result.erase("search_time_ms");
    auto expected = R"({"facet_counts":[],"found":1,"found_docs":0,"out_of":2048,
        "page":1,"request_params":{"collection_name":"review_probe","first_q":"*","per_page":1,"q":"*"},
        "search_cutoff":true})"_json;
    nlohmann::json hit = {{"curated",true},{"document",document_7},
        {"highlight",nlohmann::json::object()},{"highlights",nlohmann::json::array()}};
    nlohmann::json group = {{"group_key",nlohmann::json::array({true})},
        {"hits",nlohmann::json::array({hit})}};
    expected["grouped_hits"] = nlohmann::json::array({group});
    EXPECT_EQ(expected, result);
    EXPECT_GT(clock_calls,1);

}

TEST_F(CuratedFilterDeadlineTest, NumericNotEqualsOwnsAllTemporaryArrays) {
    for (const auto& predicate : {"n:!=4", "n:!=99", "n:[!=4,7]"}) {
        for (bool timeout : {false, true}) {
            SCOPED_TRACE(std::string(predicate) + (timeout ? ":timeout" : ":complete"));
            for (size_t repeat = 0; repeat < 8; ++repeat) {
                const std::string expression = timeout ?
                        "(" + std::string(predicate) + ") && b:true" : predicate;
                auto root = parse(expression);
                arrays.clear();
                clock_base_us = 1000000;
                scripted_clock = timeout; advance_clock = false;
                {
                    filter_result_iterator_t it(coll->get_name(),coll->_get_index(),root.get(),true,
                        DEFAULT_FILTER_BY_CANDIDATES,1000000,timeout ? 30000000 : UINT64_MAX);
                    ASSERT_TRUE(it.init_status().ok());
                    clock_calls = 1; advance_clock = true;
                    observe_array_lifetimes = true;
                    track_arrays = true;
                    it.compute_iterators();
                    track_arrays = false;
                    EXPECT_EQ(std::string(predicate)=="n:!=99" ? 2048 : 1920,it.approx_filter_ids_length);
                    if (timeout) EXPECT_EQ(filter_result_iterator_t::timed_out,it.validity);
                }
                observe_array_lifetimes = false;
                scripted_clock = false;
                EXPECT_TRUE(arrays.empty()) << "Outstanding new[] allocations: " << arrays.size();
                // Remove observation roots without reclaiming anything: LSan must
                // independently see any ownership regression on process exit.
                std::vector<std::pair<void*,size_t>>().swap(arrays);
            }
        }
    }
}
TEST_F(CuratedFilterDeadlineTest, CandidatePreservesReferencesAndLeafLimits) {
    auto refs_schema = R"({"name":"cf_refs","fields":[
        {"name":"allowed","type":"bool"}]})"_json;
    auto refs = manager.create_collection(refs_schema);
    ASSERT_TRUE(refs.ok());
    ASSERT_TRUE(refs.get()->add(R"({"id":"r0","allowed":true})").ok());
    ASSERT_TRUE(refs.get()->add(R"({"id":"r1","allowed":true})").ok());
    auto joined_schema = R"({"name":"cf_joined","fields":[
        {"name":"ref","type":"string","reference":"cf_refs.id"},
        {"name":"b","type":"bool"},{"name":"s","type":"string"}]})"_json;
    auto joined = manager.create_collection(joined_schema);
    ASSERT_TRUE(joined.ok());
    coll = joined.get();
    for (uint32_t i = 0; i < 1024; ++i) {
        nlohmann::json doc = {{"id",std::to_string(i)},{"ref",i%2 ? "r1" : "r0"},
            {"b",true},{"s","alpha"+std::to_string(i%32)}};
        ASSERT_TRUE(coll->add(doc.dump()).ok());
    }
    auto root = parse("$cf_refs(allowed:true) && b:true");
    const auto begin = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count();
    filter_result_iterator_t it(coll->get_name(),coll->_get_index(),root.get(),true,
        DEFAULT_FILTER_BY_CANDIDATES,begin,30000000);
    ASSERT_TRUE(it.init_status().ok());
    EXPECT_FALSE(it.compute_for_curation());
    ASSERT_TRUE(it._get_is_filter_result_initialized());
    ASSERT_EQ(1,it.reference.at("cf_refs").count);
    EXPECT_EQ(0,it.reference.at("cf_refs").docs[0]);
    for (uint32_t id = 0; id < 4; ++id) EXPECT_EQ(1,it.is_valid(id));
    EXPECT_EQ(1,it.reference.at("cf_refs").docs[0]);
    it.reset();
    EXPECT_EQ(0,it.reference.at("cf_refs").docs[0]);

    auto string_root = parse("s:alpha*");
    auto group_root = parse("b:true");
    auto* string_it = new filter_result_iterator_t(coll->get_name(),coll->_get_index(),string_root.get(),true,1);
    std::vector<uint32_t> expected, ids;
    for (uint32_t i = 0; i < 1024; ++i) {
        ids.push_back(i);
        if (string_it->is_valid(i)==1) expected.push_back(i);
    }
    ASSERT_GT(expected.size(),0);
    ASSERT_LT(expected.size(),ids.size());
    auto* group_it = new filter_result_iterator_t(coll->get_name(),coll->_get_index(),group_root.get(),true,1);
    filter_result_iterator_t effective(AND,string_it,group_it,string_root,group_root.release());
    ASSERT_FALSE(effective._get_is_filter_result_initialized());
    EXPECT_FALSE(effective.compute_for_curation());
    std::vector<uint32_t> actual;
    for (auto id : ids) if (effective.is_valid(id)==1) actual.push_back(id);
    EXPECT_EQ(expected,actual);
}
#endif

#if defined(__linux__)
TEST_F(CuratedFilterDeadlineTest, ExpiredEvalBuildIsNotAdmittedToRequestCache) {
    for (size_t after : {size_t(0), size_t(1)}) {
        eval_membership_cache_t cache;
        testing_eval_membership_builds = 0;
        eval_cache_clock_calls = 0;
        eval_cache_expire_after = after;
        std::map<std::string, std::string> request = {
            {"collection", "review_probe"}, {"q", "*"}, {"query_by", "s"},
            {"sort_by", "_eval(n:>=0 && tags:=red):desc"}};
        nlohmann::json embedded;
        std::string result;
        eval_cache_timeout_probe = true;
        auto op = manager.do_search(request, embedded, result, clock_base_us, &cache);
        eval_cache_timeout_probe = false;
        EXPECT_GT(testing_eval_membership_builds, 0);
        EXPECT_EQ(0, cache.admissions()) << after;
        EXPECT_EQ(0, cache.size()) << after;
    }
}
#endif
