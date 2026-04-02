/*
 * Copyright (C) 2025-2026 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(B3_JIT)

#include "AirPhaseStats.h"
#include "B3Bank.h"

namespace JSC { namespace B3 { namespace Air {

#define FOR_EACH_REGISTER_ALLOCATOR_STAT(macro) \
    macro(numTmpsIn, unsigned)                            \
    macro(numFastTmps, unsigned)                          \
    macro(numUnspillableTmps, unsigned)                   \
    macro(numSpillTmps, unsigned)                         \
    macro(numTmpsOut, unsigned)                           \
    macro(numCoalescedRegisterMoves, unsigned)            \
    macro(numCoalescedStackSlotMoves, unsigned)           \
    macro(numCoalescedPinned, unsigned)                   \
    macro(numSpilledTmps, unsigned)                       \
    macro(numSpillStackSlots, unsigned)                   \
    macro(numLoadSpill, unsigned)                         \
    macro(numStoreSpill, unsigned)                        \
    macro(numInPlaceSpill, unsigned)                      \
    macro(numInPlaceSpillGiveUpSpillWidth, unsigned)      \
    macro(numMoveSpillSpillInsts, unsigned)               \
    macro(numRematerializeConst, unsigned)                \
    macro(weightedLoadSpill, float)                       \
    macro(weightedStoreSpill, float)                      \
    macro(weightedInPlaceSpill, float)                    \
    macro(weightedRematerializeConst, float)              \
    macro(maxLiveRangeSize, unsigned)                     \
    macro(maxLiveRangeIntervals, unsigned)                \
    macro(didSpill, unsigned)                             \
    macro(numSplitAroundClobbers, unsigned)               \
    macro(numSplitAroundClobberSpilled, unsigned)         \
    macro(numSplitIntraBlockNoCluster, unsigned)          \
    macro(numSplitIntraBlock, unsigned)                   \
    macro(numSplitIntraBlockClusterTmps, unsigned)        \
    macro(numSplitIntraBlockClusterTmpsSpilled, unsigned) \
    macro(numSplitIntraBlockLoad, unsigned)               \
    macro(numSplitIntraBlockStore, unsigned)              \
    macro(numSplitAroundLoop, unsigned)                   \
    macro(numSplitAroundLoopBothSpilled, unsigned)        \
    macro(numSplitAroundLoopLoopSpilled, unsigned)        \
    macro(numSplitAroundLoopNonLoopSpilled, unsigned)     \
    macro(numSplitAroundLoopBailNoLoop, unsigned)         \
    macro(numSplitAroundLoopBailAlreadySplitAroundClobbers, unsigned)   \
    macro(numSplitAroundLoopBailLocalOnly, unsigned)      \
    macro(numSplitAroundLoopBailTooSmall, unsigned)       \
    macro(numSplitAroundLoopBailConstDef, unsigned)       \
    macro(numSplitAroundLoopBailTooDeep, unsigned)        \
    macro(numSplitAroundLoopSkipDisjointRange, unsigned)  \
    macro(numGroupTmpsCoalesced, unsigned)                \
    macro(numGroupsCreated, unsigned)                     \
    macro(numGroupMovesCoalesced, unsigned)               \
    macro(numGroupConstDefMerged, unsigned)               \
    macro(maxGroupSize, unsigned)                         \
    macro(numInsts, unsigned)                             \

class AirAllocateRegistersStats {
public:
    AirAllocateRegistersStats(Bank bank)
        : m_bank(bank) { }

    ASCIILiteral name() const
    {
        return m_bank == GP ? "RegAlloc<GP>"_s : "RegAlloc<FP>"_s;
    }

    DEFINE_PHASE_STATS(AirAllocateRegistersStats, FOR_EACH_REGISTER_ALLOCATOR_STAT)

private:
    Bank m_bank;
};

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
