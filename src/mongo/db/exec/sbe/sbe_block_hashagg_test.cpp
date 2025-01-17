/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <algorithm>
#include <map>
#include <type_traits>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {

typedef std::map<int32_t, std::vector<int32_t>> TestResultType;

class BlockHashAggStageTest : public PlanStageTestFixture {
public:
    void setUp() override {
        PlanStageTestFixture::setUp();
        _globalLock = std::make_unique<Lock::GlobalLock>(operationContext(), MODE_IS);
    }

    void tearDown() override {
        _globalLock.reset();
        PlanStageTestFixture::tearDown();
    }

    static std::pair<value::TypeTags, value::Value> unpackSingletonBlock(
        std::pair<value::TypeTags, value::Value> blockPair) {
        auto [blockTag, blockVal] = blockPair;
        ASSERT_EQ(blockTag, value::TypeTags::valueBlock);
        auto deblocked = value::bitcastTo<value::ValueBlock*>(blockVal)->extract();
        ASSERT_EQ(deblocked.count, 1);
        return {deblocked.tags[0], deblocked.vals[0]};
    }

    static std::vector<std::pair<value::TypeTags, value::Value>> unpackArrayOfSingleBlocks(
        value::Value arrayVal) {
        auto arr = value::getArrayView(arrayVal);
        std::vector<std::pair<value::TypeTags, value::Value>> result;
        for (size_t i = 0; i < arr->size(); i++) {
            result.emplace_back(unpackSingletonBlock(arr->getAt(i)));
        }
        return result;
    }

