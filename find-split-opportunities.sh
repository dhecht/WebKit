#!/bin/bash

# Helper script to find intra-block split opportunities in JetStream3
# This will run JSC with the new airAnalyzeIntraBlockSplitOpportunities flag
# and extract examples of spilled temporaries that could benefit from intra-block splitting

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/WebKitBuild/Release}"
JSC="$BUILD_DIR/bin/jsc"

if [ ! -f "$JSC" ]; then
    echo "Error: jsc not found at $JSC"
    echo "Please build WebKit first or set BUILD_DIR environment variable"
    exit 1
fi

# Create output directory
OUTPUT_DIR="$SCRIPT_DIR/split-analysis"
mkdir -p "$OUTPUT_DIR"

echo "=== Finding Intra-Block Split Opportunities ==="
echo "JSC: $JSC"
echo "Output: $OUTPUT_DIR"
echo ""

# Simple test case that's likely to have register pressure
TEST_FILE="$OUTPUT_DIR/test.js"
cat > "$TEST_FILE" << 'EOF'
// Simple test with register pressure
function complexComputation(a, b, c, d, e, f, g, h) {
    var t1 = a + b;
    var t2 = c + d;
    var t3 = e + f;
    var t4 = g + h;
    var t5 = t1 * t2;
    var t6 = t3 * t4;
    var t7 = t5 + t6;

    // Force some spills with more computation
    var t8 = t1 + t2 + t3 + t4;
    var t9 = t5 + t6 + t7 + t8;
    var t10 = t8 * t9;

    // Reuse earlier values to create long live ranges
    var result = t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + t10;
    return result;
}

// Run it many times to trigger JIT compilation
for (var i = 0; i < 10000; i++) {
    complexComputation(1, 2, 3, 4, 5, 6, 7, 8);
}
EOF

echo "Running test case with split opportunity analysis..."
"$JSC" --airAnalyzeIntraBlockSplitOpportunities=true "$TEST_FILE" 2>&1 | tee "$OUTPUT_DIR/full-output.txt"

# Extract just the opportunities
grep -A 100 "INTRA-BLOCK SPLIT OPPORTUNITY" "$OUTPUT_DIR/full-output.txt" > "$OUTPUT_DIR/opportunities.txt" || true

if [ -s "$OUTPUT_DIR/opportunities.txt" ]; then
    echo ""
    echo "=== Summary ==="
    OPPORTUNITY_COUNT=$(grep -c "INTRA-BLOCK SPLIT OPPORTUNITY" "$OUTPUT_DIR/opportunities.txt" || echo "0")
    echo "Found $OPPORTUNITY_COUNT split opportunities"
    echo ""
    echo "Details saved to:"
    echo "  Full output: $OUTPUT_DIR/full-output.txt"
    echo "  Opportunities: $OUTPUT_DIR/opportunities.txt"
else
    echo ""
    echo "No split opportunities found in this simple test case."
    echo ""
fi

echo ""
echo "=== To analyze JetStream3 benchmarks ==="
echo ""
echo "Run on a specific benchmark:"
echo "  $JSC --airAnalyzeIntraBlockSplitOpportunities=true path/to/benchmark.js 2>&1 | tee output.txt"
echo ""
echo "Look for patterns like:"
echo "  === INTRA-BLOCK SPLIT OPPORTUNITY ==="
echo "  Spilled: tmp42 with 3 use clusters (total benefit: 2450.0)"
echo "    BB#5 [inst 10-15] 4 uses, density=0.67, freq=100"
echo "    BB#8 [inst 2-5] 3 uses, density=0.75, freq=100"
echo ""
