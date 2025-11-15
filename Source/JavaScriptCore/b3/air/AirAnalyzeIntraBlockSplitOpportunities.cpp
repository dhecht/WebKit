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
#include "AirAnalyzeIntraBlockSplitOpportunities.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirTmpInlines.h"
#include <wtf/DataLog.h>

namespace JSC { namespace B3 { namespace Air {

struct BlockUseInfo {
    unsigned firstUseIndex { UINT_MAX };
    unsigned lastUseIndex { 0 };
    unsigned useCount { 0 };
    bool hasDef { false };

    void recordUse(unsigned index)
    {
        firstUseIndex = std::min(firstUseIndex, index);
        lastUseIndex = std::max(lastUseIndex, index);
        useCount++;
    }
};

void analyzeIntraBlockSplitOpportunities(Code& code)
{
    dataLogLn("=== Analyzing Intra-Block Split Opportunities ===");

    // For each tmp, track which blocks it's used in and how
    HashMap<Tmp, HashMap<BasicBlock*, BlockUseInfo>> tmpBlockUsage;
    HashMap<Tmp, unsigned> blockSpanCount; // How many blocks is tmp live across

    // Scan all blocks and instructions
    for (BasicBlock* block : code) {
        for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
            Inst& inst = block->at(instIndex);

            inst.forEachTmp([&](Tmp tmp, Arg::Role role, Bank, Width) {
                if (tmp.isReg())
                    return;

                auto& blockMap = tmpBlockUsage.add(tmp, HashMap<BasicBlock*, BlockUseInfo>()).iterator->value;
                auto& useInfo = blockMap.add(block, BlockUseInfo()).iterator->value;

                if (Arg::isAnyUse(role) || Arg::isAnyDef(role))
                    useInfo.recordUse(instIndex);
                if (Arg::isAnyDef(role))
                    useInfo.hasDef = true;
            });
        }
    }

    // Count block spans
    for (auto& entry : tmpBlockUsage)
        blockSpanCount.add(entry.key, entry.value.size());

    struct Opportunity {
        Tmp tmp;
        BasicBlock* block;
        float benefit;
        unsigned firstUse;
        unsigned lastUse;
        unsigned uses;
        unsigned totalBlocks;

        void dump(PrintStream& out) const
        {
            out.print(tmp, " in BB", *block, " [", firstUse, "-", lastUse, "] ",
                     uses, " uses, live in ", totalBlocks, " blocks, benefit=", benefit);
        }
    };

    Vector<Opportunity> opportunities;

    // Find opportunities: tmp live across many blocks but with dense use cluster in one block
    for (auto& tmpEntry : tmpBlockUsage) {
        Tmp tmp = tmpEntry.key;
        unsigned totalBlocks = tmpEntry.value.size();

        // Only interesting if tmp is live across multiple blocks
        if (totalBlocks < 2)
            continue;

        for (auto& blockEntry : tmpEntry.value) {
            BasicBlock* block = blockEntry.key;
            const BlockUseInfo& info = blockEntry.value;

            // Look for dense use clusters: at least 2 uses
            if (info.useCount < 2)
                continue;

            unsigned clusterSize = info.lastUseIndex - info.firstUseIndex + 1;

            // Skip if cluster is too sparse
            if (clusterSize < 2)
                continue;

            float useDensity = float(info.useCount) / clusterSize;
            float benefit = block->frequency() * info.useCount * useDensity * totalBlocks;

            Opportunity opp;
            opp.tmp = tmp;
            opp.block = block;
            opp.benefit = benefit;
            opp.firstUse = info.firstUseIndex;
            opp.lastUse = info.lastUseIndex;
            opp.uses = info.useCount;
            opp.totalBlocks = totalBlocks;

            opportunities.append(opp);
        }
    }

    // Sort by benefit
    std::sort(opportunities.begin(), opportunities.end(),
        [](const Opportunity& a, const Opportunity& b) {
            return a.benefit > b.benefit;
        });

    // Report top opportunities
    dataLogLn("\nTop ", std::min<size_t>(30, opportunities.size()), " intra-block split opportunities:");
    for (size_t i = 0; i < std::min<size_t>(30, opportunities.size()); ++i) {
        dataLog("  #", i + 1, ": ");
        opportunities[i].dump(dataLog());
        dataLogLn();
    }

    dataLogLn("\n=== Analysis Complete ===\n");
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
