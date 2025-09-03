//@ requireOptions("--useWasmSIMD=1")
//@ skip if !$isSIMDPlatform
import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    ;; Test i8x16.extract_lane_s (signed byte extraction) - individual functions for each lane
    (func (export "test_i8x16_extract_lane_s_0") (result i32)
        (i8x16.extract_lane_s 0 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_s_1") (result i32)
        (i8x16.extract_lane_s 1 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_s_2") (result i32)
        (i8x16.extract_lane_s 2 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_s_3") (result i32)
        (i8x16.extract_lane_s 3 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_s_4") (result i32)
        (i8x16.extract_lane_s 4 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_s_5") (result i32)
        (i8x16.extract_lane_s 5 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )

    ;; Test i8x16.extract_lane_u (unsigned byte extraction) - key test cases
    (func (export "test_i8x16_extract_lane_u_0") (result i32)
        (i8x16.extract_lane_u 0 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_u_3") (result i32)
        (i8x16.extract_lane_u 3 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_u_4") (result i32)
        (i8x16.extract_lane_u 4 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )
    (func (export "test_i8x16_extract_lane_u_5") (result i32)
        (i8x16.extract_lane_u 5 (v128.const i8x16 0x00 0x01 0x7F 0x80 0xFF 0x81 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B))
    )

    ;; Test i32x4.extract_lane
    (func (export "test_i32x4_extract_lane_0") (result i32)
        (i32x4.extract_lane 0 (v128.const i32x4 0x12345678 0x9ABCDEF0 0x11111111 0x22222222))
    )
    (func (export "test_i32x4_extract_lane_1") (result i32)
        (i32x4.extract_lane 1 (v128.const i32x4 0x12345678 0x9ABCDEF0 0x11111111 0x22222222))
    )

    ;; Test v128.const basic functionality
    (func (export "test_v128_const") (result i32)
        (i8x16.extract_lane_u 0 (v128.const i8x16 0x42 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00))
    )
)
`;

async function test() {
    const instance = await instantiate(wat, {}, { simd: true });
    const { 
        test_i8x16_extract_lane_s_0, test_i8x16_extract_lane_s_1, test_i8x16_extract_lane_s_2, 
        test_i8x16_extract_lane_s_3, test_i8x16_extract_lane_s_4, test_i8x16_extract_lane_s_5,
        test_i8x16_extract_lane_u_0, test_i8x16_extract_lane_u_3, test_i8x16_extract_lane_u_4, test_i8x16_extract_lane_u_5,
        test_i32x4_extract_lane_0, test_i32x4_extract_lane_1, test_v128_const 
    } = instance.exports;

    // Test i8x16.extract_lane_s (signed)
    // Expected values: 0x00=0, 0x01=1, 0x7F=127, 0x80=-128, 0xFF=-1, 0x81=-127, etc.
    assert.eq(test_i8x16_extract_lane_s_0(), 0);      // 0x00 -> 0
    assert.eq(test_i8x16_extract_lane_s_1(), 1);      // 0x01 -> 1  
    assert.eq(test_i8x16_extract_lane_s_2(), 127);    // 0x7F -> 127
    assert.eq(test_i8x16_extract_lane_s_3(), -128);   // 0x80 -> -128 (sign extended)
    assert.eq(test_i8x16_extract_lane_s_4(), -1);     // 0xFF -> -1 (sign extended)
    assert.eq(test_i8x16_extract_lane_s_5(), -127);   // 0x81 -> -127 (sign extended)

    // Test i8x16.extract_lane_u (unsigned)
    // Expected values: all bytes interpreted as unsigned 0-255
    assert.eq(test_i8x16_extract_lane_u_0(), 0);      // 0x00 -> 0
    assert.eq(test_i8x16_extract_lane_u_3(), 128);    // 0x80 -> 128 (unsigned)
    assert.eq(test_i8x16_extract_lane_u_4(), 255);    // 0xFF -> 255 (unsigned)
    assert.eq(test_i8x16_extract_lane_u_5(), 129);    // 0x81 -> 129 (unsigned)

    // Test i32x4.extract_lane
    assert.eq(test_i32x4_extract_lane_0(), 0x12345678);
    assert.eq(test_i32x4_extract_lane_1(), 0x9ABCDEF0 | 0); // Force to signed 32-bit

    // Test v128.const
    assert.eq(test_v128_const(), 0x42);

    print("All SIMD extract lane tests passed!");
}

await assert.asyncTest(test())