    static std::pair<value::TypeTags, value::Value> makeArray(
        std::vector<std::pair<value::TypeTags, value::Value>> vals) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard(arrTag, arrVal);
        for (auto [t, v] : vals) {
            value::getArrayView(arrVal)->push_back(t, v);
        }
        guard.reset();
        return {arrTag, arrVal};
    }

    template <typename... BlockData>
    static std::pair<value::TypeTags, value::Value> makeInputArray(int32_t id,
                                                                   std::vector<bool> bitset,
                                                                   BlockData... blockData) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard(arrTag, arrVal);
        auto arr = value::getArrayView(arrVal);

        // Append groupBy key.
        arr->push_back(makeInt32(id));
        // Append bitset block.
        auto bitsetBlock = makeBoolBlock(bitset);
        arr->push_back({sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(bitsetBlock.release())});
        // Append data.
        (arr->push_back(makeHeterogeneousBlockTagVal(blockData)), ...);
        guard.reset();
        return {arrTag, arrVal};
    }

    // This helper takes an array of groupby results and compares to the expectedMap of group ID to
    // a list of accumulator results.
    static void assertResultMatchesMap(std::pair<value::TypeTags, value::Value> result,
                                       TestResultType expectedMap) {
        ASSERT_EQ(result.first, value::TypeTags::Array);
        auto resultArr = value::getArrayView(result.second);
        for (auto [subArrTag, subArrVal] : resultArr->values()) {
            ASSERT_EQ(subArrTag, value::TypeTags::Array);
            auto subArray = value::getArrayView(subArrVal);

            // Unpack the key.
            auto [idTag, idVal] = unpackSingletonBlock(subArray->getAt(0));
            ASSERT_EQ(idTag, value::TypeTags::NumberInt32);
            auto key = value::bitcastTo<int32_t>(idVal);

            // Get the expected results for this accumulator.
            auto expectedVals = expectedMap.at(key);
            ASSERT_EQ(subArray->size(), expectedVals.size() + 1);

            // Now assert against our expected values.
            for (size_t i = 0; i < expectedVals.size(); i++) {
                auto [gbTag, gbVal] = unpackSingletonBlock(subArray->getAt(i + 1));
                assertValuesEqual(gbTag,
                                  gbVal,
                                  value::TypeTags::NumberInt32,
                                  value::bitcastTo<int32_t>(expectedVals[i]));
            }

            // Delete from the expected map so we know we get the results exactly once.
            expectedMap.erase(key);
        }
        ASSERT(expectedMap.empty());
    }

    template <typename... BlockData>
    static std::pair<value::TypeTags, value::Value> makeInputArray(
        std::vector<std::pair<value::TypeTags, value::Value>> id,
        std::vector<bool> bitset,
        BlockData... blockData) {
        auto [arrTag, arrVal] = value::makeNewArray();
        value::Array* arr = value::getArrayView(arrVal);

        // Append groupby keys.
        arr->push_back(makeHeterogeneousBlockTagVal(id));
        // Append corresponding bitset.
        auto bitsetBlock = makeBoolBlock(bitset);
        arr->push_back({sbe::value::TypeTags::valueBlock,
                        value::bitcastFrom<value::ValueBlock*>(bitsetBlock.release())});
        // Append data.
        (arr->push_back(makeHeterogeneousBlockTagVal(blockData)), ...);
        return {arrTag, arrVal};
    }

    // Given the data input, the number of slots the stage requires, accumulators used, and expected
    // output, runs the BlockHashAgg stage and asserts that we get correct results.
    void runBlockHashAggTest(std::pair<value::TypeTags, value::Value> inputData,
                             size_t numScanSlots,
                             std::vector<std::pair<std::string, std::string>> accNames,
                             TestResultType expected) {
        auto makeFn = [&](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
            auto idSlot = scanSlots[0];
            auto bitsetInSlot = scanSlots[1];
            value::SlotVector outputSlots{idSlot};

            auto accumulatorBitset = generateSlotId();
            auto internalSlot = generateSlotId();
            BlockHashAggStage::BlockAndRowAggs aggs;
            size_t scanSlotIdx = 2;
            for (const auto& [blockAcc, rowAcc] : accNames) {
                auto outputSlot = generateSlotId();
                std::unique_ptr<sbe::EExpression> blockAccFunc;
                if (blockAcc == "valueBlockCount") {
                    // valueBlockCount is the exception - it takes just the bitset.
                    blockAccFunc =
                        stage_builder::makeFunction(blockAcc, makeE<EVariable>(accumulatorBitset));
                } else {
                    blockAccFunc =
                        stage_builder::makeFunction(blockAcc,
                                                    makeE<EVariable>(accumulatorBitset),
                                                    makeE<EVariable>(scanSlots[scanSlotIdx]));
                    scanSlotIdx++;
                }
                aggs.emplace(
                    outputSlot,
                    BlockHashAggStage::BlockRowAccumulators{
                        std::move(blockAccFunc),
                        stage_builder::makeFunction(rowAcc, makeE<EVariable>(internalSlot))});
                outputSlots.push_back(outputSlot);
            }

            auto outStage = makeS<BlockHashAggStage>(std::move(scanStage),
                                                     idSlot,
                                                     bitsetInSlot,
                                                     internalSlot,
                                                     accumulatorBitset,
                                                     std::move(aggs),
                                                     kEmptyPlanNodeId,
                                                     true);
            return std::make_pair(outputSlots, std::move(outStage));
        };

        auto result = runTestMulti(numScanSlots, inputData.first, inputData.second, makeFn);
        value::ValueGuard resultGuard{result};
        assertResultMatchesMap(result, expected);
    }

private:
    std::unique_ptr<Lock::GlobalLock> _globalLock;
};

TEST_F(BlockHashAggStageTest, NoData) {
    auto [inputTag, inputVal] = makeArray({});
    // We should have an empty block with no data.
    TestResultType expected = {};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockMin", "min"}}, expected);
}

TEST_F(BlockHashAggStageTest, AllDataFiltered) {
    // All data has "false" for bitset.
    auto [inputTag, inputVal] =
        makeArray({makeInputArray(0, {false, false, false}, makeInt32s({50, 20, 30}))});
    // We should have an empty block with no data.
    TestResultType expected = {};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockMin", "min"}}, expected);
}

