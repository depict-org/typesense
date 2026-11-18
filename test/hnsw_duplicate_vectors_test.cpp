#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>
#include "hnswlib/hnswlib.h"

TEST(HnswDuplicateVectorsTest, IdenticalNormalizedUpdatesRemainReachable) {
    const size_t count = 4096;
    hnswlib::InnerProductSpace space(32);
    hnswlib::HierarchicalNSW<float> graph(&space, count, 16, 200, 100, true);
    std::vector<float> vector(32, 0.0f);
    vector[0] = 1.0f;
    for (size_t label = 0; label < count; ++label) graph.addPoint(vector.data(), label, true);
    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t label = 0; label < count; ++label) graph.addPoint(vector.data(), label, true);
        EXPECT_EQ(count, graph.searchKnn(vector.data(), count, count).size());
    }
}

// T2 needs ~13 s at -O0: groups exceed the 32-link degree; smaller shapes did not fail, and one re-embed round is needed.
TEST(HnswDuplicateVectorsTest, WholeProductReembeddingRetainsEveryLiveLabel) {
    const size_t dimensions = 16, groups = 16, variants = 119, singles = 100;
    const size_t count = groups * variants + singles;
    std::mt19937 generator(7);
    std::normal_distribution<float> normal(0.0f, 1.0f);
    auto normalize = [](std::vector<float>& vector) {
        float squared_norm = 0.0f;
        for (float value : vector) squared_norm += value * value;
        const float inverse_norm = 1.0f / std::sqrt(squared_norm);
        for (float& value : vector) value *= inverse_norm;
    };
    auto random_unit = [&]() {
        std::vector<float> vector(dimensions);
        for (float& value : vector) value = normal(generator);
        normalize(vector);
        return vector;
    };
    std::vector<std::vector<float>> product_vectors;
    for (size_t product = 0; product < groups + singles; ++product)
        product_vectors.push_back(random_unit());
    auto product_of = [&](size_t label) {
        return label < groups * variants ? label / variants : groups + label - groups * variants;
    };
    hnswlib::InnerProductSpace space(dimensions);
    hnswlib::HierarchicalNSW<float> graph(&space, count, 16, 200, 100, true);
    for (size_t label = 0; label < count; ++label)
        graph.addPoint(product_vectors[product_of(label)].data(), label, true);
    const auto query = random_unit();
    for (size_t product = 0; product < groups; ++product) {
        const auto noise = random_unit();
        for (size_t dimension = 0; dimension < dimensions; ++dimension)
            product_vectors[product][dimension] += 1e-5f * noise[dimension];
        normalize(product_vectors[product]);
    }
    for (size_t label = 0; label < groups * variants; ++label)
        graph.addPoint(product_vectors[product_of(label)].data(), label, true);
    EXPECT_EQ(count, graph.searchKnn(query.data(), count, count).size());
}
