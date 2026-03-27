//@ runDefault
// Test that verifies correctness of loop-aware live range splitting in the
// greedy register allocator. The function has many variables live across a
// loop (defined before, used after), while the loop body creates register
// pressure through a function call that clobbers registers. Loop splitting
// should split the across-loop variables at the loop boundary, freeing
// registers for the loop body.

function clobber(x) { return x | 0; }
noInline(clobber);

function test(p) {
    // These variables are live across the loop but not used inside it.
    // They are candidates for loop splitting: the in-loop portion has no
    // uses (spillCost == 0) and should be spilled to free registers.
    let v0  = p + 0;
    let v1  = p + 1;
    let v2  = p + 2;
    let v3  = p + 3;
    let v4  = p + 4;
    let v5  = p + 5;
    let v6  = p + 6;
    let v7  = p + 7;
    let v8  = p + 8;
    let v9  = p + 9;
    let v10 = p + 10;
    let v11 = p + 11;
    let v12 = p + 12;
    let v13 = p + 13;
    let v14 = p + 14;
    let v15 = p + 15;

    // Loop with a function call that clobbers registers, creating pressure.
    let sum = 0;
    for (let i = 0; i < 100; i++)
        sum = clobber(sum + i);

    // Use all across-loop variables after the loop.
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 +
           v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + sum;
}
noInline(test);

// Expected: v0..v15 = p+0..p+15, sum = (0+1+...+99) = 4950
// With p=1: v-sum = (1+2+...+16) = 136, total = 136 + 4950 = 5086
let expected = 5086;
for (let i = 0; i < testLoopCount; i++) {
    let result = test(1);
    if (result !== expected)
        throw "FAIL: expected " + expected + " but got " + result;
}
