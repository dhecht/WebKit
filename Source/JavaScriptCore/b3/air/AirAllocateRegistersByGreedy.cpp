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
        if (m_intervals.isEmpty() || interval.end() < m_intervals.last().begin())
            m_intervals.prepend(WTFMove(interval));
        else {
            ASSERT(interval.end() == m_intervals.last().begin());
            m_intervals.last() |= interval;
        }
        m_size += interval.distance();
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
        out.print(" }[", m_size, "]");
    }

private:
    Deque<Interval> m_intervals;
    size_t m_size { 0 };
};

enum class Stage {
    New,
    Split,
    Unspillable,
    Spill,
    Assigned,
    Spilled,
    WasSplit,
};

struct QueueElement {
    QueueElement(Tmp tmp, Stage stage, bool maybeCoalescable, size_t rangeSize)
        : tmp(tmp)
        , stage(stage)
        , maybeCoalescable(maybeCoalescable)
        , rangeSize(rangeSize)
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

        if (left.rangeSize > right.rangeSize)
            return true;
        if (left.rangeSize < right.rangeSize)
            return false;

        // Guarantee a strict total ordering for determinism.
        return left.tmp.tmpIndex() < right.tmp.tmpIndex();
    }

    Tmp tmp;
    Stage stage;
    bool maybeCoalescable;
    size_t rangeSize;
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
            ASSERT_UNUSED(r, r == 1);
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
        auto rangeIter = range.intervals().begin();
        auto rangeEnd = range.intervals().end();

        if (rangeIter == rangeEnd)
            return;
        auto nextSearch = rangeIter->begin();

        while (true) {
            Tmp conflict;
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
                conflict = conflictIter->tmp;
                nextSearch = conflictIter->interval.end();
            }
            // 'func' can invalidate iterators of 'm_allocations'.
            if (func(conflict) == IterationStatus::Done)
                return;
        }
    }

    LiveRange toMergedLiveRange()
    {
        LiveRange merged;
        for (auto i = m_allocations.rbegin(); i != m_allocations.rend(); i++)
            merged.prepend(i->interval);
        return merged;
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
        out.print("(", other, ", ", weight, ")");
    }

    Tmp other;
    float weight;
};

struct TmpData {
    void dump(PrintStream& out) const
    {
        out.print("{liveRange = ", liveRange, ", preferredReg = ", preferredReg, ", affinity = ", listDump(affinity), ", members = ", listDump(members), ", groupLeader = ", groupLeader, " spillCost = ", spillCost, ", stage = ", stage, ", assigned = ", assigned, ", spilled = ", pointerDump(spilled), "}");
    }

    void validate()
    {
        RELEASE_ASSERT(!(spilled && assigned) && (!spilled || spillCost != unspillableCost));
    }

    LiveRange liveRange;
    float spillCost { 0.0f };
    Reg preferredReg;
    Vector<AffinityWith> affinity;
    Vector<Tmp> members;
    Tmp groupLeader;

    Stage stage { Stage::New };

    Reg assigned;
    StackSlot* spilled { nullptr };
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
    static constexpr bool eagerGroups = true;

    GreedyAllocator(Code& code)
        : m_code(code)
        , m_startIndex(code.size())
        , m_map(code)
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

        insertSpillCode();
        assignRegisters();
        fixSpillsAfterTerminals(m_code);
    }