TEST_F(BlockHashAggStageTest, SingleAccumulatorMin) {
    // Each entry is ID followed by bitset followed by a block of data. For example
    // [groupid, [block bitset values], [block data values]]
    auto [inputTag, inputVal] =
        makeArray({makeInputArray(0, {true, true, false}, makeInt32s({50, 20, 30})),
                   makeInputArray(2, {false, true, true}, makeInt32s({40, 30, 60})),
                   makeInputArray(1, {true, true, true}, makeInt32s({70, 80, 10})),
                   makeInputArray(2, {false, false, false}, makeInt32s({10, 20, 30})),
                   makeInputArray(2, {true, false, true}, makeInt32s({30, 40, 50}))});
    /*
     * 0 -> min(50, 20) = 20
     * 1 -> min(70, 80, 10) = 10
     * 2 -> min(30, 60, 30, 50) = 30
     */
    TestResultType expected = {{0, {20}}, {1, {10}}, {2, {30}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockMin", "min"}}, expected);
}

TEST_F(BlockHashAggStageTest, Count1) {
    // Each entry is ID followed by a bitset.
    auto [inputTag, inputVal] = makeArray({makeInputArray(0, {true, true, true}),
                                           makeInputArray(0, {true, false, true}),
                                           makeInputArray(1, {true, false, true}),
                                           makeInputArray(1, {true, true, false})});
    TestResultType expected = {{0, {5}}, {1, {4}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockCount", "sum"}}, expected);
}

TEST_F(BlockHashAggStageTest, Sum1) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] =
        makeArray({makeInputArray(0, {true, true, false}, makeInt32s({1, 2, 3})),
                   makeInputArray(2, {false, true, true}, makeInt32s({4, 5, 6})),
                   makeInputArray(1, {true, true, true}, makeInt32s({7, 8, 9})),
                   makeInputArray(2, {false, false, false}, makeInt32s({10, 11, 12})),
                   makeInputArray(2, {true, false, true}, makeInt32s({13, 14, 15}))});
    /*
     * 0 -> 1+2 = 3
     * 1 -> 7+8+9 = 24
     * 2 -> 5+6+13+15 = 39
     */
    TestResultType expected = {{0, {3}}, {1, {24}}, {2, {39}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockSum", "sum"}}, expected);
}

TEST_F(BlockHashAggStageTest, MultipleAccumulators) {
    // Each entry is ID followed by bitset followed by block A and block B.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray(
             100, {true, true, false}, makeInt32s({200, 100, 150}), makeInt32s({2, 4, 7})),
         makeInputArray(
             100, {false, true, true}, makeInt32s({50, 90, 60}), makeInt32s({-100, 20, 3})),
         makeInputArray(
             50, {true, true, true}, makeInt32s({200, 100, 150}), makeInt32s({-150, 150, 20})),
         makeInputArray(
             25, {true, false, false}, makeInt32s({20, 75, 10}), makeInt32s({0, 20, -20})),
         makeInputArray(
             50, {true, false, true}, makeInt32s({75, 75, 75}), makeInt32s({-2, 5, 8}))});
    /*
     * 25  -> min(20) = 20, count=1, min(0) = 0
     * 50  -> min(200, 100, 150, 75, 75) = 75, count = 5, min(-150, 150, 20, -2, 8) = -150
     * 100 -> min(200, 100, 90, 60) = 60, count = 4, min(2, 4, 20, 3) = 2
     */
    TestResultType expected = {{25, {20, 1, 0}}, {50, {75, 5, -150}}, {100, {60, 4, 2}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal),
        4,
        {{"valueBlockMin", "min"}, {"valueBlockCount", "sum"}, {"valueBlockMin", "min"}},
        expected);
}


// --- Tests with block groupby key inputs ---

TEST_F(BlockHashAggStageTest, SumBlockGroupByKey1) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray(makeInt32s({0, 0, 0}), {true, true, false}, makeInt32s({1, 2, 3})),
         makeInputArray(makeInt32s({2, 2, 2}), {false, true, true}, makeInt32s({4, 5, 6})),
         makeInputArray(makeInt32s({1, 1, 1}), {true, true, true}, makeInt32s({7, 8, 9})),
         makeInputArray(makeInt32s({2, 2, 2}), {false, false, false}, makeInt32s({10, 11, 12})),
         makeInputArray(makeInt32s({2, 2, 2}), {true, false, true}, makeInt32s({13, 14, 15}))});

    /*
     * 0 -> 1+2 = 3
     * 1 -> 7+8+9 = 24
     * 2 -> 5+6+13+15 = 39
     */
    TestResultType expected = {{0, {3}}, {1, {24}}, {2, {39}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockSum", "sum"}}, expected);
}

// Similar to the test above, but we change the groupby keys so they are different within each
// block.
TEST_F(BlockHashAggStageTest, SumDifferentBlockGroupByKeys2) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray(makeInt32s({1, 2, 3}), {true, true, false}, makeInt32s({1, 2, 3})),
         makeInputArray(makeInt32s({2, 2, 2}), {false, true, true}, makeInt32s({4, 5, 6})),
         makeInputArray(makeInt32s({3, 2, 1}), {true, true, true}, makeInt32s({7, 8, 9})),
         makeInputArray(makeInt32s({2, 3, 4}), {false, true, true}, makeInt32s({10, 11, 12})),
         makeInputArray(makeInt32s({2, 3, 4}), {false, false, false}, makeInt32s({0, 5, 4})),
         makeInputArray(makeInt32s({1, 1, 2}), {true, true, true}, makeInt32s({13, 14, 15}))});

    /*
     * 1 -> 1+9+13+14  = 37
     * 2 -> 2+5+6+8+15 = 36
     * 3 -> 7+11       = 18
     * 4 -> 12         = 12
     */
    TestResultType expected = {{1, {37}}, {2, {36}}, {3, {18}}, {4, {12}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockSum", "sum"}}, expected);
}

// Similar test as above but the "2" key appears in every block but is always false, so we make sure
// it's missing.
TEST_F(BlockHashAggStageTest, SumDifferentBlockGroupByKeysMissingKey) {
    // Each entry is ID followed by bitset followed by a block of data.
    auto [inputTag, inputVal] = makeArray(
        {makeInputArray(makeInt32s({1, 2, 3}), {true, false, false}, makeInt32s({1, 2, 3})),
         makeInputArray(makeInt32s({2, 2, 2}), {false, false, false}, makeInt32s({4, 5, 6})),
         makeInputArray(makeInt32s({3, 2, 1}), {true, false, true}, makeInt32s({7, 8, 9})),
         makeInputArray(makeInt32s({2, 3, 4}), {false, true, true}, makeInt32s({10, 11, 12})),
         makeInputArray(makeInt32s({2, 3, 4}), {false, false, false}, makeInt32s({0, 5, 4})),
         makeInputArray(makeInt32s({1, 1, 2}), {true, true, false}, makeInt32s({13, 14, 15}))});

    /*
     * 1 -> 1+9+13+14  = 37
     * 2 -> missing
     * 3 -> 7+11       = 18
     * 4 -> 12         = 12
     */
    TestResultType expected = {{1, {37}}, {3, {18}}, {4, {12}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal), 3, {{"valueBlockSum", "sum"}}, expected);
}

TEST_F(BlockHashAggStageTest, MultipleAccumulatorsDifferentBlockGroupByKeys) {
    // Each entry is ID followed by bitset followed by block A and block B.
    auto [inputTag, inputVal] = makeArray({makeInputArray(makeInt32s({25, 50, 100}),
                                                          {true, true, false},
                                                          makeInt32s({200, 100, 150}),
                                                          makeInt32s({2, 4, 7})),
                                           makeInputArray(makeInt32s({50, 50, 50}),
                                                          {false, true, true},
                                                          makeInt32s({50, 90, 60}),
                                                          makeInt32s({-100, 20, 3})),
                                           makeInputArray(makeInt32s({25, 25, 100}),
                                                          {true, true, true},
                                                          makeInt32s({200, 100, 150}),
                                                          makeInt32s({-150, 150, 2})),
                                           makeInputArray(makeInt32s({100, 50, 25}),
                                                          {true, false, false},
                                                          makeInt32s({20, 75, 10}),
                                                          makeInt32s({0, 20, -20})),
                                           makeInputArray(makeInt32s({100, 25, 50}),
                                                          {true, false, true},
                                                          makeInt32s({75, 75, 75}),
                                                          makeInt32s({-2, 5, 8}))});

    /*
     * 25  -> min(200, 200, 100) = 100, count = 3, min(2, -150, 150) = -150
     * 50  -> min(100, 90, 60, 75) = 60, count = 4, min(4, 20, 3, 8) = 3
     * 100 -> min(150, 20, 75) = 20, count = 3, min(20, 0, -2) = -2
     */
    TestResultType expected = {{25, {100, 3, -150}}, {50, {60, 4, 3}}, {100, {20, 3, -2}}};
    runBlockHashAggTest(
        std::make_pair(inputTag, inputVal),
        4,
        {{"valueBlockMin", "min"}, {"valueBlockCount", "sum"}, {"valueBlockMin", "min"}},
        expected);
}
}  // namespace mongo::sbe
