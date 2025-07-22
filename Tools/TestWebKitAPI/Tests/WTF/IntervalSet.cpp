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
#include <wtf/Range.h>

namespace TestWebKitAPI {

TEST(WTF_IntervalSet, Basic)
{
    IntervalSet<int, int> intervalSet;
    
    // Test empty set
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(0, 10)));
    EXPECT_EQ(nullptr, intervalSet.find(Range<int>(0, 10)));
}

TEST(WTF_IntervalSet, SingleInterval)
{
    IntervalSet<int, int> intervalSet;
    
    // Insert a single interval [10, 20) with value 42
    intervalSet.insert(Range<int>(10, 20), 42);
    
    // Test overlap detection
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(15, 25)));  // Overlaps
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(5, 15)));   // Overlaps
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(10, 20)));  // Exact match
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(0, 10)));  // No overlap (adjacent)
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(20, 30))); // No overlap (adjacent)
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(0, 5)));   // No overlap (before)
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(25, 30))); // No overlap (after)
    
    // Test find
    const int* value = intervalSet.find(Range<int>(15, 16));
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(42, *value);
    
    // Test find with non-overlapping interval
    EXPECT_EQ(nullptr, intervalSet.find(Range<int>(0, 5)));
}

TEST(WTF_IntervalSet, MultipleIntervals)
{
    IntervalSet<int, int> intervalSet;
    
    // Insert multiple non-overlapping intervals
    intervalSet.insert(Range<int>(10, 20), 1);
    intervalSet.insert(Range<int>(30, 40), 2);
    intervalSet.insert(Range<int>(50, 60), 3);
    
    // Test overlap detection
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(15, 25)));  // Overlaps first
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(35, 45)));  // Overlaps second
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(55, 65)));  // Overlaps third
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(20, 30))); // Gap between first and second
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(40, 50))); // Gap between second and third
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(0, 10)));  // Before all intervals
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(60, 70))); // After all intervals
    
    // Test find for each interval
    const int* value1 = intervalSet.find(Range<int>(15, 16));
    EXPECT_NE(nullptr, value1);
    EXPECT_EQ(1, *value1);
    
    const int* value2 = intervalSet.find(Range<int>(35, 36));
    EXPECT_NE(nullptr, value2);
    EXPECT_EQ(2, *value2);
    
    const int* value3 = intervalSet.find(Range<int>(55, 56));
    EXPECT_NE(nullptr, value3);
    EXPECT_EQ(3, *value3);
    
    // Test find in gaps
    EXPECT_EQ(nullptr, intervalSet.find(Range<int>(25, 26)));
    EXPECT_EQ(nullptr, intervalSet.find(Range<int>(45, 46)));
}

TEST(WTF_IntervalSet, DifferentTypes)
{
    // Test with different value type
    IntervalSet<int, const char*> intervalSet;
    
    intervalSet.insert(Range<int>(0, 100), "first");
    intervalSet.insert(Range<int>(200, 300), "second");
    
    const char* const* value = intervalSet.find(Range<int>(50, 60));
    EXPECT_NE(nullptr, value);
    EXPECT_STREQ("first", *value);
    
    value = intervalSet.find(Range<int>(250, 260));
    EXPECT_NE(nullptr, value);
    EXPECT_STREQ("second", *value);
    
    EXPECT_EQ(nullptr, intervalSet.find(Range<int>(150, 160)));
}

TEST(WTF_IntervalSet, EdgeCases)
{
    IntervalSet<int, int> intervalSet;
    
    // Insert interval [0, 1) - single unit interval
    intervalSet.insert(Range<int>(0, 1), 100);
    
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(0, 1)));
    EXPECT_FALSE(intervalSet.hasOverlap(Range<int>(1, 2)));
    
    const int* value = intervalSet.find(Range<int>(0, 1));
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(100, *value);
    
    // Test with larger intervals that span the small one
    EXPECT_TRUE(intervalSet.hasOverlap(Range<int>(-10, 10)));
    value = intervalSet.find(Range<int>(-10, 10));
    EXPECT_NE(nullptr, value);
    EXPECT_EQ(100, *value);
}

} // namespace TestWebKitAPI