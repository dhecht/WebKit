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

#include <wtf/IntervalSet.h>
#include <wtf/Vector.h>

#include <random>
#include <algorithm>

namespace TestWebKitAPI {

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

TEST(WTF_IntervalSet, RandomStressTest)
{
    constexpr size_t numberTestIntervals = 10000;
    constexpr size_t maxGap = 1000;
    constexpr size_t maxSize = 1000;
    constexpr size_t maxPoint = numberTestIntervals * (maxGap + maxSize);

    IntervalSet<Point, Value> intervalSet;
    
    // Use UnitTest seed like Int128.cpp for reproducible randomness
    std::mt19937 gen(testing::UnitTest::GetInstance()->random_seed());
    std::uniform_int_distribution<size_t> gapDist(0, maxGap);
    std::uniform_int_distribution<size_t> sizeDist(1, maxSize);
    std::uniform_int_distribution<Value> valueDist(0, 10000);
    
    // Generate non-overlapping intervals by sorting start points
    Vector<Interval> testIntervals;
    Point end = 0;
    for (unsigned i = 0; i < numberTestIntervals; ++i) {
        Point start = end + gapDist(gen);
        end = start + sizeDist(gen);
        testIntervals.append({ start, end });
    }
    
    // Shuffle the intervals to insert them in random order
    std::shuffle(testIntervals.begin(), testIntervals.end(), gen);
    
    Vector<std::pair<Interval, Value>> insertedData;
    for (const auto& interval : testIntervals) {
        Value value = valueDist(gen);
        insertedData.append({ interval, value });

        intervalSet.insert(interval, value);
    }
    
    // Test that all inserted intervals can be found with correct values
    for (const auto& data : insertedData) {
        EXPECT_TRUE(intervalSet.hasOverlap(data.first));
        const Value* found = intervalSet.find(data.first);
        EXPECT_NE(nullptr, found);
        EXPECT_EQ(data.second, *found);
    }
    
    std::uniform_int_distribution<size_t> pointDist(0, maxPoint);
    // Test random queries
    for (unsigned i = 0; i < 500; ++i) {
        Point start = pointDist(gen);
        Point end = start + sizeDist(gen);
        Interval query = { start, end };
        
        bool shouldOverlap = false;
        Value expectedValue = 0;
        for (const auto& data : insertedData) {
            if (query.overlaps(data.first)) {
                shouldOverlap = true;
                expectedValue = data.second;
                break;
            }
        }
        
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

} // namespace TestWebKitAPI