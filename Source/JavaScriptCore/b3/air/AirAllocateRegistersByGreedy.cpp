/*
 * Copyright (C) 2024-2025 Apple Inc. All rights reserved.
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
#include "AirAllocateRegistersByGreedy.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirCode.h"
#include "AirFixSpillsAfterTerminals.h"
#include "AirPhaseInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLiveness.h"
#include "AirPadInterference.h"
#include "AirPhaseScope.h"
#include "AirRegLiveness.h"
#include "AirTmpMap.h"
#include "AirTmpWidth.h"
#include "AirUseCounts.h"
#include <wtf/IterationStatus.h>
#include <wtf/ListDump.h>
#include <wtf/PriorityQueue.h>
#include <wtf/Range.h>

using WTF::Range;

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

// FIXME: anonymous namespace (combines with LinearScan due to unified sources)
namespace Greedy {

static bool verbose() { return Options::airGreedyRegAllocVerbose(); }

// Phase constants used for the PhaseInsertionSet.
const unsigned spillStore = 0;
const unsigned splitMoveTo = 1;
const unsigned splitMoveFrom = 2;
const unsigned spillLoad = 3;

const size_t splitMinRangeSize = 8;

typedef Range<size_t> Interval;

class LiveRange {
public:
    LiveRange() = default;

    inline void validate()
    {
#if ASSERT_ENABLED
        size_t size = 0;
        Interval* prevInterval = nullptr;
        for (auto& interval : m_intervals) {
            ASSERT(interval.begin() < interval.end());
            ASSERT(!prevInterval || prevInterval->end() < interval.begin());
            size += interval.distance();
            prevInterval = &interval;
        }
        ASSERT(size == m_size);
#endif
    }

    void prepend(Interval interval)
    {
        if (m_intervals.isEmpty() || interval.end() < m_intervals.first().begin())
            m_intervals.prepend(WTFMove(interval));
        else {
            ASSERT(interval.end() == m_intervals.first().begin());
            m_intervals.first() |= interval;
        }
        m_size += interval.distance();
        validate();
    }

    void append(Interval interval)
    {
        ASSERT(m_intervals.isEmpty() || m_intervals.last().end() < interval.begin());
        m_intervals.append(WTFMove(interval));
        m_size += interval.distance();
        validate();
    }

    void crossBasicBlockBoundary()
    {
        crossesBasicBlockBoundary = true;
    }

    bool isLocal()
    {
        return !crossesBasicBlockBoundary && m_intervals.size() <= 1;
    }

    const Deque<Interval>& intervals() const
    {
        return m_intervals;
    }

    size_t size()
    {
        return m_size;
    }

    bool overlaps(LiveRange& other)
    {
        auto otherIter = other.intervals().begin();
        auto otherEnd = other.intervals().end();
        for (auto interval : intervals()) {
            while (otherIter != otherEnd && otherIter->end() <= interval.begin())
                ++otherIter; // otherIter was entirely before interval
            if (otherIter == otherEnd)
                return false;
            // Either otherIter overlaps interval or otherIter is entirely after interval.
            if (otherIter->begin() < interval.end())
                return true;
        }
        return false;
    }

    static LiveRange merge(const LiveRange& a, const LiveRange& b)
    {
        auto appendRanges = [](LiveRange& dest, auto iter, auto end) {
            for (; iter != end; ++iter) {
                dest.m_intervals.append(*iter);
                dest.m_size += iter->distance();
            }
        };

        auto consumeOverlapping = [](LiveRange& dest, auto& iter, auto end) {
            bool merged = false;
            auto& last = dest.m_intervals.last();
            while (iter != end && (last.end() == iter->begin() || last.overlaps(*iter))) {
                last |= *iter;
                ++iter;
                merged = true;
            }
            return merged;
        };

        LiveRange result;
        auto aIter = a.intervals().begin();
        auto bIter = b.intervals().begin();

        while (true) {
            if (aIter == a.intervals().end()) {
                appendRanges(result, bIter, b.intervals().end());
                break;
            }
            if (bIter == b.intervals().end()) {
                appendRanges(result, aIter, a.intervals().end());
                break;
            }
            ASSERT(aIter != a.intervals().end() && bIter != b.intervals().end());
            if (aIter->begin() < bIter->begin()) {
                result.m_intervals.append(*aIter);
                ++aIter;
            } else {
                result.m_intervals.append(*bIter);
                ++bIter;
            }
            bool merged;
            do {
                merged = false;
                merged |= consumeOverlapping(result, aIter, a.intervals().end());
                merged |= consumeOverlapping(result, bIter, b.intervals().end());
            } while (merged);
            result.m_size += result.intervals().last().distance();
        }
        result.validate();
        return result;
    }

    static LiveRange subtract(const LiveRange& a, const LiveRange& b)
    {
        LiveRange result;
        auto aIter = a.intervals().begin();
        auto bIter = b.intervals().begin();

        if (aIter == a.intervals().end())
            return result;
        Interval interval = *aIter;
        ++aIter;

        while (true) {
            // Skip over intervals in b that come before the current interval of a.
            while (bIter != b.intervals().end() && bIter->end() <= interval.begin())
                ++bIter;
            
            if (bIter != b.intervals().end() && bIter->overlaps(interval)) {
                // Overlap: Split the interval into 0, 1 or 2 intervals.
                if (interval.begin() < bIter->begin())
                    result.append({ interval.begin(), bIter->begin() });
                if (bIter->end() < interval.end()) {
                    // Process remaining portion of the interval.
                    interval = { bIter->end(), interval.end() };
                    continue;
                }
            } else {
                // No overlap: include entire interval in result.
                result.append(interval);
            }
            // Finished processing interval; move on to the next.
            if (aIter == a.intervals().end())
                break;
            interval = *aIter;
            ++aIter;
        }
        result.validate();
        return result;
    }

    void dump(PrintStream& out) const
    {
        WTF::CommaPrinter comma;
        out.print("{ ");
        for (auto& interval : intervals())
            out.print(comma, interval);
        out.print(" }[", m_size, "]");
    }

private:
    Deque<Interval> m_intervals;
    size_t m_size { 0 };
    bool crossesBasicBlockBoundary { false };
};

enum class Stage {
    New,
    Unspillable,
    TryAllocate,
    TrySplit,
    Spill,
    Assigned,
    Coalesced,
    Spilled,
    Replaced,
};

struct QueueElement {
    QueueElement(Tmp tmp, Stage stage, size_t rangeSize, bool maybeCoalescable, bool isLocal)
        : tmp(tmp)
        , stage(stage)
        , rangeSize(rangeSize)
        , maybeCoalescable(maybeCoalescable)
        , isLocal(isLocal)
    {
    }

    void dump(PrintStream& out) const
    {
        out.print("<", tmp, ", ", stage, ", ", maybeCoalescable, ", ", rangeSize, ">");
    }
 
    static bool isHigherPriority(const QueueElement& left, const QueueElement& right)
    {
        ASSERT(!left.tmp.isReg() && !right.tmp.isReg());
        // FIXME: could prepack so this can be a single comparison.
        if (left.stage < right.stage)
            return true;
        if (left.stage > right.stage)
            return false;

        if (left.maybeCoalescable && !right.maybeCoalescable)
            return true;
        if (!left.maybeCoalescable && right.maybeCoalescable)
            return false;

        if (!left.isLocal && right.isLocal)
            return true;
        if (left.isLocal && !right.isLocal)
            return false;

        if (left.rangeSize > right.rangeSize)
            return true;
        if (left.rangeSize < right.rangeSize)
            return false;

        // Guarantee a strict total ordering for determinism.
        return left.tmp.tmpIndex() < right.tmp.tmpIndex();
    }

    Tmp tmp;
    Stage stage;
    size_t rangeSize;
    bool maybeCoalescable;
    bool isLocal;
};

static constexpr float unspillableCost = std::numeric_limits<float>::infinity();
static constexpr float fastTmpSpillCost = std::numeric_limits<float>::max();
static_assert(unspillableCost > fastTmpSpillCost);

class RegisterRanges {
public:
    RegisterRanges() = default;

    struct AllocatedInterval {
        Tmp tmp;
        Interval interval;

        bool operator<(const AllocatedInterval& other) const
        {
            return this->interval.end() < other.interval.end();
        }

        void dump(PrintStream& out) const
        {
            out.print("{ ", tmp, " ", interval, " }");
        }
    };

    typedef StdSet<AllocatedInterval> AllocatedIntervalSet;

    void add(Tmp tmp, LiveRange& range)
    {
        ASSERT(!hasConflict(range)); // Can't add overlapping LiveRanges
        for (auto& interval : range.intervals()) {
            ASSERT(interval != Interval()); // Strict ordering requires no empty intervals.
            m_allocations.insert({ tmp, interval });
        }
    }

    void evict(Tmp tmp, LiveRange& range)
    {
        for (auto& interval : range.intervals()) {
            auto r = m_allocations.erase({ tmp, interval });
            ASSERT_UNUSED(r, r == 1);
        }
    }

    bool hasConflict(LiveRange& range)
    {
        bool hasConflict = false;
        forEachConflict(range, [&](auto&) -> IterationStatus {
            hasConflict = true;
            return IterationStatus::Done;
        });
        return hasConflict;
    }

    template<typename Func>
    void forEachConflict(const LiveRange& range, const Func& func)
    {
        auto rangeIter = range.intervals().begin();
        auto rangeEnd = range.intervals().end();

        if (rangeIter == rangeEnd)
            return;
        auto nextSearch = rangeIter->begin();

        while (true) {
            AllocatedInterval conflict;
            {
                auto conflictIter = findFirstIntervalEndingAfter(nextSearch);
                if (conflictIter == m_allocations.end())
                    return; // End of 'm_allocations', so no more potential conflicts
                if (rangeIter->end() <= conflictIter->interval.begin()) {
                    // No more conflicts of this 'range' interval. Move on to the next interval in 'range'.
                    if (++rangeIter == rangeEnd)
                        return; // End of 'range', so no more potential conflicts
                    // Start searching for conflicts of the next 'range' interval.
                    nextSearch = rangeIter->begin();
                    continue;
                }
                // Found a conflict. There may be additional conflicts of this 'range' interval, so advance
                // the search position beyond this conflict but don't advance the 'range' interval.
                conflict = *conflictIter;
                nextSearch = conflictIter->interval.end();
            }
            // 'func' can invalidate iterators of 'm_allocations'.
            if (func(conflict) == IterationStatus::Done)
                return;
        }
    }

    void dump(PrintStream& out) const
    {
        CommaPrinter comma;
        out.print("[");
        for (auto& alloc : m_allocations) {
            out.print(comma);
            out.print(alloc);
        }
        out.print("]");
    }

private:
    AllocatedIntervalSet::iterator findFirstIntervalEndingAfter(size_t pos)
    {
        Interval query(pos);
        // pos can be 0, yet we can't express a non-empty interval with end==0, so instead of looking
        // for the first interval ending after pos we find the first interval ending at or after pos+1.
        ASSERT(query.end() == pos + 1);
        auto iter = m_allocations.lower_bound({ Tmp(), query });
        ASSERT(iter == m_allocations.end() || iter->interval.end() > pos);
        return iter;
    }

    AllocatedIntervalSet m_allocations;
};

struct AffinityWith {
    void dump(PrintStream& out) const
    {
        out.print("(", tmp, ", ", weight, ")");
    }

    Tmp tmp;
    float weight;
};

struct TmpData {
    void dump(PrintStream& out) const
    {
        out.print("{liveRange = ", liveRange, ", preferredReg = ", preferredReg, ", affinity = ", listDump(affinity), ", subGroup0 = ", subGroup0, ", subGroup1 = ", subGroup1, " spillCost = ", spillCost, ", stage = ", stage, ", assigned = ", assigned, ", spilled = ", pointerDump(spillSlot), "}");
    }

    bool isGroup()
    {
        ASSERT(!subGroup0 == !subGroup1);
        return !!subGroup0;
    }

    void validate()
    {
        ASSERT(!(spillSlot && assigned));
        ASSERT(!!assigned == (stage == Stage::Assigned));
        ASSERT_IMPLIES(spillSlot, stage == Stage::Spilled);
        ASSERT_IMPLIES(spillSlot, spillCost != unspillableCost);
        ASSERT_IMPLIES(spillSlot, !isGroup()); // Should have been split
        ASSERT_IMPLIES(!spillCost, !liveRange.size());
        ASSERT_IMPLIES(assigned, !parentGroup); // Only top-most should be assigned
        ASSERT_IMPLIES(affinity.size(), !isGroup()); // Only bottom-most should have affinity
    }

    LiveRange liveRange;
    float spillCost { 0.0f };
    Reg preferredReg;
    Vector<AffinityWith> affinity;
    Tmp parentGroup;
    Tmp subGroup0, subGroup1;

    Stage stage { Stage::New };
    Reg assigned;
    StackSlot* spillSlot { nullptr };
    size_t splitMetadataIndex { 0 };
};

struct SplitMetadata {
    void dump(PrintStream& out) const
    {
        out.print(originalTmp, " : { ", listDump(gapTmps), " } ");
    }

    Tmp originalTmp;
    Vector<Tmp> gapTmps;
};

class GreedyAllocator {
public:
    static constexpr bool eagerGroups = true;
    static constexpr bool eagerGroupsSplitFully = false;

    GreedyAllocator(Code& code)
        : m_code(code)
        , m_headIndex(code.size())
        , m_tailIndicies(code.size())
        , m_map(code)
        , m_splitMetadata(1) // Sacrifice index 0.
        , m_regRanges(Reg::maxIndex() + 1)
        , m_insertionSets(code.size())
        , m_useCounts(m_code)
        , m_tmpWidth(m_code)
    {
    }

    void run()
    {
        padInterference(m_code);
        buildRegisterSets();
        buildIndices();
        buildIntervals();
        initSpillCosts<GP>();
        initSpillCosts<FP>();
        finalizeAffinity<GP>();
        finalizeAffinity<FP>();

        allocateRegisters<GP>();
        allocateRegisters<FP>();

        insertFixupCode();
        assignRegisters();
        fixSpillsAfterTerminals(m_code);
    }

private:
    void buildRegisterSets()
    {
        forEachBank([&] (Bank bank) {
                m_allowedRegistersInPriorityOrder[bank] = m_code.regsInPriorityOrder(bank);
                for (Reg r : m_allowedRegistersInPriorityOrder[bank])
                    m_allAllowedRegisters.add(r, IgnoreVectors);
        });
        m_allAllowedRegistersWholeWidth = m_allAllowedRegisters.toRegisterSet().includeWholeRegisterWidth();
    }

    void buildIndices()
    {
        size_t headIndex = 0;
        size_t tailIndex = 0;
        for (size_t i = 0; i < m_code.size(); i++) {
            BasicBlock* block = m_code[i];
            if (!block) {
                m_tailIndicies[i] = tailIndex;
                continue;
            }
            tailIndex = headIndex + 2 * block->size() - 1;
            m_headIndex[block] = headIndex;
            m_tailIndicies[i] = tailIndex;
            headIndex += 2 * block->size();
        }
    }

    BasicBlock* findBlockContainingIndex(size_t index)
    {
        auto iter = std::lower_bound(m_tailIndicies.begin(), m_tailIndicies.end(), index);
        ASSERT(iter != m_tailIndicies.end()); // Should ask only about legal instruction boundaries.
        size_t blockIndex = std::distance(m_tailIndicies.begin(), iter);
        BasicBlock* block = m_code[blockIndex];
        ASSERT(indexOfHead(block) <= index && index <= indexOfTail(block));
        return block;
    }

    size_t indexOfHead(BasicBlock* block)
    {
        return m_headIndex[block];
    }

    size_t indexOfTail(BasicBlock* block)
    {
        return indexOfHead(block) + block->size() * 2 - 1;
    }

    static size_t instIndex(size_t indexOfHead, Interval interval)
    {
        return (interval.begin() - indexOfHead) / 2;
    }

    static size_t indexOfEarly(Interval interval)
    {
        return interval.begin() & ~static_cast<size_t>(1);
    }

    static Interval earlyInterval(size_t indexOfEarly)
    {
        ASSERT(!(indexOfEarly & 1));
        return Interval(indexOfEarly);
    }

    static Interval lateInterval(size_t indexOfEarly)
    {
        ASSERT(!(indexOfEarly & 1));
        return Interval(indexOfEarly + 1);
    }

    static Interval earlyAndLateInterval(size_t indexOfEarly)
    {
        return earlyInterval(indexOfEarly) | lateInterval(indexOfEarly);
    }

    static Interval interval(size_t indexOfEarly, Arg::Timing timing)
    {
        switch (timing) {
        case Arg::OnlyEarly:
            return earlyInterval(indexOfEarly);
        case Arg::OnlyLate:
            return lateInterval(indexOfEarly);
        case Arg::EarlyAndLate:
            return earlyAndLateInterval(indexOfEarly);
        }
        ASSERT_NOT_REACHED();
        return Interval();
    }

    static Interval intervalForSpill(size_t indexOfEarly, Arg::Role role)
    {
        Arg::Timing timing = Arg::timing(role);
        switch (timing) {
        case Arg::OnlyEarly:
            if (Arg::isAnyDef(role))
                return earlyAndLateInterval(indexOfEarly); // We have a spill store after this insn.
            return earlyInterval(indexOfEarly);
        case Arg::OnlyLate:
            if (Arg::isAnyUse(role))
                return earlyAndLateInterval(indexOfEarly); // We had a spill load before this insn.
            return lateInterval(indexOfEarly);
        case Arg::EarlyAndLate:
            return earlyAndLateInterval(indexOfEarly);
        }
        ASSERT_NOT_REACHED();
        return Interval();
    }

    Tmp groupForTmp(Tmp tmp)
    {
        while (Tmp parent = m_map[tmp].parentGroup)
            tmp = parent;
        return tmp;
    }

    Reg assignedReg(Tmp tmp)
    {
        return m_map[groupForTmp(tmp)].assigned;
    }

    StackSlot* spillSlot(Tmp tmp)
    {
        return m_map[groupForTmp(tmp)].spillSlot;
    }

    float adjustedBlockFrequency(BasicBlock* block)
    {
        float freq = block->frequency();
        if (UNLIKELY(!m_fastBlocks.saw(block)))
            freq *= Options::rareBlockPenalty();
        return freq;
    }

    void buildIntervals()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::buildIntervals"_s);
        UnifiedTmpLiveness liveness(m_code);
        TmpMap<Interval> activeIntervals(m_code);

        // Find non-rare blocks.
        m_fastBlocks.push(m_code[0]);
        while (BasicBlock* block = m_fastBlocks.pop()) {
            for (FrequentedBlock& successor : block->successors()) {
                if (!successor.isRare())
                    m_fastBlocks.push(successor.block());
            }
        }

        auto coalescableMoveSrc = [&](Inst& inst) {
            return mayBeCoalescable(inst) ? inst.args[0].tmp() : Tmp();
        };

        auto addAffinity = [&](Tmp a, Tmp b, BasicBlock* block) {
            TmpData& tmpData = m_map[a];
            float freq = adjustedBlockFrequency(block);
            for (AffinityWith& with : tmpData.affinity) {
                if (with.tmp == b) {
                    with.weight += freq;
                    return;
                }
            }
            tmpData.affinity.append(AffinityWith{ b, freq });
        };

        auto pruneAffinity = [&](Inst& inst, Tmp def) {
            TmpData& defData = m_map[def];
            if (!defData.affinity.size())
                return;
            Tmp movSrc = coalescableMoveSrc(inst);
            dataLogLnIf(verbose(), "Checking affinity ", inst, " def=", def, " movSrc=", movSrc);
            defData.affinity.removeAllMatching([&](AffinityWith& with) {
                if (with.tmp != movSrc && activeIntervals[with.tmp]) {
                    dataLogLnIf(verbose(), "Pruning affinity ", def, " ", with.tmp);
                    m_map[with.tmp].affinity.removeAllMatching([def](AffinityWith& with) {
                        return with.tmp == def;
                    });
                    return true;
                }
                return false;
            });
        };

        auto closeInterval = [&](Tmp tmp) {
            ASSERT(activeIntervals[tmp] != Interval());
            m_map[tmp].liveRange.prepend(activeIntervals[tmp]);
            activeIntervals[tmp] = Interval();
        };

        for (BasicBlock* block : m_code) {
            if (!block)
                continue;
            for (Inst& inst : block->insts()) {
                if (mayBeCoalescable(inst)) {
                    ASSERT(inst.args.size() == 2);
                    if (inst.args[0].isReg() || inst.args[1].isReg()) {
                        unsigned regIdx = inst.args[0].isReg() ? 0 : 1;
                        Reg reg = inst.args[regIdx].reg();
                        if (m_allAllowedRegisters.contains(reg, IgnoreVectors)) {
                            Tmp other = inst.args[regIdx ^ 1].tmp();
                            if (!m_map[other].preferredReg)
                                m_map[other].preferredReg = inst.args[regIdx].reg();
                        }
                    } else {
                        ASSERT(inst.args[0].isTmp() && inst.args[1].isTmp());
                        addAffinity(inst.args[0].tmp(), inst.args[1].tmp(), block);
                        addAffinity(inst.args[1].tmp(), inst.args[0].tmp(), block);
                    }
                }
            }
        }

        BasicBlock* blockAfter = nullptr;
        for (size_t blockIndex = m_code.size(); blockIndex--;) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;

            size_t indexOfHead = this->indexOfHead(block);
            size_t indexOfTail = this->indexOfTail(block);
            if (verbose()) {
                dataLog("At BB", pointerDump(block), "\n");
                dataLog("  indexOfHead = ", indexOfHead, "\n");
                dataLog("  indexOfTail = ", indexOfTail, "\n");
            }

            for (Tmp tmp : liveness.liveAtTail(block))
                activeIntervals[tmp] |= Interval(indexOfTail); // FIXME: could just set interval start

            if (blockAfter) {
                for (Tmp tmp : liveness.liveAtHead(blockAfter)) {
                    if (activeIntervals[tmp].contains(indexOfTail))
                        m_map[tmp].liveRange.crossBasicBlockBoundary();
                    else {
                        // If tmp was live at the head of the next block but no longer live, close
                        // the current interval.
                        ASSERT(activeIntervals[tmp].begin() == this->indexOfHead(blockAfter));
                        closeInterval(tmp);
                    }
                }
            }

            for (unsigned instIndex = block->size(); instIndex--;) {
                Inst& inst = block->at(instIndex);
                size_t indexOfEarly = indexOfHead + instIndex * 2;

                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    auto& interval = activeIntervals[tmp];
                    if (Arg::isLateUse(role))
                        interval |= lateInterval(indexOfEarly);
                    if (Arg::isLateDef(role)) {
                        interval |= lateInterval(indexOfEarly);
                        closeInterval(tmp);
                        pruneAffinity(inst, tmp);
                    }
                    if (Arg::isEarlyUse(role))
                        interval |= earlyInterval(indexOfEarly);
                    if (Arg::isEarlyDef(role)) {
                        interval |= earlyInterval(indexOfEarly);
                        closeInterval(tmp);
                        pruneAffinity(inst, tmp);
                    }
                });
                if (inst.kind.opcode == Patch) {
                    auto clobberReg = [&](Reg reg, Interval interval) {
                        Tmp tmp = Tmp(reg);
                        bool isAlive = !!activeIntervals[tmp];
                        activeIntervals[tmp] |= interval;
                        if (!isAlive)
                            closeInterval(tmp);
                    };
                    inst.extraClobberedRegs().forEachWithWidthAndPreserved(
                        [&](Reg reg, Width, PreservedWidth) {
                            clobberReg(reg, lateInterval(indexOfEarly));
                        });
                    inst.extraEarlyClobberedRegs().forEachWithWidthAndPreserved(
                        [&](Reg reg, Width, PreservedWidth) {
                            clobberReg(reg, earlyInterval(indexOfEarly));
                        });
                }

            }
            for (Tmp tmp : liveness.liveAtHead(block))
                activeIntervals[tmp] |= Interval(indexOfHead);

            blockAfter = block;
        }
        if (blockAfter) {
            for (Tmp tmp : liveness.liveAtHead(blockAfter)) {
                ASSERT(activeIntervals[tmp].begin() == this->indexOfHead(blockAfter));
                closeInterval(tmp);
            }
        }

#if ASSERT_ENABLED
        m_code.forAllTmps([&](Tmp tmp) {
            ASSERT(!activeIntervals[tmp]);
        });
#endif
        if (verbose()) {
            dataLog("Intervals:\n");
            auto dumpRegTmpData = [&](Reg r) {
                TmpData& tmpData = m_map[Tmp(r)];
                if (tmpData.liveRange.size())
                    dataLog("    ", r, ": ", m_map[Tmp(r)], "\n");
            };
            for (Reg r : m_allowedRegistersInPriorityOrder[GP])
                dumpRegTmpData(r);
            for (Reg r : m_allowedRegistersInPriorityOrder[FP])
                dumpRegTmpData(r);
            m_code.forEachTmp([&](Tmp tmp) {
                dataLog("    ", tmp, ": ", m_map[tmp], "\n");
            });
        }
    }

    template<typename Func>
    IterationStatus forEachTmpInGroup(Tmp tmp, const Func& func)
    {
        TmpData& data = m_map[tmp];
        if (!data.isGroup())
            return func(tmp);
        ASSERT(data.subGroup0 && data.subGroup1);
        if (forEachTmpInGroup(data.subGroup0, func) == IterationStatus::Done)
            return IterationStatus::Done;
        return forEachTmpInGroup(data.subGroup1, func);
    }

    template <Bank bank>
    void finalizeAffinity()
    {
        struct Move {
            Tmp tmp0, tmp1;
            float weight;

            void dump(PrintStream& out) const
            {
                out.print(tmp0, ", ", tmp1, " ", weight);
            }
        };
        Vector<Move> moves;

        m_code.forAllTmps([&](Tmp tmp) {
            if (tmp.bank() != bank || tmp.isReg())
                return;

            TmpData& data = m_map[tmp];
            std::sort(data.affinity.begin(), data.affinity.end(),
                [this] (AffinityWith& a, AffinityWith& b) -> bool {
                    if (a.weight != b.weight)
                        return a.weight > b.weight;
                    // Favor coalescing shorter live ranges.
                    auto aSize = m_map[a.tmp].liveRange.size();
                    auto bSize = m_map[b.tmp].liveRange.size();
                    if (aSize != bSize)
                        return aSize < bSize;
                    return a.tmp.tmpIndex(bank) < b.tmp.tmpIndex(bank);
            });

            if (!eagerGroups)
                return;

            for (AffinityWith& with : m_map[tmp].affinity) {
                if (tmp.tmpIndex(bank) < with.tmp.tmpIndex(bank))
                    moves.append({ tmp, with.tmp, with.weight });
            }
        });

        ASSERT_IMPLIES(!eagerGroups, moves.isEmpty());
        std::sort(moves.begin(), moves.end(),
            [](Move& a, Move& b) -> bool {
                if (a.weight != b.weight)
                    return a.weight > b.weight;
                if (a.tmp0.tmpIndex(bank) != b.tmp1.tmpIndex(bank))
                    return a.tmp0.tmpIndex(bank) < a.tmp0.tmpIndex(bank);
                ASSERT(a.tmp1.tmpIndex(bank) != b.tmp1.tmpIndex(bank));
                return a.tmp1.tmpIndex(bank) < b.tmp1.tmpIndex(bank);
            });

        auto hasConflict = [this](Tmp grp0, Tmp grp1) {
            bool conflicts = false;
            forEachTmpInGroup(grp0, [&](Tmp tmp0) {
                ASSERT(!conflicts);
                TmpData& data0 = m_map[tmp0];
                ASSERT(!data0.subGroup0 && !data0.subGroup1);
                forEachTmpInGroup(grp1, [&](Tmp tmp1) {
                    ASSERT(!conflicts);
                    ASSERT(tmp0 != tmp1);
                    TmpData& data1 = m_map[tmp1];
                    if (!data0.affinity.containsIf([tmp1](auto& with) { return with.tmp == tmp1; })
                        && data0.liveRange.overlaps(data1.liveRange)) {
                        conflicts = true;
                        return IterationStatus::Done;
                    }
                    return IterationStatus::Continue;
                });
                return conflicts ? IterationStatus::Done : IterationStatus::Continue;
            });
            return conflicts;
        };

        auto addSubGroup = [this](Tmp group, TmpData& groupData, Tmp& subGroupField, Tmp subGroup) {
            TmpData& subGroupData = m_map[subGroup];
            subGroupField = subGroup;
            subGroupData.parentGroup = group;
            subGroupData.stage = Stage::Coalesced;

            groupData.liveRange = LiveRange::merge(groupData.liveRange, subGroupData.liveRange);
            groupData.spillCost += subGroupData.spillCost;
            if (!groupData.preferredReg)
                groupData.preferredReg = subGroupData.preferredReg;

            Width defWidth, useWidth;
            defWidth = std::max(m_tmpWidth.defWidth(group), m_tmpWidth.defWidth(subGroup));
            useWidth = std::max(m_tmpWidth.useWidth(group), m_tmpWidth.useWidth(subGroup));
            m_tmpWidth.setWidths(group, useWidth, defWidth);
        };

        for (Move& move : moves) {
            dataLogLnIf(verbose(), "Processing move: ", move);
            Tmp grp0 = groupForTmp(move.tmp0);
            Tmp grp1 = groupForTmp(move.tmp1);
            if (grp0 == grp1) {
                dataLogLnIf(verbose(), "Already grouped transitively into ", grp0);
                continue;
            }
            if (!hasConflict(grp0, grp1)) {
                Tmp newGrp = m_code.newTmp(bank);
                TmpData newGrpData;
                m_tmpWidth.setWidths(newGrp, Width8, Width8);

                addSubGroup(newGrp, newGrpData, newGrpData.subGroup0, grp0);
                addSubGroup(newGrp, newGrpData, newGrpData.subGroup1, grp1);
                newGrpData.validate();
                m_map.append(newGrp, newGrpData);
                dataLogLnIf(verbose(), "Created group ", newGrp, ": ", m_map[newGrp]);
            }
        }
        if (verbose()) {
            m_code.forAllTmps([&](Tmp tmp) {
                if (tmp.bank() != bank || tmp.isReg())
                    return;
                TmpData& data = m_map[tmp];
                if (!data.parentGroup && data.isGroup()) {
                    dataLog("Group: ", tmp, " = { ");
                    CommaPrinter comma;
                    forEachTmpInGroup(tmp, [&comma](Tmp member) {
                        dataLog(comma, member);
                        return IterationStatus::Continue;
                    });
                    dataLogLn(" }");
                }
            });
        }
    }

    template<Bank bank>
    void initSpillCosts()
    {
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank])
            m_map[Tmp(reg)].spillCost = unspillableCost;

        // XXX: FIXME: tmps alive only in one gap should be unspillable.
        m_code.forEachTmp([&](Tmp tmp) {
            if (tmp.bank() != bank || tmp.isReg())
                return;
            auto index = AbsoluteTmpMapper<bank>::absoluteIndex(tmp);
            float spillCost = m_useCounts.numWarmUsesAndDefs<bank>(index);
            if (bank == GP && m_useCounts.isConstDef<GP>(index))
                spillCost /= 2; // Can rematerialize rather than spill in many cases.
            ASSERT(m_map[tmp].spillCost == 0.0f);
            m_map[tmp].spillCost = spillCost;
        });
        m_code.forEachFastTmp([&](Tmp tmp) {
            if (tmp.bank() != bank)
                return;
            m_map[tmp].spillCost = fastTmpSpillCost;
            dataLogLnIf(verbose(), "FastTmp: ", tmp);
        });
    }

    // newTmp creates and returns a new tmp that can hold the values of 'from'.
    // Note that all TmpData references invalidated since it may expand/realloc the TmpData map.
    Tmp newTmp(Tmp from, float spillCost, Interval interval)
    {
        Tmp tmp = m_code.newTmp(from.bank());
        m_tmpWidth.setWidths(tmp, m_tmpWidth.useWidth(from), m_tmpWidth.defWidth(from));

        m_map.append(tmp, TmpData());
        TmpData& tmpData = m_map[tmp];
        tmpData.liveRange.prepend(interval);
        tmpData.spillCost = spillCost;
        tmpData.validate();
        return tmp;
    }

    Tmp addSpillTmpWithInterval(Tmp spilledTmp, Interval interval)
    {
        Tmp tmp = newTmp(spilledTmp, unspillableCost, interval);
        dataLogLnIf(verbose(), "New spill for ", spilledTmp, " tmp: ", tmp, ": ", m_map[tmp]);
        setStageAndEnqueue(tmp, m_map[tmp], Stage::Unspillable);
        return tmp;
    }

    void dumpRegRanges(Bank bank)
    {
        for (Reg r : m_allowedRegistersInPriorityOrder[bank])
            dataLogLn("   regRanges[", r, "]: ", m_regRanges[r]);
    }

    void setStageAndEnqueue(Tmp tmp, TmpData& tmpData, Stage stage)
    {
        ASSERT(!tmp.isReg());
        ASSERT(stage == Stage::Unspillable || stage == Stage::TryAllocate || stage == Stage::TrySplit || stage == Stage::Spill);
        ASSERT_IMPLIES(!tmpData.spillCost, !tmpData.liveRange.size());
        ASSERT(!tmpData.parentGroup); // Group member should not be enquened
        ASSERT_IMPLIES(!eagerGroups, !tmpData.isGroup());
        tmpData.validate();

        tmpData.stage = stage;
        size_t rangeSizeOrStart = tmpData.liveRange.size();
        if (tmpData.liveRange.isLocal())
            rangeSizeOrStart = tmpData.liveRange.intervals().first().begin();

        m_queue.enqueue({ tmp, stage, rangeSizeOrStart, tmpData.preferredReg || tmpData.affinity.size(), tmpData.liveRange.isLocal() });
        dataLogLnIf(verbose(), "Enqueued (", stage, ") ", tmp);
    }

    template <Bank bank>
    void allocateRegisters()
    {
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank])
            assign(Tmp(reg), m_map[Tmp(reg)], reg);

        m_code.forEachTmp(
            [&] (Tmp tmp) {
                if (tmp.bank() != bank || tmp.isReg())
                    return;
                TmpData& tmpData = m_map[tmp];
                if (tmpData.parentGroup) {
                    ASSERT(eagerGroups);
                    return;
                }
                if (tmpData.liveRange.intervals().isEmpty())
                    return;
                setStageAndEnqueue(tmp, tmpData, Stage::TryAllocate);
        });

        do {
            while (!m_queue.isEmpty()) {
                auto entry = m_queue.dequeue();
                Tmp tmp = entry.tmp;
                TmpData& tmpData = m_map[tmp];
                if (verbose()) {
                    dataLogLn("Pop: ", entry, " tmp: ", tmpData);
                    dumpRegRanges(bank);
                }
                if (tryAllocate<bank>(tmp, tmpData))
                    continue;
                if (tmpData.stage != Stage::TrySplit && tryEvict<bank>(tmp, tmpData))
                    continue;

                switch (tmpData.stage) {
                case Stage::TryAllocate:
                    // If we couldn't allocate tmp, allow it to split next time.
                    setStageAndEnqueue(tmp, tmpData, tmpData.liveRange.size() >= splitMinRangeSize ? Stage::TrySplit : Stage::Spill);
                    continue;
                case Stage::TrySplit:
                    if (!trySplit<bank>(tmp, tmpData))
                        setStageAndEnqueue(tmp, tmpData, Stage::Spill);
                    continue;
                case Stage::Spill:
                    ASSERT(queueContainsOnlySpills()); // XXX: remove
                    spill(tmp, tmpData);
                    continue;
                case Stage::Unspillable:
                    // Unspillables must have been allocated during tryAllocate or tryEvict.
                    RELEASE_ASSERT_NOT_REACHED();
                case Stage::New:
                case Stage::Assigned:
                case Stage::Spilled:
                case Stage::Coalesced:
                case Stage::Replaced:
                    // Tmps in these stages should not have been enqueued.
                    RELEASE_ASSERT_NOT_REACHED();
                }
                RELEASE_ASSERT_NOT_REACHED();
            }
            if (m_didSpill) {
                emitSpillCodeAndEnqueueNewTmps<bank>();
                m_didSpill = false;
            }
            // Process the spill/fill tmps,
        } while (!m_queue.isEmpty());
    }

    template <Bank bank>
    bool tryAllocate(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(&m_map[tmp] == &tmpData);
        ASSERT(!assignedReg(tmp));
        ASSERT(!tmpData.parentGroup);

        auto tryAllocateToReg = [&](Reg r) {
            LiveRange& liveRange = tmpData.liveRange;
            RegisterRanges& regRanges = m_regRanges[r];
            if (!regRanges.hasConflict(liveRange)) {
                assign(tmp, tmpData, r);
                return true;
            }
            return false;
        };

        ScalarRegisterSet alreadyAttempted;
        // FIXME: this will check affinities within the group, which is wasteful and common.
        // But without it, we won't try to affinitize between partially split groups.
#if 0
        IterationStatus status = forEachTmpInGroup(tmp, [&](Tmp member) {
            for (auto& with : m_map[member].affinity) {
                Reg r = assignedReg(with.other);
                if (r) {
                    if (tryAllocateToReg(r))
                        return IterationStatus::Done;
                    alreadyAttempted.add(r, IgnoreVectors);
                }
            }
            return IterationStatus::Continue;
        });
        if (status == IterationStatus::Done) {
            ASSERT(tmpData.assigned);
            return true;
        }
#else
        for (auto& with : tmpData.affinity) {
            Reg r = m_map[with.tmp].assigned;
            if (r) {
                if (tryAllocateToReg(r))
                    return true;
                alreadyAttempted.add(r, IgnoreVectors);
            }
        }

#endif
        ASSERT(!tmpData.assigned);

        if (tmpData.preferredReg) {
            if (tryAllocateToReg(tmpData.preferredReg))
                return true;
            alreadyAttempted.add(tmpData.preferredReg, IgnoreVectors);
        }
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (alreadyAttempted.contains(r, IgnoreVectors))
                continue;
            if (tryAllocateToReg(r))
                return true;
        }
        return false;
    }

    // FIXME: need some mechanism to avoid infinite eviction loops. (LLVM uses "Cascade").
    template <Bank bank>
    bool tryEvict(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(&m_map[tmp] == &tmpData);
        ASSERT(tmp.bank() == bank);

        Reg bestEvictReg;
        float minSpillCost = unspillableCost;
        LiveRange& liveRange = tmpData.liveRange;
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            float conflictsSpillCost = 0.0f;
            // TODO: maybe use IndexSparseSet instead and hoist construction (but that takes wrong type).
            // Or add a way to reset IndexSet and hoist construction.
            IndexSet<Tmp::Indexed<bank>> visited;
            m_regRanges[r].forEachConflict(liveRange,
                [&] (auto& conflict) -> IterationStatus {
                    if (conflict.tmp.isReg()) {
                        // Conflicts with a register clobber, cannot evict clobbers.
                        conflictsSpillCost = unspillableCost;
                        return IterationStatus::Done;
                    }
                    if (visited.contains(conflict.tmp))
                        return IterationStatus::Continue;
                    visited.add(conflict.tmp);
                    auto cost = m_map[conflict.tmp].spillCost;
                    if (cost == unspillableCost) {
                        conflictsSpillCost = unspillableCost;
                        return IterationStatus::Done;
                    }
                    conflictsSpillCost += cost;
                    return IterationStatus::Continue;
            });
            if (conflictsSpillCost < minSpillCost) {
                minSpillCost = conflictsSpillCost;
                bestEvictReg = r;
            }
        }
        if (minSpillCost >= tmpData.spillCost) {
            // If 'tmp' was unspillable, we better have found at least one suitable register.
            RELEASE_ASSERT(tmpData.spillCost != unspillableCost);
            return false;
        }
        // It's cheaper to spill all the already-assigned conflicting tmps, so evict them in favor of assigning 'tmp'.
        m_regRanges[bestEvictReg].forEachConflict(liveRange,
            [&](auto& conflict) -> IterationStatus {
                TmpData& conflictData = m_map[conflict.tmp];
                evict(conflict.tmp, conflictData, bestEvictReg);
                setStageAndEnqueue(conflict.tmp, conflictData, Stage::TryAllocate);
                return IterationStatus::Continue;
            });
        assign(tmp, tmpData, bestEvictReg);
        return true;
    }

    void assign(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        m_regRanges[reg].add(tmp, tmpData.liveRange);
        ASSERT(tmpData.stage != Stage::Assigned && tmpData.stage != Stage::Spilled);
        tmpData.stage = Stage::Assigned;
        tmpData.assigned = reg;
        dataLogLnIf(verbose(), "Assigned ", tmp, " to ", reg);
        tmpData.validate();
    }

    void evict(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(tmpData.stage == Stage::Assigned);
        ASSERT(tmpData.spillCost != unspillableCost);
        ASSERT(tmpData.assigned == reg);
        m_regRanges[reg].evict(tmp, tmpData.liveRange);
        tmpData.stage = Stage::New;
        tmpData.assigned = Reg();
        dataLogLnIf(verbose(), "Evicted ", tmp, " from ", reg);
        tmpData.validate();
    }

    template<Bank bank>
    bool trySplit(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(tmpData.spillCost != unspillableCost); // Should have evicted.
        if (trySplitGroup(tmp, tmpData))
            return true;
        return trySplitAroundClobbers<bank>(tmp, tmpData);
    }

    bool trySplitGroup(Tmp tmp, TmpData& tmpData)
    {
        if (!tmpData.isGroup())
            return false;
        ASSERT(eagerGroups);
        auto enqueueSubgroup = [&](Tmp subGrp) {
            m_map[subGrp].parentGroup = Tmp();
            setStageAndEnqueue(subGrp, m_map[subGrp], Stage::TryAllocate);
        };
        if (eagerGroupsSplitFully) {
            forEachTmpInGroup(tmp, [&](Tmp member) {
                enqueueSubgroup(member);
                return IterationStatus::Continue;
            });
        } else {
            enqueueSubgroup(tmpData.subGroup0);
            enqueueSubgroup(tmpData.subGroup1);
        }
        tmpData.stage = Stage::Replaced;
        dataLogLnIf(verbose(), "Split (group) ", tmp);
        tmpData.validate();
        return true;
    }

    template<Bank bank>
    bool trySplitAroundClobbers(Tmp tmp, TmpData& tmpData)
    {
        static unsigned count;

        if (tmpData.splitMetadataIndex)
            return false; // Already split around clobbers
        if (tmpData.liveRange.size() < splitMinRangeSize)
            return false; // Not enough instructions to be worthwhile

        if (Options::airGreedyLimit() && count >= Options::airGreedyLimit())
            return false;

        auto instUsesOrDefsTmp = [](Inst& inst, Tmp tmp) {
            bool result = false;
            inst.forEachTmpFast([&](Tmp useOrDef) {
                result |= useOrDef == tmp;
            });
            return result;
        };

        Reg bestSplitReg;
        float minSplitCost = unspillableCost;
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            float splitCost = 0.0f;
            m_regRanges[r].forEachConflict(tmpData.liveRange,
                [&](auto& conflict) -> IterationStatus {
                    if (conflict.tmp.isReg() && conflict.interval.distance() == 1) {
                        // Block freq * rare block penalty
                        BasicBlock* block = findBlockContainingIndex(conflict.interval.begin());
                        unsigned instIndex = this->instIndex(indexOfHead(block), conflict.interval);
                        Inst& inst = block->at(instIndex);
                        if (instUsesOrDefsTmp(inst, tmp)) {
                            // If the inst that clobbers regs also use/def the tmp trying to be split, then
                            // can't split the tmp around this clobber.
                            // FixMe: could allow uses, but then we'd have to make split tmp conflict with any
                            // spill tmps used by this instruction, so unclear if that's better.
                            dataLogLnIf(verbose(), "XXX use/def: tmp=", tmp, " inst = ", inst);
                            splitCost = unspillableCost;
                            return IterationStatus::Done;
                        }
                        // Times 2 for MOV tmp, split & MOV split, tmp
                        splitCost += adjustedBlockFrequency(block) * 2;
                        return IterationStatus::Continue;
                    }
                    // Conflict with non clobber - don't try to split.
                    splitCost = unspillableCost;
                    return IterationStatus::Done;
                });
            if (splitCost < minSplitCost) {
                minSplitCost = splitCost;
                bestSplitReg = r;
            }
        }
        ASSERT(tmpData.spillCost != unspillableCost); // Should have evicted.
        if (minSplitCost >= unspillableCost) //tmpData.spillCost) // FixMe: Use a multiple?
            return false; // Better to spill than to split.

        LiveRange allGapsRange;
        m_regRanges[bestSplitReg].forEachConflict(tmpData.liveRange,
            [&](auto& conflict) -> IterationStatus {
                ASSERT(conflict.tmp.isReg() && conflict.interval.distance() == 1);
                // Extend interval to include both early and late since we'll insert a Move
                // before and after the clobbering instruction.
                Interval gapInterval = earlyAndLateInterval(indexOfEarly(conflict.interval));
                allGapsRange.append(gapInterval);
                return IterationStatus::Continue;
            });

        size_t size = tmpData.liveRange.size();
        tmpData.liveRange = LiveRange::subtract(tmpData.liveRange, allGapsRange);
        ASSERT(tmpData.liveRange.size() + allGapsRange.size() == size);
        tmpData.splitMetadataIndex = m_splitMetadata.size();
        setStageAndEnqueue(tmp, tmpData, Stage::TryAllocate);

        SplitMetadata metadata;
        metadata.originalTmp = tmp;
        // Create tmps to carry the value across register clobbering instructions. These tmps
        // might spill or be assigned another register.
        for (Interval gapInterval : allGapsRange.intervals()) {
            float freq = 2 * adjustedBlockFrequency(findBlockContainingIndex(gapInterval.begin()));
            Tmp gapTmp = newTmp(tmp, freq, gapInterval);
            metadata.gapTmps.append(gapTmp);
            setStageAndEnqueue(gapTmp, m_map[gapTmp], Stage::TryAllocate);
        }
        dataLogLnIf(verbose(), "Split (clobbers): reg = ", bestSplitReg, " splitCost = ", minSplitCost, " split tmp = ", metadata);
        m_splitMetadata.append(WTFMove(metadata));

        count++;
        return true;
    }

    static unsigned stackSlotMinimumWidth(Width width)
    {
        if (width <= Width32)
            return 4;
        if (width <= Width64)
            return 8;
        ASSERT(width == Width128);
        return 16;
    }

    void spill(Tmp tmp, TmpData& tmpData)
    {
        RELEASE_ASSERT(tmpData.spillCost != unspillableCost);
        ASSERT(tmpData.assigned == Reg());
        ASSERT(!tmpData.isGroup()); // Should have been split
        tmpData.stage = Stage::Spilled;

        dataLogLnIf(verbose(), "Spilled ", tmp);
        if (tmpData.splitMetadataIndex) {
            dataLogLnIf(verbose(), "   evicting tmps created during split");
            auto& metadata = m_splitMetadata[tmpData.splitMetadataIndex];
            ASSERT(metadata.originalTmp == tmp);
            // Splitting didn't prevent originalTmp from spilling after all, so no point assigning
            // registers or stack slots to the gap tmps for this split.
            for (Tmp gapTmp : metadata.gapTmps) {
                Reg reg = m_map[gapTmp].assigned;
                if (reg)
                    evict(gapTmp, m_map[gapTmp], reg);
                m_map[gapTmp].stage = Stage::Replaced;
            }
        }
        // Batch generating spill tmps so that we can limit traversals of the code without
        // needing to keep track of each tmp's use/defs.
        // FIXME: revisit if live range splitting needs that info anyway.
        // FIXME: this might not be the best if e.g. an unspillable forces a loop tmp to be spilled.
        m_didSpill = true;
        tmpData.validate();
    }

    bool queueContainsOnlySpills()
    {
        for (auto& elem : m_queue) {
            if (elem.stage != Stage::Spill) {
                return false;
            }
        }
        return true;
    }

    Opcode moveOpcode(Tmp tmp)
    {
        Opcode move = Oops;
        Width width = m_tmpWidth.requiredWidth(tmp);
        switch (stackSlotMinimumWidth(width)) {
        case 4:
            move = tmp.bank() == GP ? Move32 : MoveFloat;
            break;
        case 8:
            move = tmp.bank() == GP ? Move : MoveDouble;
            break;
        case 16:
            ASSERT(tmp.bank() == FP);
            move = MoveVector;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        return move;
    }

    // FIXME: merge with linear scan emitSpillCode().
    template <Bank bank>
    void emitSpillCodeAndEnqueueNewTmps()
    {
        m_code.forAllTmps([&](Tmp tmp) {
            TmpData& tmpData = m_map[tmp];
            if (tmpData.stage == Stage::Spilled && !tmpData.spillSlot)
                tmpData.spillSlot = m_code.addStackSlot(stackSlotMinimumWidth(m_tmpWidth.requiredWidth(tmp)), StackSlotKind::Spill);
        });
        // FIXME: this is too inefficient to do for each spilled tmp, one at a time.
        for (BasicBlock* block : m_code) {
            size_t indexOfHead = this->indexOfHead(block);
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                unsigned indexOfEarly = indexOfHead + instIndex * 2;

                // The TmpWidth analysis will say that a Move only stores 32 bits into the destination,
                // if the source only had 32 bits worth of non-zero bits. Same for the source: it will
                // only claim to read 32 bits from the source if only 32 bits of the destination are
                // read. Note that we only apply this logic if this turns into a load or store, since
                // Move is the canonical way to move data between GPRs.
                bool canUseMove32IfDidSpill = false;
                bool didSpill = false;
                bool needScratch = false;
                Tmp scratchForTmp;
                if (bank == GP && inst.kind.opcode == Move) {
                    if ((inst.args[0].isTmp() && m_tmpWidth.width(inst.args[0].tmp()) <= Width32)
                        || (inst.args[1].isTmp() && m_tmpWidth.width(inst.args[1].tmp()) <= Width32))
                        canUseMove32IfDidSpill = true;
                }

                // Try to replace the register use by memory use when possible.
                inst.forEachArg(
                    [&] (Arg& arg, Arg::Role role, Bank argBank, Width width) {
                        if (!arg.isTmp())
                            return;
                        if (argBank != bank)
                            return;
                        if (arg.isReg())
                            return;

                        StackSlot* spilled = spillSlot(arg.tmp());
                        if (!spilled)
                            return;
                        bool needScratchIfSpilledInPlace = false;
                        if (!inst.admitsStack(arg)) {
                            if (false) // XXX
                                dataLog("Have an inst that won't admit stack: ", inst, "\n");
                            switch (inst.kind.opcode) {
                            case Move:
                            case MoveDouble:
                            case MoveFloat:
                            case Move32: {
                                unsigned argIndex = &arg - &inst.args[0];
                                unsigned otherArgIndex = argIndex ^ 1;
                                Arg otherArg = inst.args[otherArgIndex];
                                if (inst.args.size() == 2
                                    && otherArg.isStack()
                                    && otherArg.stackSlot()->isSpill()) {
                                    needScratchIfSpilledInPlace = true;
                                    break;
                                }
                                return;
                            }
                            default:
                                return;
                            }
                        }
                        // If the Tmp holds a constant then we want to rematerialize its
                        // value rather than loading it from the stack. In order for that
                        // optimization to kick in, we need to avoid placing the Tmp's stack
                        // address into the instruction.
                        if (!Arg::isColdUse(role) && m_useCounts.isConstDef<bank>(AbsoluteTmpMapper<bank>::absoluteIndex(arg.tmp())))
                            return;

                        Width spillWidth = m_tmpWidth.requiredWidth(arg.tmp());
                        if (Arg::isAnyDef(role) && width < spillWidth) {
                            // Either there are users of this tmp who will use more than width,
                            // or there are producers who will produce more than width non-zero
                            // bits.
                            // FIXME: It's not clear why we should have to return here. We have
                            // a ZDef fixup in allocateStack. And if this isn't a ZDef, then it
                            // doesn't seem like it matters what happens to the high bits. Note
                            // that this isn't the case where we're storing more than what the
                            // spill slot can hold - we already got that covered because we
                            // stretch the spill slot on demand. One possibility is that it's ZDefs of
                            // smaller width than 32-bit.
                            // https://bugs.webkit.org/show_bug.cgi?id=169823
                            return;
                        }
                        ASSERT(inst.kind.opcode == Move || !(Arg::isAnyUse(role) && width > spillWidth));
                        
                        if (spillWidth != Width32)
                            canUseMove32IfDidSpill = false;
                        
                        spilled->ensureSize(canUseMove32IfDidSpill ? 4 : bytesForWidth(width));
                        didSpill = true;
                        if (needScratchIfSpilledInPlace) {
                            needScratch = true;
                            scratchForTmp = arg.tmp();
                        }
                        arg = Arg::stack(spilled);
                    });

                if (didSpill && canUseMove32IfDidSpill)
                    inst.kind.opcode = Move32;
                
                if (needScratch) {
                    ASSERT(scratchForTmp != Tmp());
                    // XXX does this need to be EarlyAndLate? Does it matter?
                    Tmp tmp = addSpillTmpWithInterval(scratchForTmp, intervalForSpill(indexOfEarly, Arg::Scratch));
                    inst.args.append(tmp);
                    RELEASE_ASSERT(inst.args.size() == 3);
                    continue;
                }

                // For every other case, add Load/Store as needed.
                inst.forEachTmp([&] (Tmp& tmp, Arg::Role role, Bank argBank, Width) {
                    if (tmp.isReg() || argBank != bank)
                        return;
                    StackSlot* spilled = spillSlot(tmp);
                    if (!spilled)
                        return;

                    Opcode move = moveOpcode(tmp);
                    auto oldTmp = tmp;
                    tmp = addSpillTmpWithInterval(tmp, intervalForSpill(indexOfEarly, role));
                    if (role == Arg::Scratch)
                        return;

                    Arg arg = Arg::stack(spilled);
                    // FIXME: try rematerialize
                    if (Arg::isAnyUse(role)) {
                        auto tryRematerialize = [&]() {
                            if constexpr (bank == GP) {
                                auto oldIndex = AbsoluteTmpMapper<bank>::absoluteIndex(oldTmp);
                                if (m_useCounts.isConstDef<bank>(oldIndex)) {
                                    int64_t value = m_useCounts.constant<bank>(oldIndex);
                                    if (Arg::isValidImmForm(value) && isValidForm(Move, Arg::Imm, Arg::Tmp)) {
                                        m_insertionSets[block].insert(instIndex, spillLoad, Move, inst.origin, Arg::imm(value), tmp);
                                        dataLogLnIf(verbose(), "Rematerialized (imm) ", oldTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                        return true;
                                    }
                                    if (isValidForm(Move, Arg::BigImm, Arg::Tmp)) {
                                        m_insertionSets[block].insert(instIndex, spillLoad, Move, inst.origin, Arg::bigImm(value), tmp);
                                        dataLogLnIf(verbose(), "Rematerialized (bigImm) ", oldTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };

                        if (!tryRematerialize())
                            m_insertionSets[block].insert(instIndex, spillLoad, move, inst.origin, arg, tmp);
                    }
                    if (Arg::isAnyDef(role))
                        m_insertionSets[block].insert(instIndex + 1, spillStore, move, inst.origin, tmp, arg);
                });
            }
        }
    }

    void insertFixupCode()
    {
        for (auto& metadata : m_splitMetadata) {
            if (!metadata.originalTmp)
                continue;
            if (spillSlot(metadata.originalTmp))
                continue; // If spilled, better to not split after all
            ASSERT(assignedReg(metadata.originalTmp));
            // Emit moves to and from each tmp (or stack stot) that fills the split gaps.
            for (Tmp gapTmp : metadata.gapTmps) {
                TmpData& gapData = m_map[gapTmp];
                for (auto& interval : gapData.liveRange.intervals()) {
                    ASSERT(interval.distance() == 2);
                    BasicBlock* block = findBlockContainingIndex(interval.begin());
                    unsigned instIndex = this->instIndex(indexOfHead(block), interval);
                    Inst& inst = block->at(instIndex);

                    Arg arg = gapTmp;
                    StackSlot* spilled = spillSlot(gapTmp);
                    if (spilled)
                        arg = Arg::stack(spilled);
                    // XXX what to use for origin?
                    Opcode move = moveOpcode(gapTmp);
                    m_insertionSets[block].insert(instIndex, splitMoveFrom, move, inst.origin, metadata.originalTmp, arg);
                    m_insertionSets[block].insert(instIndex + 1, splitMoveTo, move, inst.origin, arg, metadata.originalTmp);
                    dataLogLnIf(verbose(), "Inserted Moves around clobber tmp=", metadata.originalTmp, " gapTmp=", gapTmp, " block=", *block, " index=", instIndex, " inst = ", inst);
                }
            }
        }

        for (BasicBlock* block : m_code)
            m_insertionSets[block].execute(block);
    }

    // FIXME: combine with graph coloring version?
    bool mayBeCoalescable(Inst& inst)
    {
        switch (inst.kind.opcode) {
        case Move:
        case Move32:
        case MoveFloat:
        case MoveDouble:
        case MoveVector:
            break;
        default:
            return false;
        }

        // Avoid the three-argument coalescable spill moves.
        if (inst.args.size() != 2)
            return false;

        if (!inst.args[0].isTmp() || !inst.args[1].isTmp())
            return false;

        // We can coalesce a Move32 so long as either of the following holds:
        // - The input is already zero-filled.
        // - The output only cares about the low 32 bits.
        //
        // Note that the input property requires an analysis over ZDef's, so it's only valid so long
        // as the input gets a register. We don't know if the input gets a register, but we do know
        // that if it doesn't get a register then we will still emit this Move32.
        if (inst.kind.opcode == Move32 && !is32Bit() && m_tmpWidth.defWidth(inst.args[0].tmp()) > Width32)
            return false;
        return true;
    }

    void assignRegisters()
    {
        if (verbose()) {
            dataLog("About to assign registers. State of all tmps:\n");
            m_code.forEachTmp(
                [&] (Tmp tmp) {
                    dataLog("    ", tmp, ": ", m_map[tmp], "\n");
                });
            dataLog("IR:\n");
            dataLog(m_code);
        }

        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                bool mayBeCoalescable = this->mayBeCoalescable(inst);

                dataLogLnIf(verbose(), "At: ", inst, mayBeCoalescable ? " [coalescable]" : "");

                if constexpr (isX86_64()) {
                    // Move32 is cheaper if we know that it's equivalent to a Move in x86_64. It's
                    // equivalent if the destination's high bits are not observable or if the source's high
                    // bits are all zero.
                    if (inst.kind.opcode == Move && inst.args[0].isTmp() && inst.args[1].isTmp()) {
                        if (m_tmpWidth.useWidth(inst.args[1].tmp()) <= Width32 || m_tmpWidth.defWidth(inst.args[0].tmp()) <= Width32)
                            inst.kind.opcode = Move32;
                    }
                }
                if constexpr (isARM64()) {
                    // On the other hand, on ARM64, Move is cheaper than Move32. We would like to use Move instead of Move32.
                    // Move32 on ARM64 is explicitly selected in B3LowerToAir for ZExt32 for example. But using ZDef information
                    // here can optimize it from Move32 to Move.
                    if (inst.kind.opcode == Move32 && inst.args[0].isTmp() && inst.args[1].isTmp()) {
                        if (m_tmpWidth.defWidth(inst.args[0].tmp()) <= Width32)
                            inst.kind.opcode = Move;
                    }
                }

                inst.forEachTmpFast([&](Tmp& tmp) {
                    if (tmp.isReg())
                        return;

                    Reg reg = assignedReg(tmp);
                    if (!reg) {
                        dataLog("Failed to allocate reg for: ", tmp, "\n");
                        RELEASE_ASSERT_NOT_REACHED();
                    }
                    tmp = Tmp(reg);
                });

                if (mayBeCoalescable && inst.args[0].isTmp() && inst.args[1].isTmp() 
                    && inst.args[0].tmp() == inst.args[1].tmp())
                    inst = Inst();
            }
            // Remove all the useless moves we created in this block.
            block->insts().removeAllMatching([&] (const Inst& inst) {
                return !inst;
            });
        }
    }

    Code& m_code;
    Vector<Reg> m_allowedRegistersInPriorityOrder[numBanks];
    ScalarRegisterSet m_allAllowedRegisters;
    RegisterSet m_allAllowedRegistersWholeWidth;
    IndexMap<BasicBlock*, size_t> m_headIndex;
    Vector<size_t> m_tailIndicies;
    TmpMap<TmpData> m_map;
    Vector<SplitMetadata> m_splitMetadata;
    IndexMap<Reg, RegisterRanges> m_regRanges;
    PriorityQueue<QueueElement, QueueElement::isHigherPriority> m_queue;
    IndexMap<BasicBlock*, PhaseInsertionSet> m_insertionSets;
    BlockWorklist m_fastBlocks;
    UseCounts m_useCounts;
    TmpWidth m_tmpWidth;
    bool m_didSpill { false };
};

} // anonymous namespace

void allocateRegistersByGreedy(Code& code)
{
    RELEASE_ASSERT(!code.usesSIMD());
    PhaseScope phaseScope(code, "allocateRegistersAndStackByGreedy"_s);
    if (Greedy::verbose())
        dataLog("Air before greedy register allocation:\n", code);
    Greedy::GreedyAllocator allocator(code);
    allocator.run();
    if (Greedy::verbose())
        dataLog("Air after greedy register allocation:\n", code);
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