private:
    void buildRegisterSets()
    {
        forEachBank(
            [&] (Bank bank) {
                m_allowedRegistersInPriorityOrder[bank] = m_code.regsInPriorityOrder(bank);
                for (Reg r : m_allowedRegistersInPriorityOrder[bank])
                    m_allAllowedRegisters.add(r, IgnoreVectors);
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

        TmpMap<Interval> activeIntervals(m_code);

        auto closeInterval = [&](Tmp &tmp) {
            ASSERT(activeIntervals[tmp] != Interval());
            m_map[tmp].liveRange.prepend(activeIntervals[tmp]);
            activeIntervals[tmp] = Interval();
        };

        // Find non-rare blocks.
        BlockWorklist fastWorklist;
        fastWorklist.push(m_code[0]);
        while (BasicBlock* block = fastWorklist.pop()) {
            for (FrequentedBlock& successor : block->successors()) {
                if (!successor.isRare())
                    fastWorklist.push(successor.block());
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
                dataLog("At block ", pointerDump(block), "\n");
                dataLog("  indexOfHead = ", indexOfHead, "\n");
                dataLog("  indexOfTail = ", indexOfTail, "\n");
            }

            // At tail, for every dead tmp, close interval
            for (Tmp tmp : liveness.liveAtTail(block)) {
                if (!tmp.isReg())
                    // FIXME: could just set interval start
                    activeIntervals[tmp] |= Interval(indexOfTail);
            }
            if (blockAfter) {
                // If it was live at the head of the next block but no longer live, close
                // the current interval.
                for (Tmp tmp : liveness.liveAtHead(blockAfter)) {
                    if (!tmp.isReg() && !activeIntervals[tmp].contains(indexOfTail)) {
                        ASSERT(activeIntervals[tmp].begin() == this->indexOfHead(blockAfter));
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
                    auto& interval = activeIntervals[tmp];
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
                    } else if (fastWorklist.saw(block)) {
                        ASSERT(inst.args[0].isTmp() && inst.args[1].isTmp());
                        addAffinity(inst.args[0].tmp(), inst.args[1].tmp(), block->frequency());
                        addAffinity(inst.args[1].tmp(), inst.args[0].tmp(), block->frequency());
                    } else
                        dataLogLnIf(verbose(), "Rare move: ", inst);
                }
            }
            for (Tmp tmp : liveness.liveAtHead(block)) {
                if (!tmp.isReg())
                    activeIntervals[tmp] |= Interval(indexOfHead);
            }

            buildClobbers(liveness, block);
            blockAfter = block;
        }
        if (blockAfter) {
            for (Tmp tmp : liveness.liveAtHead(blockAfter)) {
                if (!tmp.isReg()) {
                    ASSERT(activeIntervals[tmp].begin() == this->indexOfHead(blockAfter));
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

    void addAffinity(Tmp a, Tmp b, float freq)
    {
        auto& tmpData = m_map[a];
        for (auto& aff : tmpData.affinity) {
            if (aff.other == b) {
                aff.weight += freq;
                return;
            }
        }
        tmpData.affinity.append(AffinityWith{ b, freq });
    }

    template <Bank bank>
    void finalizeAffinity()
    {
        IndexSet<Tmp::Indexed<bank>> alreadyInGroup;
        m_code.forAllTmps([&](Tmp tmp) {
            if (tmp.bank() != bank)
                return;

            if (alreadyInGroup.contains(tmp))
                return;

            auto& affinity = m_map[tmp].affinity;
            if (affinity.isEmpty())
                return;

            std::sort(affinity.begin(), affinity.end(),
                [this] (AffinityWith& a, AffinityWith& b) -> bool {
                    if (a.weight > b.weight)
                        return true;
                    if (a.weight < b.weight)
                        return false;
                    // Favor coalescing shorter live ranges.
                    auto aSize = m_map[a.other].liveRange.size();
                    auto bSize = m_map[b.other].liveRange.size();
                    if (aSize < bSize)
                        return true;
                    if (aSize > bSize)
                        return false;
                    return a.other.tmpIndex() < b.other.tmpIndex();
            });

            if (!eagerGroups)
                return;

            RegisterRanges group;
            struct Elem {
                Tmp tmp;
                float weight;

                static bool isHigherPriority(const Elem& a, const Elem& b)
                {
                    return a.weight > b.weight;
                }
                void dump(PrintStream& out) const
                {
                    out.print(tmp, " (weight: ", weight, ")");
                }
            };
            PriorityQueue<Elem, Elem::isHigherPriority> worklist;
            Vector<Tmp> groupMembers;
            IndexSet<Tmp::Indexed<bank>> visited = alreadyInGroup;
            Reg groupPreferredReg;

            dataLogLnIf(verbose(), "Creating group rooted at ", tmp);
            worklist.enqueue({ tmp, 0 });
            visited.add(tmp);

            while (!worklist.isEmpty()) {
                auto candidate = worklist.dequeue();
                LiveRange& candidateRange = m_map[candidate.tmp].liveRange;

                if (!group.hasConflict(candidateRange)) {
                    dataLogLnIf(verbose(), "   Adding ", candidate, " to group");
                    group.add(candidate.tmp, candidateRange);
                    groupMembers.append(candidate.tmp);
                    alreadyInGroup.add(candidate.tmp);
                    if (!groupPreferredReg && m_map[candidate.tmp].preferredReg)
                        groupPreferredReg = m_map[candidate.tmp].preferredReg;
                    for (auto& other : m_map[candidate.tmp].affinity) {
                        if (!visited.contains(other.other)) {
                            visited.add(other.other);
                            worklist.enqueue({ other.other, other.weight });
                        }
                    }
                } else
                    dataLogLnIf(verbose(), "   Rejected ", candidate);
            }
            if (groupMembers.size() > 1) {
                Tmp groupTmp = m_code.newTmp(bank);
                TmpData groupData;
                groupData.liveRange = group.toMergedLiveRange();
                groupData.preferredReg = groupPreferredReg;
                groupData.members = WTFMove(groupMembers);
                Width defWidth, useWidth;
                defWidth = useWidth = widthForBytes(0);
                for (Tmp member : groupData.members) {
                    defWidth = std::max(defWidth, m_tmpWidth.defWidth(member));
                    useWidth = std::max(useWidth, m_tmpWidth.useWidth(member));
                    m_map[member].groupLeader = groupTmp;
                }
                m_tmpWidth.setWidths(groupTmp, useWidth, defWidth);
                m_map.append(groupTmp, groupData);
                dataLogLnIf(verbose(), "Created group ", groupTmp, ": ", m_map[groupTmp]);
            }
        });
    }

    template<Bank bank>
    void initSpillCosts()
    {
        m_code.forEachTmp([&](Tmp tmp) {
            if (tmp.bank() != bank)
                return;
            if (tmp.isReg())
                return;
            auto index = AbsoluteTmpMapper<bank>::absoluteIndex(tmp);
            float spillCost = m_useCounts.numWarmUsesAndDefs<bank>(index);
            if (bank == GP && m_useCounts.isConstDef<GP>(index))
                spillCost /= 2; // Can rematerialize rather than spill in many cases.
            m_map[tmp].spillCost = spillCost;
        });
        m_code.forEachFastTmp([&](Tmp tmp) {
            if (tmp.bank() != bank)
                return;
            m_map[tmp].spillCost = fastTmpSpillCost;
            dataLogLnIf(verbose(), "FastTmp: ", tmp);
        });
    }

    Tmp addSpillTmpWithInterval(Tmp spilledTmp, Interval interval)
    {
        TmpData data;
        data.liveRange.prepend(interval);
        data.spillCost = unspillableCost;

        Tmp tmp = m_code.newTmp(spilledTmp.bank());
        m_tmpWidth.setWidths(tmp, m_tmpWidth.useWidth(spilledTmp), m_tmpWidth.defWidth(spilledTmp));
        m_map.append(tmp, data);
        setStageAndEnqueue(tmp, data, Stage::Unspillable);

        dataLogLnIf(verbose(), "New spill tmp: ", tmp, ": ", data);
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
        ASSERT(stage != Stage::Assigned && stage != Stage::Spilled && stage != Stage::WasSplit);
        ASSERT(!tmpData.groupLeader); // Group member should not be enquened
        ASSERT(eagerGroups || !tmpData.members.size()); // Lazy groups -> not a group leader
        ASSERT(!tmpData.members.size() || !tmpData.affinity.size()); // Group leader -> no affinities
        tmpData.stage = stage;
        m_queue.enqueue({ tmp, stage, tmpData.preferredReg || tmpData.affinity.size(), tmpData.liveRange.size() });
        dataLogLnIf(verbose(), "Enqueued (", stage, ") ", tmp);
    }

    template <Bank bank>
    void allocateRegisters()
    {
        m_code.forEachTmp(
            [&] (Tmp tmp) {
                if (tmp.bank() != bank)
                    return;
                if (tmp.isReg())
                    return;
                TmpData& tmpData = m_map[tmp];
                if (tmpData.groupLeader) {
                    ASSERT(eagerGroups);
                    return;
                }
                setStageAndEnqueue(tmp, tmpData, Stage::New);
        });

        // FIXME: could do this more directly rather than via m_clobbers.
        for (Clobber& clobber : m_clobbers) {
            for (Reg reg : clobber.regs)
                m_regRanges[reg].addClobber(reg, clobber.index);
        }

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
                    ASSERT(queueContainsOnlySpills()); // XXX: remove
                    spill(tmp, tmpData);
                    continue;
                case Stage::Unspillable:
                    // Unspillables must have been allocated during tryAllocate or tryEvict.
                    RELEASE_ASSERT_NOT_REACHED();
                case Stage::Assigned:
                case Stage::Spilled:
                case Stage::WasSplit:
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
        for (auto& affinity : tmpData.affinity) {
            Reg r = m_map[affinity.other].assigned;
            if (r) {
                if (tryAllocateToReg(r))
                    return true;
                alreadyAttempted.add(r, IgnoreVectors);
            }
        }
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
            auto& regRanges = m_regRanges[r];
            // TODO: maybe use IndexSparseSet instead and hoist construction (but that takes wrong type).
            // Or add a way to reset IndexSet and hoist construction.
            IndexSet<Tmp::Indexed<bank>> visited;
            regRanges.forEachConflict(liveRange,
                [&] (Tmp conflict) -> IterationStatus {
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
            // If 'tmp' was unspillable, we better have found at least one suitable register.
            RELEASE_ASSERT(tmpData.spillCost != unspillableCost);
            return false;
        }
        // It's cheaper to spill all the already-assigned conflicting tmps, so evict them in favor of assigning 'tmp'.
        m_regRanges[bestEvictReg].forEachConflict(liveRange,
            [&] (Tmp conflict) -> IterationStatus {
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
        ASSERT(tmpData.spillCost != unspillableCost);
        ASSERT(tmpData.assigned == reg);
        m_regRanges[reg].evict(tmp, tmpData.liveRange);
        tmpData.assigned = Reg();
        dataLogLnIf(verbose(), "Evicted ", tmp, " from ", reg);
    }

    bool trySplit(Tmp tmp, TmpData& tmpData)
    {
        if (!tmpData.members.size())
            return false;
        for (Tmp member : tmpData.members) {
            TmpData& memberData = m_map[member];
            memberData.groupLeader = Tmp();
            setStageAndEnqueue(member, memberData, Stage::New);
        }
        tmpData.stage = Stage::WasSplit;
        dataLogLnIf(verbose(), "Split (group) ", tmp);
        return true;
    }

    // FIXME: dup from GraphColoring.cpp
    static unsigned stackSlotMinimumWidth(Width width)
    {
        if (width <= Width32)
            return 4;
        if (width <= Width64)
            return 8;
        ASSERT(width == Width128);
        return 16;
    }

    void spill(Tmp tmp, TmpData& data)
    {
        RELEASE_ASSERT(data.spillCost != unspillableCost);
        ASSERT(data.assigned == Reg());
        ASSERT(!data.members.size()); // Should have been split
        data.stage = Stage::Spilled;
        data.spilled = m_code.addStackSlot(stackSlotMinimumWidth(m_tmpWidth.requiredWidth(tmp)), StackSlotKind::Spill);
        dataLogLnIf(verbose(), "Spilled ", tmp, " to ", data.spilled);
        // Batch generating spill tmps so that we can limit traversals of the code without
        // needing to keep track of each tmp's use/defs.
        // FIXME: revisit if live range splitting needs that info anyway.
        // FIXME: this might not be the best if e.g. an unspillable forces a loop tmp to be spilled.
        m_didSpill = true;
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

    // FIXME: merge with linear scan emitSpillCode().
    template <Bank bank>
    void emitSpillCodeAndEnqueueNewTmps()
    {
        auto spilledLoc = [&](Tmp tmp) {
            TmpData& tmpData = m_map[tmp];
            StackSlot* spilled = tmpData.spilled;
            if (tmpData.groupLeader) {
                ASSERT(eagerGroups);
                ASSERT(!spilled);
                spilled = m_map[tmpData.groupLeader].spilled;
            }
            return spilled;
        };

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

                        StackSlot* spilled = spilledLoc(arg.tmp());
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
                    StackSlot* spilled = spilledLoc(tmp);
                    if (!spilled)
                        return;

                    Width spillWidth = m_tmpWidth.requiredWidth(tmp);
                    Opcode move = Oops;
                    switch (stackSlotMinimumWidth(spillWidth)) {
                    case 4:
                        move = bank == GP ? Move32 : MoveFloat;
                        break;
                    case 8:
                        move = bank == GP ? Move : MoveDouble;
                        break;
                    case 16:
                        ASSERT(bank == FP);
                        move = MoveVector;
                        break;
                    default:
                        RELEASE_ASSERT_NOT_REACHED();
                        break;
                    }
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
                                        m_insertionSets[block].insert(instIndex, secondPhase, Move, inst.origin, Arg::imm(value), tmp);
                                        dataLogLnIf(verbose(), "Rematerialized (imm) ", oldTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                        return true;
                                    }
                                    if (isValidForm(Move, Arg::BigImm, Arg::Tmp)) {
                                        m_insertionSets[block].insert(instIndex, secondPhase, Move, inst.origin, Arg::bigImm(value), tmp);
                                        dataLogLnIf(verbose(), "Rematerialized (bigImm) ", oldTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };

                        if (!tryRematerialize())
                            m_insertionSets[block].insert(instIndex, secondPhase, move, inst.origin, arg, tmp);
                    }
                    if (Arg::isAnyDef(role))
                        m_insertionSets[block].insert(instIndex + 1, firstPhase, move, inst.origin, tmp, arg);
                });
            }
        }
    }

    void insertSpillCode()
    {
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

                inst.forEachTmpFast(
                    [&] (Tmp& tmp) {
                        if (tmp.isReg())
                            return;

                        Reg reg = m_map[tmp].assigned;
                        TmpData& tmpData = m_map[tmp];
                        if (tmpData.groupLeader) {
                            ASSERT(eagerGroups);
                            ASSERT(!reg);
                            reg = m_map[tmpData.groupLeader].assigned;
                        }
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
    IndexMap<BasicBlock*, size_t> m_startIndex;
    TmpMap<TmpData> m_map;
    IndexMap<Reg, RegisterRanges> m_regRanges;
    PriorityQueue<QueueElement, QueueElement::isHigherPriority> m_queue;
    IndexMap<BasicBlock*, PhaseInsertionSet> m_insertionSets;
    Vector<Clobber> m_clobbers;
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
