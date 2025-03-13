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

#pragma once

#if ENABLE(JIT)

#include "JITCompilationMode.h"

#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ProcessID.h>
#include <wtf/RawPointer.h>

namespace JSC {

#define FOR_EACH_COMPILE_STAT(macro) \
    macro(operationTriggerReoptimizationNow)                 \
    macro(operationTriggerReoptimizationNowJettison)         \
    macro(operationTriggerTierUpNow)                         \
    macro(triggerFTLReplacementCompile)                      \
    macro(operationTriggerTierUpNowInLoop)                   \
    macro(operationTriggerOSREntryNow)                       \
    macro(tierUpCommonCompile)                               \

#define FOR_EACH_PER_TIER_COMPILE_STAT(macro) \
    macro(compile) \
    macro(failedFinalizer) \
    macro(invalidatedCodeblock) \
    macro(failedFinalize) \
    macro(invalidatedReallyAdd) \
    macro(invalidatedIsJettisoned) \
    macro(canceledPlanInQueue) \
    macro(canceledPlanWhileCompiling) \

#define FOR_EACH_PER_TIER_COMPILE_DURATION_AGG(macro) \
    macro(queuedTime) \
    macro(compileTime) \
    macro(readyTime) \

struct CompileStats{
    using Counter = unsigned;
    static constexpr size_t numModes = 6;

    class Mark;
    class DurationAggregate {
    public:

        void dump(PrintStream& out) const 
        {
            out.println("count: ", m_count);
            out.println("avg: ", (m_total / m_count).milliseconds(), " ms");
            out.println("min: ", m_min.milliseconds(), " ms");
            out.println("max: ", m_max.milliseconds(), " ms");
        }
    
    private:
        friend class Mark;

        void aggregate(MonotonicTime start, MonotonicTime end)
        {
            ASSERT(start && end);
            Seconds duration = end - start;
            m_total += duration;
            m_count++;
            m_max = std::max(m_max, duration);
            m_min = std::min(m_min, duration);
        }

        Counter m_count { 0 };
        Seconds m_total { 0 };
        Seconds m_max { 0 };
        Seconds m_min { Seconds::infinity() };
    };

    class Mark {
    public:
        void start()
        {
            ASSERT(!m_start);
            m_start = MonotonicTime::now();
        }

        void stop(DurationAggregate& agg)
        {
            agg.aggregate(m_start, MonotonicTime::now());
        }

    private:
        MonotonicTime m_start;
    };

    static CompileStats& ensure()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            atexit([]() {
                dataLogLn(WTF::getCurrentProcessID(), ": CompileStats: ", RawPointer(globalStats), pointerDump(globalStats));
                WTF::dataFile().flush();
            });
            auto* stats = new CompileStats;
            WTF::storeStoreFence();
            globalStats = stats;
            dataLogLn(WTF::getCurrentProcessID(), ": CompileStats::ensure(): ", RawPointer(globalStats));
        });
        return *globalStats;
    }

    void dump(PrintStream& out) const
    {
#define STAT_PRINT(name) out.print("\n   " #name ": ", name);
        FOR_EACH_COMPILE_STAT(STAT_PRINT)
#undef STAT_PRINT

        for (size_t m = 0; m < numModes; m++) {
            JITCompilationMode mode = static_cast<JITCompilationMode>(m);
            if (mode == JITCompilationMode::InvalidCompilation)
                continue;
            out.print("\n\n   ", mode, ":", perMode(mode));
        }
    }

#define STAT_DEF(name) Counter name { 0 };
    FOR_EACH_COMPILE_STAT(STAT_DEF)
#undef STAT_DEF

    struct PerModeStats {

        void dump(PrintStream& out) const
        {
#define STAT_PRINT(name) out.print("\n     " #name ": ", name);
            FOR_EACH_PER_TIER_COMPILE_STAT(STAT_PRINT)
            FOR_EACH_PER_TIER_COMPILE_DURATION_AGG(STAT_PRINT)
#undef STAT_DEF
        }

#define STAT_DEF(name) Counter name { 0 };
        FOR_EACH_PER_TIER_COMPILE_STAT(STAT_DEF)
#undef STAT_DEF

#define STAT_DEF(name) DurationAggregate name { };
        FOR_EACH_PER_TIER_COMPILE_DURATION_AGG(STAT_DEF)
#undef STAT_DEF
    };

    static PerModeStats& perMode(JITCompilationMode mode)
    {
        return ensure().perModeStats[static_cast<size_t>(mode)];
    }

    std::array<PerModeStats, numModes> perModeStats;

    static CompileStats* globalStats;
};

}

#endif
