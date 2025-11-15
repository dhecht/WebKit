# Intra-Block Split Opportunity Analysis Guide

## Step 1: Build and Run

```bash
# Build JSC
cd /Volumes/Data/src/ws2/OpenSource
./Tools/Scripts/build-jsc --release

# Run on JetStream3 benchmark with analysis
WebKitBuild/Release/bin/jsc \
    --airAnalyzeIntraBlockSplitOpportunities=true \
    --airDumpRegAllocStats=true \
    path/to/JetStream3/benchmark.js 2>&1 | tee split-analysis.log
```

## Step 2: Extract Top Opportunities

The output will show opportunities like this:

```
=== INTRA-BLOCK SPLIT OPPORTUNITY ===
Spilled: tmp234 with 4 use clusters (total benefit: 3850.5)
  BB#10 [inst 5-8] 3 uses, density=0.75, freq=1000
  BB#15 [inst 12-18] 6 uses, density=0.86, freq=1000
  BB#23 [inst 3-7] 4 uses, density=0.80, freq=500
  BB#45 [inst 1-3] 2 uses, density=0.67, freq=100
```

**Sort by benefit score** to find top 10:
```bash
grep -A 1 "INTRA-BLOCK SPLIT OPPORTUNITY" split-analysis.log | \
    grep "total benefit" | \
    sort -t: -k2 -rn | \
    head -10
```

## Step 3: Output Format

For each function with opportunities, you'll see:

### 3a. Opportunities Found
```
=== INTRA-BLOCK SPLIT OPPORTUNITY ===
Spilled: tmp234 with 4 use clusters (total benefit: 3850.5)
  BB#10 [inst 5-8] 3 uses, density=0.75, freq=1000
  BB#15 [inst 12-18] 6 uses, density=0.86, freq=1000
  ...
```

### 3b. Spill Slot Mapping
```
=== SPILL SLOT MAPPING FOR OPPORTUNITIES ===
  tmp234 -> stack42 (8 bytes)
  tmp156 -> stack28 (8 bytes)
```

### 3c. IR Before Register Assignment
```
=== AIR BEFORE REGISTER ASSIGNMENT (showing Tmps and StackSlots) ===
BB#10:
  Move stack42, %rax      <- Load from spill
  Add32 %rax, %rdx, %rax
  Mul32 %rax, %rcx, %rax
  ...
=== END IR ===
```

### 3d. IR After fixObviousSpills (Only for functions with opportunities)
```
=== AIR AFTER fixObviousSpills ===
BB#10:
  Move %rbx, %rax         <- Optimized! Used register from predecessor
  Add32 %rax, %rdx, %rax
  Mul32 %rax, %rcx, %rax
  ...
=== END IR (after fixObviousSpills) ===
```

**Note**: The "after fixObviousSpills" IR is only dumped for functions where opportunities were found, making the output focused and easy to analyze.

### 3e. Statistics Summary
```
Register allocator stats for GP bank:
   numTmpsIn: 450
   numSpilledTmps: 23
   numIntraBlockSplitOpportunities: 8
   totalIntraBlockSplitBenefit: 12450
```

## Step 4: Analysis Checklist

For each of the top 10 opportunities, check:

### ✅ **Fixed by fixObviousSpills** (No split needed)
Look for patterns where the load was optimized:

**Before fixObviousSpills:**
```
BB#10:
  Move stack42, %rax      <- Load from spill
```

**After fixObviousSpills:**
```
BB#10:
  Move %rbx, %rax         <- Changed to register-to-register!
```

**OR:**
```
BB#10:
  [instruction removed]   <- Load eliminated entirely!
```

### ❌ **NOT Fixed by fixObviousSpills** (Split might help)
The load instruction remains unchanged:

**Before fixObviousSpills:**
```
BB#10:
  Move stack42, %rax      <- Load from spill
```

**After fixObviousSpills:**
```
BB#10:
  Move stack42, %rax      <- STILL loading from stack!
```

## Step 5: Create Summary Table

Create a table like this for top 10 opportunities:

| Rank | Tmp | Benefit | Clusters | Fixed by fixObviousSpills? | Notes |
|------|-----|---------|----------|----------------------------|-------|
| 1 | tmp234 | 3850.5 | 4 | ✅ 3/4 blocks | BB#10,15,23 fixed, BB#45 not |
| 2 | tmp156 | 2940.0 | 3 | ❌ 0/3 blocks | All blocks still load from stack |
| 3 | tmp89 | 2100.0 | 2 | ✅ 2/2 blocks | All fixed, no split needed |
| ... | | | | | |

## Step 6: Identify True Opportunities

Count opportunities by fixObviousSpills effectiveness:

```bash
# Total opportunities
TOTAL=$(grep -c "INTRA-BLOCK SPLIT OPPORTUNITY" split-analysis.log)

# Opportunities that matter = those NOT fully fixed by fixObviousSpills
# (You'll need to manually count from your analysis)
```

## What to Look For

### **Good Split Candidates** (fixObviousSpills didn't help)
- Hot blocks (freq > 100)
- High use density (> 0.7)
- Multiple blocks still loading from stack after fixObviousSpills

### **Not Worth Splitting** (fixObviousSpills handled it)
- Loads optimized to register moves
- Stores eliminated
- Most uses already in registers

## Example Analysis

**Case 1: Split Helps**
```
tmp156 benefit=2940.0
  BB#15: freq=1000, 6 uses
  BB#23: freq=500, 4 uses

After fixObviousSpills:
  BB#15: STILL "Move stack28, %rax"  <- Not optimized!
  BB#23: STILL "Move stack28, %rcx"  <- Not optimized!

Conclusion: GOOD CANDIDATE for splitting
```

**Case 2: Already Fixed**
```
tmp234 benefit=3850.5
  BB#10: freq=1000, 3 uses

After fixObviousSpills:
  BB#10: "Move %rbx, %rax"  <- Optimized to reg-to-reg!

Conclusion: fixObviousSpills already handles this, split not needed
```

## Questions to Answer

1. **What % of opportunities are already handled by fixObviousSpills?**
   - If > 80%, splitting might not be worth it

2. **What's the total benefit of UNfixed opportunities?**
   - If low, splitting won't help much

3. **What patterns appear in UNfixed cases?**
   - Loop bodies?
   - Complex control flow?
   - Specific register pressure scenarios?

## Next Steps

Based on analysis:
- **If < 20% fixed by fixObviousSpills**: Proceed with splitting implementation
- **If > 80% fixed**: Consider tuning fixObviousSpills instead
- **If mixed**: Focus on specific patterns that aren't handled
