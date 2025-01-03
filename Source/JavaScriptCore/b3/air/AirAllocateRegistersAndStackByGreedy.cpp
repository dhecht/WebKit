/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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
#include "AirAllocateRegistersAndStackByGreedy.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirCode.h"
#include "AirFixSpillsAfterTerminals.h"
#include "AirHandleCalleeSaves.h"
#include "AirPhaseInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLiveness.h"
#include "AirPadInterference.h"
#include "AirPhaseScope.h"
#include "AirRegLiveness.h"
#include "AirStackAllocation.h"
#include "AirTmpMap.h"
#include <wtf/IterationStatus.h>
#include <wtf/ListDump.h>
#include <wtf/PriorityQueue.h>
#include <wtf/Range.h>

using WTF::Range;

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

// FIXME: anonymous namespace (combines with LinearScan due to unified sources)
namespace Greedy {

static bool verbose() { return Options::airLinearScanVerbose(); }

// Phase constants we use for the PhaseInsertionSet.
const unsigned firstPhase = 0;
const unsigned secondPhase = 1;

typedef Range<size_t> Interval;

class LiveRange {
public:
    LiveRange() = default;

    void prepend(Interval interval)
    {
        // Intervals should be ordered, non-overlapping, and non-contiguous.
        ASSERT(m_intervals.isEmpty() || interval.end() < m_intervals.last().begin());
        m_intervals.prepend(WTFMove(interval));
        m_size += m_intervals.first().distance();
    }

    const Deque<Interval>& intervals() const
    {
        return m_intervals;
    }

    size_t size()
    {
        return m_size;
    }

    void dump(PrintStream& out) const 
    {
        WTF::CommaPrinter comma;
        out.print("{ ");
        for (auto& interval : intervals())
            out.print(comma, interval);
        out.print(" }");
    }

private:
    Deque<Interval> m_intervals;
    size_t m_size { 0 };
};

enum class Stage {
    New,
    Split,
    Spill,
    Unspillable,
    Assigned,
    Spilled,
};

struct QueueElement {
    QueueElement(Tmp tmp, Stage stage, size_t rangeSize)
        : tmp(tmp)
        , stage(stage)
        , rangeSize(rangeSize)
    {
    }

    void dump(PrintStream& out) const
    {
        out.print("<", tmp, ", ", stage, ", ", rangeSize, ">");
    }
 
    static bool isHigherPriority(const QueueElement& left, const QueueElement& right)
    {
        ASSERT(!left.tmp.isReg() && !right.tmp.isReg());
        // FIXME: could prepack so this can be a single comparison.
        if (left.stage < right.stage)
            return true;
        if (left.stage == right.stage) {
            if (left.rangeSize > right.rangeSize)
                return true;
            if (left.rangeSize == right.rangeSize)
                return left.tmp.tmpIndex() < right.tmp.tmpIndex();
            return false;
        }
        return false;
    }

    Tmp tmp;
    Stage stage;
    size_t rangeSize;
};

static constexpr float unspillableCost = std::numeric_limits<float>::infinity();

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

