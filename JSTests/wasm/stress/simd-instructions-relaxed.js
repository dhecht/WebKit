//@ requireOptions("--useWasmSIMD=1", "--useWasmRelaxedSIMD=1")
//@ skip if !$isSIMDPlatform
import { runSIMDTests } from "./simd-instructions-lib.js"

const verbose = false;

// Table-driven test data for SIMD relaxed instructions
// Each entry: [instruction, input0, input1, input2, expected_output]
const relaxedTests = [
    // f32x4.relaxed_madd tests - a * b + c
    [
        "f32x4.relaxed_madd",
        [2.0, 3.0, 4.0, 5.0],
        [10.0, 20.0, 30.0, 40.0],
        [1.0, 2.0, 3.0, 4.0],
        [21.0, 62.0, 123.0, 204.0]  // [2*10+1, 3*20+2, 4*30+3, 5*40+4]
    ],
    [
        "f32x4.relaxed_madd",
        [5.0, 4.0, -37.0, 6.0],
        [22.0, 25.0, -3.0, 20.0],
        [-1.0, 1.0, 0.0, -1.0],
        [109.0, 101.0, 111.0, 119.0]  // [5*22-1, 4*25+1, -37*-3+0, 6*20-1]
    ],
    [
        "f32x4.relaxed_madd",
        [0.0, -0.0, 1.0, -1.0],
        [1.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 5.0, 5.0],
        [0.0, 0.0, 5.0, 5.0]  // [0*1+0, -0*1+0, 1*0+5, -1*0+5]
    ],

    // f32x4.relaxed_nmadd tests - -(a * b) + c
    [
        "f32x4.relaxed_nmadd",
        [2.0, 3.0, 4.0, 5.0],
        [10.0, 20.0, 30.0, 40.0],
        [1.0, 2.0, 3.0, 4.0],
        [-19.0, -58.0, -117.0, -196.0]  // [-(2*10)+1, -(3*20)+2, -(4*30)+3, -(5*40)+4]
    ],
    [
        "f32x4.relaxed_nmadd",
        [5.0, 4.0, -37.0, -6.0],
        [-22.0, -25.0, 3.0, 20.0],
        [-1.0, 1.0, 0.0, -1.0],
        [109.0, 101.0, 111.0, 119.0]  // [-(5*-22)-1, -(4*-25)+1, -(-37*3)+0, -(-6*20)-1]
    ],

    // f64x2.relaxed_madd tests - a * b + c
    [
        "f64x2.relaxed_madd",
        [2.0, 3.0],
        [10.0, 20.0],
        [1.0, 2.0],
        [21.0, 62.0]  // [2*10+1, 3*20+2]
    ],
    [
        "f64x2.relaxed_madd",
        [5.5, -3.25],
        [2.0, 4.0],
        [0.5, -1.0],
        [11.5, -14.0]  // [5.5*2+0.5, -3.25*4-1]
    ],
    [
        "f64x2.relaxed_madd",
        [0.0, -0.0],
        [1.0, 1.0],
        [0.0, 0.0],
        [0.0, 0.0]
    ],

    // f64x2.relaxed_nmadd tests - -(a * b) + c
    [
        "f64x2.relaxed_nmadd",
        [2.0, 3.0],
        [10.0, 20.0],
        [1.0, 2.0],
        [-19.0, -58.0]  // [-(2*10)+1, -(3*20)+2]
    ],
    [
        "f64x2.relaxed_nmadd",
        [5.5, -3.25],
        [2.0, 4.0],
        [0.5, -1.0],
        [-10.5, 12.0]  // [-(5.5*2)+0.5, -(-3.25*4)-1]
    ],

    // i8x16.relaxed_swizzle tests - swizzle with out-of-bounds behavior relaxed
    [
        "i8x16.relaxed_swizzle",
        [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F],
        [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F],
        [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F]
    ],
    [
        "i8x16.relaxed_swizzle",
        [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F],
        [0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00],
        [0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00]
    ],
    [
        "i8x16.relaxed_swizzle",
        [0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF],
        [0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F],
        [0xA0, 0xA2, 0xA4, 0xA6, 0xA8, 0xAA, 0xAC, 0xAE, 0xA1, 0xA3, 0xA5, 0xA7, 0xA9, 0xAB, 0xAD, 0xAF]
    ],

    // i32x4.relaxed_trunc_f32x4_s tests - truncate f32 to signed i32, relaxed
    [
        "i32x4.relaxed_trunc_f32x4_s",
        [0.0, 1.0, -1.0, 42.5],
        [0x00000000, 0x00000001, 0xFFFFFFFF, 0x0000002A]
    ],
    [
        "i32x4.relaxed_trunc_f32x4_s",
        [123.7, -456.3, 789.999, -1000.1],
        [0x0000007B, 0xFFFFFE38, 0x00000315, 0xFFFFFC18]
    ],
    [
        "i32x4.relaxed_trunc_f32x4_s",
        [-0.0, 0.9, -0.9, 100.0],
        [0x00000000, 0x00000000, 0x00000000, 0x00000064]
    ],

    // i32x4.relaxed_trunc_f32x4_u tests - truncate f32 to unsigned i32, relaxed
    [
        "i32x4.relaxed_trunc_f32x4_u",
        [0.0, 1.0, 42.5, 100.9],
        [0x00000000, 0x00000001, 0x0000002A, 0x00000064]
    ],
    [
        "i32x4.relaxed_trunc_f32x4_u",
        [123.7, 456.3, 789.999, 1000.1],
        [0x0000007B, 0x000001C8, 0x00000315, 0x000003E8]
    ],
    [
        "i32x4.relaxed_trunc_f32x4_u",
        [0.9, 255.0, 256.0, 65536.0],
        [0x00000000, 0x000000FF, 0x00000100, 0x00010000]
    ],

    // i32x4.relaxed_trunc_f64x2_s_zero tests - truncate f64 to signed i32 (low 2 lanes), zero upper
    [
        "i32x4.relaxed_trunc_f64x2_s_zero",
        [0.0, 1.0],
        [0x00000000, 0x00000001, 0x00000000, 0x00000000]
    ],
    [
        "i32x4.relaxed_trunc_f64x2_s_zero",
        [123.7, -456.3],
        [0x0000007B, 0xFFFFFE38, 0x00000000, 0x00000000]
    ],
    [
        "i32x4.relaxed_trunc_f64x2_s_zero",
        [-1000.9, 2147483647.0],
        [0xFFFFFC18, 0x7FFFFFFF, 0x00000000, 0x00000000]
    ],

    // i32x4.relaxed_trunc_f64x2_u_zero tests - truncate f64 to unsigned i32 (low 2 lanes), zero upper
    [
        "i32x4.relaxed_trunc_f64x2_u_zero",
        [0.0, 1.0],
        [0x00000000, 0x00000001, 0x00000000, 0x00000000]
    ],
    [
        "i32x4.relaxed_trunc_f64x2_u_zero",
        [123.7, 456.3],
        [0x0000007B, 0x000001C8, 0x00000000, 0x00000000]
    ],
    [
        "i32x4.relaxed_trunc_f64x2_u_zero",
        [1000.9, 4294967295.0],
        [0x000003E8, 0xFFFFFFFF, 0x00000000, 0x00000000]
    ],

    // i8x16.relaxed_laneselect tests - lane-wise select, relaxed
    [
        "i8x16.relaxed_laneselect",
        [0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00],
        [0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF],
        [0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00],
        [0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF]
    ],
    [
        "i8x16.relaxed_laneselect",
        [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10],
        [0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20],
        [0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00],
        [0x01, 0x12, 0x03, 0x14, 0x05, 0x16, 0x07, 0x18, 0x09, 0x1A, 0x0B, 0x1C, 0x0D, 0x1E, 0x0F, 0x20]
    ],

    // i16x8.relaxed_laneselect tests - lane-wise select, relaxed
    [
        "i16x8.relaxed_laneselect",
        [0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000],
        [0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF],
        [0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000],
        [0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0xFFFF]
    ],
    [
        "i16x8.relaxed_laneselect",
        [0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008],
        [0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018],
        [0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000],
        [0x0001, 0x0012, 0x0003, 0x0014, 0x0005, 0x0016, 0x0007, 0x0018]
    ],

    // i32x4.relaxed_laneselect tests - lane-wise select, relaxed
    [
        "i32x4.relaxed_laneselect",
        [0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000],
        [0x00000000, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF],
        [0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000],
        [0xFFFFFFFF, 0x00000000, 0x00000000, 0xFFFFFFFF]
    ],
    [
        "i32x4.relaxed_laneselect",
        [0x00000001, 0x00000002, 0x00000003, 0x00000004],
        [0x00000011, 0x00000012, 0x00000013, 0x00000014],
        [0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000],
        [0x00000001, 0x00000012, 0x00000003, 0x00000014]
    ],

    // i64x2.relaxed_laneselect tests - lane-wise select, relaxed
    [
        "i64x2.relaxed_laneselect",
        [0xFFFFFFFFFFFFFFFFn, 0x0000000000000000n],
        [0x0000000000000000n, 0xFFFFFFFFFFFFFFFFn],
        [0xFFFFFFFFFFFFFFFFn, 0xFFFFFFFFFFFFFFFFn],
        [0xFFFFFFFFFFFFFFFFn, 0x0000000000000000n]
    ],
    [
        "i64x2.relaxed_laneselect",
        [0x0000000000000001n, 0x0000000000000002n],
        [0x0000000000000011n, 0x0000000000000012n],
        [0xFFFFFFFFFFFFFFFFn, 0x0000000000000000n],
        [0x0000000000000001n, 0x0000000000000012n]
    ],
];

await runSIMDTests(relaxedTests, verbose, "SIMD relaxed", { simd: true, relaxed_simd: true });