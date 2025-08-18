/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "Test.h"

#include <wtf/DataLog.h>
#include <wtf/IntervalSet.h>
#include <wtf/ListDump.h>
#include <wtf/Vector.h>

#include <random>
#include <algorithm>

namespace TestWebKitAPI {

struct IntervalSetTest {
    static constexpr bool verbose = false;
};

using Point = uint32_t;
using Value = int;
using Interval = Range<Point>;

TEST(WTF_IntervalSet, Basic)
{
    IntervalSet<Point, Value> intervalSet;
    
    // Test empty set
    EXPECT_FALSE(intervalSet.hasOverlap({ 0, 10 }));
    EXPECT_EQ(nullptr, intervalSet.find({ 0, 10 }));
}

TEST(WTF_IntervalSet, SingleInterval)
{
    IntervalSet<Point, Value> intervalSet;
    
    // Insert a single interval [10, 20) with value 42
    intervalSet.insert({ 10, 20 }, 42);
    
    // Test overlap detection
    EXPECT_TRUE(intervalSet.hasOverlap({ 15, 25 }));  // Overlaps
    EXPECT_TRUE(intervalSet.hasOverlap({ 5, 15 }));   // Overlaps
    EXPECT_TRUE(intervalSet.hasOverlap({ 10, 20 }));  // Exact match
    EXPECT_FALSE(intervalSet.hasOverlap({ 0, 10 }));  // No overlap (adjacent)
    EXPECT_FALSE(intervalSet.hasOverlap({ 20, 30 })); // No overlap (adjacent)
    EXPECT_FALSE(intervalSet.hasOverlap({ 0, 5 }));   // No overlap (before)
    EXPECT_FALSE(intervalSet.hasOverlap({ 25, 30 })); // No overlap (after)
    
    // Test find
    const Value* value = intervalSet.find({ 15, 16 });
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(42, *value);
    
    // Test find with non-overlapping interval
    EXPECT_EQ(nullptr, intervalSet.find({ 0, 5 }));
    
    // Test erase functionality
    intervalSet.erase({ 10, 20 });
    
    // After erase, all overlap checks should return false
    EXPECT_FALSE(intervalSet.hasOverlap({ 15, 25 }));  // No longer overlaps
    EXPECT_FALSE(intervalSet.hasOverlap({ 5, 15 }));   // No longer overlaps
    EXPECT_FALSE(intervalSet.hasOverlap({ 10, 20 }));  // No longer overlaps
    EXPECT_FALSE(intervalSet.hasOverlap({ 0, 10 }));   // Still no overlap
    EXPECT_FALSE(intervalSet.hasOverlap({ 20, 30 }));  // Still no overlap
    EXPECT_FALSE(intervalSet.hasOverlap({ 0, 5 }));    // Still no overlap
    EXPECT_FALSE(intervalSet.hasOverlap({ 25, 30 }));  // Still no overlap
    
    // After erase, all find operations should return nullptr
    EXPECT_EQ(nullptr, intervalSet.find({ 15, 16 }));
    EXPECT_EQ(nullptr, intervalSet.find({ 10, 20 }));
    EXPECT_EQ(nullptr, intervalSet.find({ 0, 5 }));
}

TEST(WTF_IntervalSet, EraseTests)
{
    IntervalSet<Point, Value> intervalSet;
    
    // Test erasing from empty set (should not crash)
    // Note: This would actually assert in debug builds since erase expects the interval to exist
    
    // Test basic erase functionality
    intervalSet.insert({ 10, 20 }, 100);
    intervalSet.insert({ 30, 40 }, 200);
    intervalSet.insert({ 50, 60 }, 300);
    
    // Verify all intervals are present
    EXPECT_TRUE(intervalSet.hasOverlap({ 10, 20 }));
    EXPECT_TRUE(intervalSet.hasOverlap({ 30, 40 }));
    EXPECT_TRUE(intervalSet.hasOverlap({ 50, 60 }));
    
    // Erase middle interval
    intervalSet.erase({ 30, 40 });
    
    // Verify middle interval is gone, others remain
    EXPECT_TRUE(intervalSet.hasOverlap({ 10, 20 }));
    EXPECT_FALSE(intervalSet.hasOverlap({ 30, 40 }));
    EXPECT_TRUE(intervalSet.hasOverlap({ 50, 60 }));
    
    // Verify find operations
    const Value* value = intervalSet.find({ 15, 16 });
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(100, *value);
    
    EXPECT_EQ(nullptr, intervalSet.find({ 35, 36 }));
    
    value = intervalSet.find({ 55, 56 });
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(300, *value);
    
    // Erase first interval
    intervalSet.erase({ 10, 20 });
    
    EXPECT_FALSE(intervalSet.hasOverlap({ 10, 20 }));
    EXPECT_FALSE(intervalSet.hasOverlap({ 30, 40 }));
    EXPECT_TRUE(intervalSet.hasOverlap({ 50, 60 }));
    
    // Erase last interval (should make set empty)
    intervalSet.erase({ 50, 60 });
    
    EXPECT_FALSE(intervalSet.hasOverlap({ 10, 20 }));
    EXPECT_FALSE(intervalSet.hasOverlap({ 30, 40 }));
    EXPECT_FALSE(intervalSet.hasOverlap({ 50, 60 }));
    
    // Verify all finds return nullptr on empty set
    EXPECT_EQ(nullptr, intervalSet.find({ 15, 16 }));
    EXPECT_EQ(nullptr, intervalSet.find({ 35, 36 }));
    EXPECT_EQ(nullptr, intervalSet.find({ 55, 56 }));
}


TEST(WTF_IntervalSet, EdgeCases)
{
    IntervalSet<Point, Value> intervalSet;
    
    // Insert interval [0, 1) - single unit interval
    intervalSet.insert({ 0, 1 }, 100);
    
    EXPECT_TRUE(intervalSet.hasOverlap({ 0, 1 }));
    EXPECT_FALSE(intervalSet.hasOverlap({ 1, 2 }));
    
    const Value* value = intervalSet.find({ 0, 1 });
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(100, *value);
    
    // Test with larger intervals that span the small one
    EXPECT_TRUE(intervalSet.hasOverlap({ 0, 10 }));
    value = intervalSet.find({ 0, 10 });
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(100, *value);
}

enum class IntervalOrdering {
    Ascending,
    Descending,
    Random,
};

static void stressTest(IntervalOrdering ordering)
{
    constexpr size_t numberTestIntervals = 10000;
    constexpr size_t maxGap = 1000;
    constexpr size_t maxSize = 1000;
    constexpr size_t maxPoint = numberTestIntervals * (maxGap + maxSize);

    struct TestCase : public std::pair<Interval, Value> {
        TestCase() = default;
        TestCase(const Interval& interval, const Value& value) : std::pair<Interval, Value>(interval, value) { }
        
        void dump(PrintStream& out) const
        {
            out.print("{ ", first, ", ", second, " }");
        }
    };

    IntervalSet<Point, Value> intervalSet;
    
    std::mt19937 gen(testing::UnitTest::GetInstance()->random_seed());
    std::uniform_int_distribution<size_t> gapDist(0, maxGap);
    std::uniform_int_distribution<size_t> sizeDist(1, maxSize);
    std::uniform_int_distribution<Value> valueDist(0, 10000);
    
    // Generate non-overlapping intervals by sorting start points
    Vector<TestCase> testData;
    Point end = 0;
    for (unsigned i = 0; i < numberTestIntervals; ++i) {
        Point start = end + gapDist(gen);
        end = start + sizeDist(gen);
        Value value = valueDist(gen);
        testData.append(TestCase({ start, end }, value));
    }
    dataLogLnIf(IntervalSetTest::verbose, "Test data: ", WTF::listDump(testData));

    auto shuffledTestData = testData;

    switch (ordering) {
    case IntervalOrdering::Ascending:
        break;
    case IntervalOrdering::Descending:
        std::reverse(shuffledTestData.begin(), shuffledTestData.end());
        break;
    case IntervalOrdering::Random:
        // Shuffle the intervals to insert them in random order
        std::shuffle(shuffledTestData.begin(), shuffledTestData.end(), gen);
        break;
    }
    dataLogLnIf(IntervalSetTest::verbose, "After shuffle: ", WTF::listDump(shuffledTestData));

    // Track which intervals are currently in the set for erase operations
    Vector<TestCase> currentlyInserted;
    std::uniform_int_distribution<int> eraseDist(1, 4); // 1 in 4 chance to erase
    
    for (const auto& entry : shuffledTestData) {
        intervalSet.insert(entry.first, entry.second);
        currentlyInserted.append(entry);
        dataLogLnIf(IntervalSetTest::verbose, "Added ", entry.first, "=", entry.second, ": ", intervalSet);
        
        // Randomly erase an interval (1 in 4 chance) if we have intervals to erase
        if (currentlyInserted.size() > 1 && eraseDist(gen) == 1) {
            std::uniform_int_distribution<size_t> eraseIndexDist(0, currentlyInserted.size() - 1);
            size_t eraseIndex = eraseIndexDist(gen);
            TestCase toErase = currentlyInserted[eraseIndex];
            
            intervalSet.erase(toErase.first);
            currentlyInserted.removeAt(eraseIndex);
            dataLogLnIf(IntervalSetTest::verbose, "Erased ", toErase.first, "=", toErase.second, ": ", intervalSet);
        }
    }

    // Test that all currently inserted intervals can be found with correct values
    std::shuffle(currentlyInserted.begin(), currentlyInserted.end(), gen);
    for (const auto& data : currentlyInserted) {
        dataLogLnIf(IntervalSetTest::verbose, "Testing: interval=", data.first, " value=", data.second);
        EXPECT_TRUE(intervalSet.hasOverlap(data.first));
        const Value* found = intervalSet.find(data.first);
        EXPECT_NE(nullptr, found);
        EXPECT_EQ(data.second, *found);
        if (data.second != *found) return;
    }
    
    std::uniform_int_distribution<size_t> pointDist(0, maxPoint);
    // Test random queries
    for (unsigned i = 0; i < 500; ++i) {
        Point start = pointDist(gen);
        Point end = start + sizeDist(gen);
        Interval query = { start, end };
        
        bool shouldOverlap = false;
        Value expectedValue = 0;
        for (const auto& data : currentlyInserted) {
            if (query.overlaps(data.first)) {
                shouldOverlap = true;
                expectedValue = data.second;
                break;
            }
        }
        dataLogLnIf(IntervalSetTest::verbose, "Testing: random interval=", query);

        EXPECT_EQ(shouldOverlap, intervalSet.hasOverlap(query));
        const Value* found = intervalSet.find(query);
        if (shouldOverlap) {
            EXPECT_NE(nullptr, found);
            EXPECT_EQ(expectedValue, *found);
        } else {
            EXPECT_EQ(nullptr, found);
        }
    }
}

TEST(WTF_IntervalSet, AscendingStressTest)
{
    stressTest(IntervalOrdering::Ascending);
}

TEST(WTF_IntervalSet, DescendingStressTest)
{
    stressTest(IntervalOrdering::Descending);
}

TEST(WTF_IntervalSet, RandomStressTest)
{
    stressTest(IntervalOrdering::Random);
}

TEST(WTF_IntervalSet, Dump)
{
    IntervalSet<int, const char*> intervalSet;
    
    // Test empty tree
    dataLogLn("Empty tree:");
    intervalSet.dump(WTF::dataFile());
    
    // Add some intervals
    intervalSet.insert({10, 20}, "first");
    intervalSet.insert({30, 40}, "second");
    intervalSet.insert({50, 60}, "third");
    
    dataLogLn("\nAfter inserting intervals:");
    intervalSet.dump(WTF::dataFile());
    
    // Add more intervals to potentially cause splits
    intervalSet.insert({5, 8}, "before");
    intervalSet.insert({25, 28}, "middle");
    intervalSet.insert({65, 70}, "after");
    
    dataLogLn("\nAfter more insertions:");
    intervalSet.dump(WTF::dataFile());
}

} // namespace TestWebKitAPI