    void addClobber(Reg r, size_t pos)
    {
        m_allocations.insert({ Tmp(r), { pos, pos + 1 }} );
    }

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
            ASSERT_UNUSED(r == 1, r);
        }
    }

    bool hasConflict(LiveRange& range)
    {
        bool hasConflict = false;
        forEachConflict(range, [&] (Tmp) -> IterationStatus {
            hasConflict = true;
            return IterationStatus::Done;
        });
        return hasConflict;
    }

    template<typename Func>
    void forEachConflict(LiveRange& range, const Func& func)
    {
        for (auto& interval: range.intervals()) {
            Tmp conflict;
            {
                auto iter = findFirstIntervalEndingAfter(interval.begin());
                // Since all allocated intervals have an end before this LiveRange interval begins (and intervals
                // are sorted), there must not exist any allocated intervals that overlap a later LiveRange interval.
                if (iter == m_allocations.end())
                    return;
                // iter references the first allocated interval with an end greater than
                // the LiveRange interval's begin. Therefore, iff the allocated interval's begin
                // is less than the LiveRange interval's end, these intervals overlap. Furthermore, we know
                // that no later (in sorted order) allocated interval can overlap this LiveRange interval since all 
                // later allocated intervals' begin is greater than or equal to the LiveRange's end.
                if (interval.end() <= iter->interval.begin())
                    continue;
                conflict = iter->tmp;
            } // func is allowed to invalidate the iterator.
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

struct TmpData {
    void dump(PrintStream& out) const
    {
        out.print("{liveRange = ", liveRange, ", spilled = ", pointerDump(spilled), ", assigned = ", assigned, ", isUnspillable = ", isUnspillable, ", possibleRegs = ", possibleRegs, ", didBuildPossibleRegs = ", didBuildPossibleRegs, "}");
    }

    void validate()
    {
        RELEASE_ASSERT(!(spilled && assigned));
    }

    Interval interval;
    LiveRange liveRange;
    Stage stage { Stage::New };
    float spillCost { 1.0f }; // FIXME
    StackSlot* spilled { nullptr };
    ScalarRegisterSet possibleRegs;
    Reg assigned;
    bool isUnspillable { false };
    bool didBuildPossibleRegs { false };
    unsigned spillIndex { 0 };
};

struct Clobber {
    Clobber() = default;

    Clobber(size_t index, RegisterSet regs)
        : index(index)
        , regs(regs)
    {
    }

    void dump(PrintStream& out) const
    {
        out.print(index, ":", regs);
    }

    size_t index { 0 };
    RegisterSet regs;
};

class GreedyAllocator {
public:
    GreedyAllocator(Code& code)
        : m_code(code)
        , m_startIndex(code.size())
        , m_map(code)
        , m_regRanges(Reg::maxIndex() + 1)
        , m_insertionSets(code.size())
    {
    }

    void run()
    {
        padInterference(m_code);
        buildRegisterSetBuilder();
        buildIndices();
        buildIntervals();
        if (shouldSpillEverything()) {
            spillEverything();
            emitSpillCode();
        }
        allocateRegisters<GP>();
        allocateRegisters<FP>();
#if 0            
        for (;;) {
            prepareIntervalsForScanForRegisters();
            m_didSpill = false;
            forEachBank(
                [&] (Bank bank) {
                    attemptScanForRegisters(bank);
                });
            if (!m_didSpill)
                break;
            emitSpillCode();
        }
#endif
        insertSpillCode();
        assignRegisters();
        fixSpillsAfterTerminals(m_code);

        handleCalleeSaves(m_code);
        allocateEscapedStackSlots(m_code);
        prepareIntervalsForScanForStack();
        scanForStack();
        updateFrameSizeBasedOnStackSlots(m_code);
        m_code.setStackIsAllocated(true);
    }

private:
    void buildRegisterSetBuilder()
    {
        forEachBank(
            [&] (Bank bank) {
                m_allowedRegistersInPriorityOrder[bank] = m_code.regsInPriorityOrder(bank);
                for (Reg r : m_allowedRegistersInPriorityOrder[bank])
                    m_allowedRegisters[bank].add(r, IgnoreVectors);
                m_allAllowedRegisters = m_allAllowedRegisters.toRegisterSet()
                    .merge(m_allowedRegisters[bank].toRegisterSet())
                    .buildScalarRegisterSet();
            });
    }

    void buildIndices()
    {
        size_t index = 0;
        for (BasicBlock* block : m_code) {
            m_startIndex[block] = index;
            index += block->size() * 2;
        }
    }

    size_t indexOfHead(BasicBlock* block)
    {
        return m_startIndex[block];
    }

    size_t indexOfTail(BasicBlock* block)
    {
        // FIXME: added -1, isn't it a bug with linear scan?
        return indexOfHead(block) + block->size() * 2 - 1;
    }

    static Interval earlyInterval(size_t indexOfEarly)
    {
        return Interval(indexOfEarly);
    }

    static Interval lateInterval(size_t indexOfEarly)
    {
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

    void buildClobbers(UnifiedTmpLiveness& liveness, BasicBlock* block)
    {
        size_t indexOfHead = this->indexOfHead(block);
        RegLiveness::LocalCalcForUnifiedTmpLiveness localCalc(liveness, block);

        auto record = [&] (unsigned instIndex) {
            // FIXME: This could get the register sets from somewhere else, like the
            // liveness constraints. Except we want those constraints to separate the late
            // actions of one instruction from the early actions of the next.
            // https://bugs.webkit.org/show_bug.cgi?id=170850
            const auto& regs = localCalc.live();
            if (Inst* prev = block->get(instIndex - 1)) {
                RegisterSetBuilder prevRegs = regs;
                prev->forEach<Reg>(
                    [&] (Reg& reg, Arg::Role role, Bank, Width width) {
                        if (Arg::isLateDef(role))
                            prevRegs.add(reg, width);
                    });
                if (prev->kind.opcode == Patch)
                    prevRegs.merge(prev->extraClobberedRegs());
                prevRegs.filter(m_allAllowedRegisters.toRegisterSet().includeWholeRegisterWidth());
                if (!prevRegs.isEmpty())
                    m_clobbers.append(Clobber(indexOfHead + instIndex * 2 - 1, prevRegs.buildAndValidate()));
            }
            if (Inst* next = block->get(instIndex)) {
                RegisterSetBuilder nextRegs = regs;
                next->forEach<Reg>(
                    [&] (Reg& reg, Arg::Role role, Bank, Width width) {
                        if (Arg::isEarlyDef(role))
                            nextRegs.add(reg, width);
                    });
                if (next->kind.opcode == Patch)
                    nextRegs.merge(next->extraEarlyClobberedRegs().buildAndValidate());
                if (!nextRegs.isEmpty())
                    m_clobbers.append(Clobber(indexOfHead + instIndex * 2, nextRegs.buildAndValidate()));
            }
        };

        record(block->size());
        for (unsigned instIndex = block->size(); instIndex--;) {
            localCalc.execute(instIndex);
            record(instIndex);
        }
    }

    void buildIntervals()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::buildIntervals"_s);
        UnifiedTmpLiveness liveness(m_code);

        TmpMap<Interval> openIntervals(m_code);

        auto closeInterval = [&](Tmp &tmp) {
            ASSERT(openIntervals[tmp] != Interval());
            m_map[tmp].liveRange.prepend(openIntervals[tmp]);
            openIntervals[tmp] = Interval();
        };

        BasicBlock* blockAfter = nullptr;
        for (size_t blockIndex = m_code.size(); blockIndex--;) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;

            size_t indexOfHead = this->indexOfHead(block);
            size_t indexOfTail = this->indexOfTail(block);
            if (verbose()) {
                dataLog("At block ", pointerDump(block), "\n");
                dataLog("  indexOfHead = ", indexOfHead, "\n");
                dataLog("  indexOfTail = ", indexOfTail, "\n");
            }

            // At tail, for every dead tmp, close interval
            for (Tmp tmp : liveness.liveAtTail(block)) {
                if (!tmp.isReg())
                    // FIXME: could just set interval start
                    openIntervals[tmp] |= Interval(indexOfTail);
            }
            if (blockAfter) {
                // If it was live at the head of the next block but no longer live, close
                // the current interval.
                for (Tmp tmp : liveness.liveAtHead(blockAfter)) {
                    if (!tmp.isReg() && !openIntervals[tmp].contains(indexOfTail)) {
                        ASSERT(openIntervals[tmp].begin() == this->indexOfHead(blockAfter));
                        closeInterval(tmp);
                    }
                }
            }
            
            for (unsigned instIndex = block->size(); instIndex--;) {
                Inst& inst = block->at(instIndex);
                size_t indexOfEarly = indexOfHead + instIndex * 2;

                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (tmp.isReg())
                        return;
                    auto& interval = openIntervals[tmp];
                    if (Arg::isLateUse(role))
                        interval |= lateInterval(indexOfEarly);
                    if (Arg::isLateDef(role)) {
                        interval |= lateInterval(indexOfEarly);
                        closeInterval(tmp);
                    }
                    if (Arg::isEarlyUse(role))
                        interval |= earlyInterval(indexOfEarly);
                    if (Arg::isEarlyDef(role)) {
                        interval |= earlyInterval(indexOfEarly);
                        closeInterval(tmp);
                    }
                });
            }
            for (Tmp tmp : liveness.liveAtHead(block)) {
                if (!tmp.isReg())
                    openIntervals[tmp] |= Interval(indexOfHead);
            }

            buildClobbers(liveness, block);
            blockAfter = block;
        }
        if (blockAfter) {
            for (Tmp tmp : liveness.liveAtHead(blockAfter)) {
                if (!tmp.isReg()) {
                    ASSERT(openIntervals[tmp].begin() == this->indexOfHead(blockAfter));
                    closeInterval(tmp);
                }
            }
        }
        // ASSERT every interval is closed.


        std::sort(
            m_clobbers.begin(), m_clobbers.end(),
            [] (Clobber& a, Clobber& b) -> bool {
                return a.index < b.index;
            });

        if (verbose()) {
            dataLog("Intervals:\n");
            m_code.forEachTmp(
                [&] (Tmp tmp) {
                    dataLog("    ", tmp, ": ", m_map[tmp], "\n");
                });
            dataLog("Clobbers: ", listDump(m_clobbers), "\n");
        }
    }

    bool shouldSpillEverything()
    {
        if (!Options::airLinearScanSpillsEverything())
            return false;

        // You're meant to hack this so that you selectively spill everything depending on reasons.
        // That's super useful for debugging.

        return true;
    }

    void spillEverything()
    {
        m_code.forEachTmp(
            [&] (Tmp tmp) {
                spill(tmp);
            });
    }

    void prepareIntervalsForScanForRegisters()
    {
        prepareIntervals(
            [&] (TmpData& data) -> bool {
                if (data.spilled)
                    return false;

                data.assigned = Reg();
                return true;
            });
    }

    void prepareIntervalsForScanForStack()
    {
        prepareIntervals(
            [&] (TmpData& data) -> bool {
                return data.spilled;
            });
    }

    template<typename SelectFunc>
    void prepareIntervals(const SelectFunc& selectFunc)
    {
        m_tmps.shrink(0);

        m_code.forEachTmp(
            [&] (Tmp tmp) {
                TmpData& data = m_map[tmp];
                if (!selectFunc(data))
                    return;

                m_tmps.append(tmp);
            });

        std::sort(
            m_tmps.begin(), m_tmps.end(),
            [&] (Tmp& a, Tmp& b) {
                return m_map[a].interval.begin() < m_map[b].interval.begin();
            });

        if (verbose())
            dataLog("Tmps: ", listDump(m_tmps), "\n");
    }

    Tmp addSpillTmpWithInterval(Bank bank, Interval interval)
    {
        TmpData data;
        data.interval = interval;
        data.isUnspillable = true;

        Tmp tmp = m_code.newTmp(bank);
        m_map.append(tmp, data);
        return tmp;
    }

    void attemptScanForRegisters(Bank bank)
    {
        // This is modeled after LinearScanRegisterAllocation in Fig. 1 in
        // http://dl.acm.org/citation.cfm?id=330250.

        m_active.clear();
        m_activeRegs = { };

        size_t clobberIndex = 0;
        for (Tmp& tmp : m_tmps) {
            if (tmp.bank() != bank)
                continue;

            TmpData& entry = m_map[tmp];
            size_t index = entry.interval.begin();

            if (verbose()) {
                dataLog("Index #", index, ": ", tmp, "\n");
                dataLog("  ", tmp, ": ", entry, "\n");
                dataLog("  clobberIndex = ", clobberIndex, "\n");
                // This could be so much faster.
                BasicBlock* block = m_code[0];
                for (BasicBlock* candidateBlock : m_code) {
                    if (m_startIndex[candidateBlock] > index)
                        break;
                    block = candidateBlock;
                }
                unsigned instIndex = (index - m_startIndex[block] + 1) / 2;
                dataLog("  At: ", pointerDump(block), ", instIndex = ", instIndex, "\n");
                dataLog("    Prev: ", pointerDump(block->get(instIndex - 1)), "\n");
                dataLog("    Next: ", pointerDump(block->get(instIndex)), "\n");
                dataLog("  Active:\n");
                for (Tmp tmp : m_active)
                    dataLog("    ", tmp, ": ", m_map[tmp], "\n");
            }

            // This is ExpireOldIntervals in Fig. 1.
            while (!m_active.isEmpty()) {
                Tmp tmp = m_active.first();
                TmpData& entry = m_map[tmp];

                bool expired = entry.interval.end() <= index;

                if (!expired)
                    break;

                m_active.removeFirst();
                m_activeRegs.remove(entry.assigned);
            }

            // If necessary, compute the set of registers that this tmp could even use. This is not
            // part of Fig. 1, but it's a technique that the authors claim to have implemented in one of
            // their two implementations. There may be other more efficient ways to do this, but this
            // implementation gets some perf wins from piggy-backing this calculation in the scan.
            //
            // Note that the didBuild flag sticks through spilling. Spilling doesn't change the
            // interference situation.
            //
            // Note that we could short-circuit this if we're dealing with a spillable tmp, there are no
            // free registers, and this register's interval ends after the one on the top of the active
            // stack.
            if (!entry.didBuildPossibleRegs) {
                // Advance the clobber index until it's at a clobber that is relevant to us.
                while (clobberIndex < m_clobbers.size() && m_clobbers[clobberIndex].index < index)
                    clobberIndex++;

                RegisterSetBuilder possibleRegs = m_allowedRegisters[bank].toRegisterSet();
                for (size_t i = clobberIndex; i < m_clobbers.size() && m_clobbers[i].index < entry.interval.end(); ++i)
                    possibleRegs.exclude(m_clobbers[i].regs.includeWholeRegisterWidth());

                entry.possibleRegs = possibleRegs.buildScalarRegisterSet();
                entry.didBuildPossibleRegs = true;
            }

            if (verbose())
                dataLog("  Possible regs: ", entry.possibleRegs, "\n");

            // Find a free register that we are allowed to use.
            if (m_active.size() != m_allowedRegistersInPriorityOrder[bank].size()) {
                bool didAssign = false;
                for (Reg reg : m_allowedRegistersInPriorityOrder[bank]) {
                    // FIXME: Could do priority coloring here.
                    // https://bugs.webkit.org/show_bug.cgi?id=170304
                    if (!m_activeRegs.contains(reg, IgnoreVectors) && entry.possibleRegs.contains(reg, IgnoreVectors)) {
                        assign(tmp, reg);
                        didAssign = true;
                        break;
                    }
                }
                if (didAssign)
                    continue;
            }

            // This is SpillAtInterval in Fig. 1, but modified to handle clobbers.
            Tmp spillTmp = m_active.takeLast(
                [&] (Tmp spillCandidate) -> bool {
                    return entry.possibleRegs.contains(m_map[spillCandidate].assigned, IgnoreVectors);
                });
            if (!spillTmp) {
                spill(tmp);
                continue;
            }
            TmpData& spillEntry = m_map[spillTmp];
            RELEASE_ASSERT(spillEntry.assigned);
            if (spillEntry.isUnspillable ||
                (!entry.isUnspillable && spillEntry.interval.end() <= entry.interval.end())) {
                spill(tmp);
                addToActive(spillTmp);
                continue;
            }

            assign(tmp, spillEntry.assigned);
            spill(spillTmp);
        }
    }

    void dumpRegRanges(Bank bank)
    {
        for (Reg r : m_allowedRegistersInPriorityOrder[bank])
            dataLogLn("   regRanges[", r, "]: ", m_regRanges[r]);
    }

    void setStageAndEnqueue(Tmp tmp, TmpData& tmpData, Stage stage)
    {
        ASSERT(!tmp.isReg());
        tmpData.stage = stage;
        m_queue.enqueue({ tmp, stage, tmpData.liveRange.size() });
    }

    template <Bank bank>
    void allocateRegisters()
    {
        m_code.forEachTmp(
            [&] (Tmp tmp) {
                if (tmp.bank() != bank)
                    return;
                setStageAndEnqueue(tmp, m_map[tmp], Stage::New);
        });

        // FIXME: could do this more directly rather than via m_clobbers.
        for (Clobber& clobber : m_clobbers) {
            for (Reg reg : clobber.regs)
                m_regRanges[reg].addClobber(reg, clobber.index);
        }

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
            if (tmpData.stage != Stage::Split && tryEvict<bank>(tmp, tmpData))
                continue;

            switch (tmpData.stage) {
            case Stage::New:
                // If we couldn't allocate tmp, allow it to split next time.
                setStageAndEnqueue(tmp, tmpData, Stage::Split);
                continue;
            case Stage::Split:
                if (!trySplit(tmp, tmpData))
                    setStageAndEnqueue(tmp, tmpData, Stage::Spill);
                continue;
            case Stage::Spill:
                spillTmp(tmp, tmpData);
                continue;
            case Stage::Unspillable:
                // Unspillables must have been allocated during tryAllocate or tryEvict.
                RELEASE_ASSERT_NOT_REACHED();
            case Stage::Assigned:
            case Stage::Spilled:
                // These terminal states should never have been enqueued.
                RELEASE_ASSERT_NOT_REACHED();
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    template <Bank bank>
    bool tryAllocate(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(&m_map[tmp] == &tmpData);
        LiveRange& liveRange = tmpData.liveRange;
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            auto& regRanges = m_regRanges[r];
            if (!regRanges.hasConflict(liveRange)) {
                assign(tmp, tmpData, r);
                return true;
            }
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
            auto& regRanges = m_regRanges[r];
            // TODO: maybe use IndexSparseSet instead and hoist construction (but that takes wrong type).
            // Or add a way to reset IndexSet and hoist construction.
            IndexSet<Tmp::Indexed<bank>> visited;
            regRanges.forEachConflict(liveRange, [&] (Tmp conflict) -> IterationStatus {
                if (conflict.isReg()) {
                    // Conflicts with a register clobber, cannot evict clobbers.
                    conflictsSpillCost = unspillableCost;
                    return IterationStatus::Done;
                }
                if (visited.contains(conflict))
                    return IterationStatus::Continue;
                visited.add(conflict);
                auto cost = m_map[conflict].spillCost;
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
            RELEASE_ASSERT(tmpData.spillCost != unspillableCost);
            return false;
        }
        // It's cheaper to spill all the already-assigned conflicting tmps, so evict them in favor of assigning 'tmp'.
        m_regRanges[bestEvictReg].forEachConflict(liveRange, [&] (Tmp conflict) -> IterationStatus {
            TmpData& conflictData = m_map[conflict];
            evict(conflict, conflictData, bestEvictReg);
            setStageAndEnqueue(conflict, conflictData, Stage::New);
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
    }

    void evict(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(tmpData.stage == Stage::Assigned);
        ASSERT(tmpData.assigned == reg);
        m_regRanges[reg].evict(tmp, tmpData.liveRange);
        tmpData.assigned = Reg();
        dataLogLnIf(verbose(), "Evicted ", tmp, " from ", reg);
    }

    bool trySplit(Tmp, TmpData)
    {
        return false;
    }

    void spillTmp(Tmp tmp, TmpData)
    {
        if (tmp) RELEASE_ASSERT_NOT_REACHED();
    }

    void addToActive(Tmp tmp)
    {
        if (m_map[tmp].isUnspillable) {
            m_active.prepend(tmp);
            return;
        }

        m_active.appendAndBubble(
            tmp,
            [&] (Tmp otherTmp) -> bool {
                TmpData& otherEntry = m_map[otherTmp];
                if (otherEntry.isUnspillable)
                    return false;
                return m_map[otherTmp].interval.end() > m_map[tmp].interval.end();
            });
    }

    NO_RETURN void assign(Tmp tmp, Reg reg)
    {
        TmpData& entry = m_map[tmp];
        RELEASE_ASSERT(!entry.spilled);
        ASSERT(false);
        entry.assigned = reg;
        m_activeRegs.add(reg, IgnoreVectors);
        addToActive(tmp);
    }

    NO_RETURN void spill(Tmp tmp)
    {
        TmpData& entry = m_map[tmp];
        RELEASE_ASSERT(!entry.isUnspillable);
        ASSERT(false);
        entry.spilled = m_code.addStackSlot(conservativeRegisterBytesWithoutVectors(tmp.bank()), StackSlotKind::Spill);
        entry.assigned = Reg();
        m_didSpill = true;
    }

    void emitSpillCode()
    {
        for (BasicBlock* block : m_code) {
            size_t indexOfHead = this->indexOfHead(block);
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                unsigned indexOfEarly = indexOfHead + instIndex * 2;

                // First try to spill directly.
                for (unsigned i = 0; i < inst.args.size(); ++i) {
                    Arg& arg = inst.args[i];
                    if (!arg.isTmp())
                        continue;
                    if (arg.isReg())
                        continue;
                    StackSlot* spilled = m_map[arg.tmp()].spilled;
                    if (!spilled)
                        continue;
                    if (!inst.admitsStack(i))
                        continue;
                    arg = Arg::stack(spilled);
                }

                // Fall back on the hard way.
                inst.forEachTmp(
                    [&] (Tmp& tmp, Arg::Role role, Bank bank, Width) {
                        if (tmp.isReg())
                            return;
                        StackSlot* spilled = m_map[tmp].spilled;
                        if (!spilled)
                            return;
                        Opcode move = bank == GP ? Move : MoveDouble;
                        tmp = addSpillTmpWithInterval(bank, intervalForSpill(indexOfEarly, role));
                        if (role == Arg::Scratch)
                            return;
                        if (Arg::isAnyUse(role))
                            m_insertionSets[block].insert(instIndex, secondPhase, move, inst.origin, Arg::stack(spilled), tmp);
                        if (Arg::isAnyDef(role))
                            m_insertionSets[block].insert(instIndex + 1, firstPhase, move, inst.origin, tmp, Arg::stack(spilled));
                    });
            }
        }
    }

    void scanForStack()
    {
        // This is loosely modeled after LinearScanRegisterAllocation in Fig. 1 in
        // http://dl.acm.org/citation.cfm?id=330250.

        m_active.clear();
        m_usedSpillSlots.clearAll();

        for (Tmp& tmp : m_tmps) {
            TmpData& entry = m_map[tmp];
            if (!entry.spilled)
                continue;

            size_t index = entry.interval.begin();

            // This is ExpireOldIntervals in Fig. 1.
            while (!m_active.isEmpty()) {
                Tmp tmp = m_active.first();
                TmpData& entry = m_map[tmp];

                bool expired = entry.interval.end() <= index;

                if (!expired)
                    break;

                m_active.removeFirst();
                m_usedSpillSlots.clear(entry.spillIndex);
            }

            entry.spillIndex = m_usedSpillSlots.findBit(0, false);
            size_t slotSize = conservativeRegisterBytesWithoutVectors(FP);
            ASSERT(entry.spilled->byteSize() <= slotSize);
            ptrdiff_t offset = -static_cast<ptrdiff_t>(m_code.frameSize()) - static_cast<ptrdiff_t>(entry.spillIndex) * slotSize - slotSize;
            if (verbose())
                dataLog("  Assigning offset = ", offset, " to spill ", pointerDump(entry.spilled), " for ", tmp, "\n");
            entry.spilled->setOffsetFromFP(offset);
            m_usedSpillSlots.set(entry.spillIndex);
            m_active.append(tmp);
        }
    }

    void insertSpillCode()
    {
        for (BasicBlock* block : m_code)
            m_insertionSets[block].execute(block);
    }

    void assignRegisters()
    {
        if (verbose()) {
            dataLog("About to allocate registers. State of all tmps:\n");
            m_code.forEachTmp(
                [&] (Tmp tmp) {
                    dataLog("    ", tmp, ": ", m_map[tmp], "\n");
                });
            dataLog("IR:\n");
            dataLog(m_code);
        }

        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                if (verbose())
                    dataLog("At: ", inst, "\n");
                inst.forEachTmpFast(
                    [&] (Tmp& tmp) {
                        if (tmp.isReg())
                            return;

                        Reg reg = m_map[tmp].assigned;
                        if (!reg) {
                            dataLog("Failed to allocate reg for: ", tmp, "\n");
                            RELEASE_ASSERT_NOT_REACHED();
                        }
                        tmp = Tmp(reg);
                    });
            }
        }
    }

    Code& m_code;
    Vector<Reg> m_allowedRegistersInPriorityOrder[numBanks];
    ScalarRegisterSet m_allowedRegisters[numBanks];
    ScalarRegisterSet m_allAllowedRegisters;
    IndexMap<BasicBlock*, size_t> m_startIndex;
    TmpMap<TmpData> m_map;
    IndexMap<Reg, RegisterRanges> m_regRanges;
    PriorityQueue<QueueElement, QueueElement::isHigherPriority> m_queue;
    IndexMap<BasicBlock*, PhaseInsertionSet> m_insertionSets;
    Vector<Clobber> m_clobbers; // After we allocate this, we happily point pointers into it.
    Vector<Tmp> m_tmps;
    Deque<Tmp> m_active;
    RegisterSet m_activeRegs;
    BitVector m_usedSpillSlots;
    bool m_didSpill { false };
};

} // anonymous namespace

void allocateRegistersAndStackByGreedy(Code& code)
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
