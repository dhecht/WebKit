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

#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/ProcessID.h>
#include <wtf/RawPointer.h>

namespace JSC {

#define FOR_EACH_DFG_COMPILE_STAT(macro) \
    macro(operationTriggerReoptimizationNow)                 \
    macro(operationTriggerReoptimizationNowJettison)         \
    macro(operationTriggerTierUpNow)                         \
    macro(triggerFTLReplacementCompile)                      \
    macro(operationTriggerTierUpNowInLoop)                   \
    macro(operationTriggerOSREntryNow)                       \
    macro(tierUpCommonCompile)                               \
    macro(baselineCompiles)                                  \
    macro(dfgCompiles)                                       \
    macro(unlikedDfgCompiles)                                \
    macro(ftlCompiles)                                       \

struct CompileStats {

    static CompileStats& ensure()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            atexit([]() {
                dataLogLn(WTF::getCurrentProcessID(), ": CompileStats: ", RawPointer(globalStats), pointerDump(globalStats));
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
        FOR_EACH_DFG_COMPILE_STAT(STAT_PRINT)
#undef STAT_PRINT
    }

#define STAT_DEF(name) unsigned name { 0 };
    FOR_EACH_DFG_COMPILE_STAT(STAT_DEF)
#undef STAT_DEF

    static CompileStats* globalStats;
};

}

#endif
