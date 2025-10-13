//@ requireOptions("--useWasmSIMD=1")
//@ skip if !$isSIMDPlatform
import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    ;; Test i32x4.trunc_sat_f32x4_u with 2^31 (2147483648.0)
    ;; This is the critical boundary case
    (func (export "test_u_2pow31_lane0") (result i32)
        (v128.const f32x4 2147483648.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 0)
    )

    (func (export "test_u_2pow31_lane1") (result i32)
        (v128.const f32x4 0.0 2147483648.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 1)
    )

    (func (export "test_u_2pow31_lane2") (result i32)
        (v128.const f32x4 0.0 0.0 2147483648.0 0.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 2)
    )

    (func (export "test_u_2pow31_lane3") (result i32)
        (v128.const f32x4 0.0 0.0 0.0 2147483648.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 3)
    )

    ;; Test i32x4.trunc_sat_f32x4_s with 2^30 (1073741824.0)
    (func (export "test_s_2pow30") (result i32)
        (v128.const f32x4 1073741824.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_s with value that should saturate
    ;; 2147483648.0 (2^31) should saturate to 0x7fffffff (2147483647)
    (func (export "test_s_overflow") (result i32)
        (v128.const f32x4 2147483648.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_s with large value that should saturate
    ;; 3000000000.0 should saturate to 0x7fffffff (2147483647)
    (func (export "test_s_large_overflow") (result i32)
        (v128.const f32x4 3000000000.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_u with large value < u32 max
    ;; 4000000000.0 should convert to 4000000000 (0xee6b2800)
    (func (export "test_u_4billion") (result i32)
        (v128.const f32x4 4000000000.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_u with negative (should be 0)
    (func (export "test_u_negative") (result i32)
        (v128.const f32x4 -100.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_u with overflow (should saturate to max u32)
    ;; 5000000000.0 > u32 max, should saturate to 0xffffffff
    (func (export "test_u_overflow") (result i32)
        (v128.const f32x4 5000000000.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_s with negative
    (func (export "test_s_negative") (result i32)
        (v128.const f32x4 -1234.5 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 0)
    )

    ;; Test i32x4.trunc_sat_f32x4_s with min negative overflow
    ;; Should saturate to -2147483648 (0x80000000)
    (func (export "test_s_min_overflow") (result i32)
        (v128.const f32x4 -3000000000.0 0.0 0.0 0.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 0)
    )

    ;; Test all lanes with different values
    (func (export "test_u_mixed_lane0") (result i32)
        (v128.const f32x4 0.0 2147483648.0 4000000000.0 100.5)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 0)
    )

    (func (export "test_u_mixed_lane1") (result i32)
        (v128.const f32x4 0.0 2147483648.0 4000000000.0 100.5)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 1)
    )

    (func (export "test_u_mixed_lane2") (result i32)
        (v128.const f32x4 0.0 2147483648.0 4000000000.0 100.5)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 2)
    )

    (func (export "test_u_mixed_lane3") (result i32)
        (v128.const f32x4 0.0 2147483648.0 4000000000.0 100.5)
        (i32x4.trunc_sat_f32x4_u)
        (i32x4.extract_lane 3)
    )

    (func (export "test_s_mixed_lane0") (result i32)
        (v128.const f32x4 -100.5 0.0 1073741824.0 2147483520.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 0)
    )

    (func (export "test_s_mixed_lane1") (result i32)
        (v128.const f32x4 -100.5 0.0 1073741824.0 2147483520.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 1)
    )

    (func (export "test_s_mixed_lane2") (result i32)
        (v128.const f32x4 -100.5 0.0 1073741824.0 2147483520.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 2)
    )

    (func (export "test_s_mixed_lane3") (result i32)
        (v128.const f32x4 -100.5 0.0 1073741824.0 2147483648.0)
        (i32x4.trunc_sat_f32x4_s)
        (i32x4.extract_lane 3)
    )
)
`

async function test() {
    const instance = await instantiate(wat, {}, { simd: true })
    const {
        test_u_2pow31_lane0,
        test_u_2pow31_lane1,
        test_u_2pow31_lane2,
        test_u_2pow31_lane3,
        test_s_2pow30,
        test_s_overflow,
        test_s_large_overflow,
        test_u_4billion,
        test_u_negative,
        test_u_overflow,
        test_s_negative,
        test_s_min_overflow,
        test_u_mixed_lane0,
        test_u_mixed_lane1,
        test_u_mixed_lane2,
        test_u_mixed_lane3,
        test_s_mixed_lane0,
        test_s_mixed_lane1,
        test_s_mixed_lane2,
        test_s_mixed_lane3
    } = instance.exports

    for (let i = 0; i < wasmTestLoopCount; ++i) {
        // Critical test: 2^31 should convert to exactly 2147483648 (0x80000000)
        // When viewed as signed i32, this is -2147483648
        assert.eq(test_u_2pow31_lane0(), -2147483648, "2^31 lane 0");
        assert.eq(test_u_2pow31_lane1(), -2147483648, "2^31 lane 1");
        assert.eq(test_u_2pow31_lane2(), -2147483648, "2^31 lane 2");
        assert.eq(test_u_2pow31_lane3(), -2147483648, "2^31 lane 3");

        // Other unsigned tests
        assert.eq(test_u_4billion(), -294967296, "4 billion as i32 (0xee6b2800)");
        assert.eq(test_u_negative(), 0, "negative should saturate to 0");
        assert.eq(test_u_overflow(), -1, "overflow should saturate to 0xffffffff");

        // Signed tests
        assert.eq(test_s_2pow30(), 1073741824, "2^30 signed");
        assert.eq(test_s_overflow(), 2147483647, "2^31 should saturate to i32 max (0x7fffffff)");
        assert.eq(test_s_large_overflow(), 2147483647, "large overflow should saturate to i32 max");
        assert.eq(test_s_negative(), -1234, "negative truncates");
        assert.eq(test_s_min_overflow(), -2147483648, "min overflow saturates");

        // Mixed lane tests
        assert.eq(test_u_mixed_lane0(), 0, "mixed unsigned lane 0");
        assert.eq(test_u_mixed_lane1(), -2147483648, "mixed unsigned lane 1 (0x80000000)");
        assert.eq(test_u_mixed_lane2(), -294967296, "mixed unsigned lane 2");
        assert.eq(test_u_mixed_lane3(), 100, "mixed unsigned lane 3");

        assert.eq(test_s_mixed_lane0(), -100, "mixed signed lane 0");
        assert.eq(test_s_mixed_lane1(), 0, "mixed signed lane 1");
        assert.eq(test_s_mixed_lane2(), 1073741824, "mixed signed lane 2");
        assert.eq(test_s_mixed_lane3(), 2147483647, "mixed signed lane 3 (saturates)");
    }
}

await assert.asyncTest(test())
