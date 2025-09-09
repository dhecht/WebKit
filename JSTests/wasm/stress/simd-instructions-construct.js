//@ requireOptions("--useWasmSIMD=1")
//@ skip if !$isSIMDPlatform
import { runSIMDTests } from "./simd-instructions-lib.js"

const verbose = false;

// Table-driven test data for SIMD construction instructions
// Each entry: [instruction, input0, input1, expected_output]
const constructTests = [
    // i8x16.splat - splat i32 value to 16 8-bit lanes
    [
        "i8x16.splat",
        0x42,  // scalar input (66 decimal)
        [0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42]
    ],
    [
        "i8x16.splat", 
        0xFF,  // test with max 8-bit value
        [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]
    ],
    [
        "i8x16.splat",
        0x00,  // test with zero
        [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
    ],
    [
        "i8x16.splat",
        -1,    // test with negative value (should become 0xFF)
        [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]
    ],
    [
        "i8x16.splat",
        -128,  // test with most negative 8-bit value
        [0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80]
    ],

    // i16x8.splat - splat i32 value to 8 16-bit lanes
    [
        "i16x8.splat",
        0x1234,  // scalar input
        [0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0x1234, 0x1234]
    ],
    [
        "i16x8.splat",
        0xFFFF,  // test with max 16-bit value
        [0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF]
    ],
    [
        "i16x8.splat",
        0x0000,  // test with zero
        [0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000]
    ],
    [
        "i16x8.splat",
        -1,      // test with negative value
        [0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF]
    ],
    [
        "i16x8.splat",
        -32768,  // test with most negative 16-bit value
        [0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000]
    ],

    // i32x4.splat - splat i32 value to 4 32-bit lanes
    [
        "i32x4.splat",
        0x12345678,  // scalar input
        [0x12345678, 0x12345678, 0x12345678, 0x12345678]
    ],
    [
        "i32x4.splat",
        0xFFFFFFFF,  // test with max 32-bit value
        [0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF]
    ],
    [
        "i32x4.splat",
        0x00000000,  // test with zero
        [0x00000000, 0x00000000, 0x00000000, 0x00000000]
    ],
    [
        "i32x4.splat",
        -1,          // test with negative value
        [0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF]
    ],
    [
        "i32x4.splat",
        -2147483648, // test with most negative 32-bit value
        [0x80000000, 0x80000000, 0x80000000, 0x80000000]
    ],

    // i64x2.splat - splat i64 value to 2 64-bit lanes
    [
        "i64x2.splat",
        0x123456789ABCDEF0n,  // scalar input (BigInt)
        [0x123456789ABCDEF0n, 0x123456789ABCDEF0n]
    ],
    [
        "i64x2.splat",
        0xFFFFFFFFFFFFFFFFn,  // test with max 64-bit value
        [0xFFFFFFFFFFFFFFFFn, 0xFFFFFFFFFFFFFFFFn]
    ],
    [
        "i64x2.splat",
        0x0000000000000000n,  // test with zero
        [0x0000000000000000n, 0x0000000000000000n]
    ],
    [
        "i64x2.splat",
        -1n,                  // test with negative value
        [0xFFFFFFFFFFFFFFFFn, 0xFFFFFFFFFFFFFFFFn]
    ],
    [
        "i64x2.splat",
        -9223372036854775808n, // test with most negative 64-bit value
        [0x8000000000000000n, 0x8000000000000000n]
    ],

    // f32x4.splat - splat f32 value to 4 32-bit float lanes
    [
        "f32x4.splat",
        3.14159,  // scalar input
        [3.14159, 3.14159, 3.14159, 3.14159]
    ],
    [
        "f32x4.splat",
        0.0,      // test with zero
        [0.0, 0.0, 0.0, 0.0]
    ],
    [
        "f32x4.splat",
        -0.0,     // test with negative zero
        [-0.0, -0.0, -0.0, -0.0]
    ],
    [
        "f32x4.splat",
        -42.5,    // test with negative value
        [-42.5, -42.5, -42.5, -42.5]
    ],
    [
        "f32x4.splat",
        Number.POSITIVE_INFINITY,  // test with infinity
        [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY]
    ],
    [
        "f32x4.splat",
        Number.NEGATIVE_INFINITY,  // test with negative infinity
        [Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY]
    ],

    // f64x2.splat - splat f64 value to 2 64-bit float lanes
    [
        "f64x2.splat",
        2.718281828459045,  // scalar input (e)
        [2.718281828459045, 2.718281828459045]
    ],
    [
        "f64x2.splat",
        0.0,                // test with zero
        [0.0, 0.0]
    ],
    [
        "f64x2.splat",
        -0.0,               // test with negative zero
        [-0.0, -0.0]
    ],
    [
        "f64x2.splat",
        -123.456789,        // test with negative value
        [-123.456789, -123.456789]
    ],
    [
        "f64x2.splat",
        Number.POSITIVE_INFINITY,  // test with infinity
        [Number.POSITIVE_INFINITY, Number.POSITIVE_INFINITY]
    ],
    [
        "f64x2.splat",
        Number.NEGATIVE_INFINITY,  // test with negative infinity
        [Number.NEGATIVE_INFINITY, Number.NEGATIVE_INFINITY]
    ],

    // i8x16.shuffle - shuffle bytes from two vectors using immediate indices
    // Note: shuffle indices are immediate operands (0-31), where 0-15 select from first vector, 16-31 from second
    [
        "i8x16.shuffle",
        [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F], // first vector
        [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F], // second vector
        // For this test, we need to specify the immediate indices in the instruction itself
        // This is a limitation - shuffle requires immediate operands that can't be easily tested with this framework
        // We'll create a simple identity shuffle: select all from first vector in order
        [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F]
    ],

    // i8x16.swizzle - swizzle bytes from first vector using indices from second vector
    // Indices >= 16 produce 0, valid indices (0-15) select from first vector
    [
        "i8x16.swizzle",
        [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00], // data vector
        [0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00], // index vector (reverse order)
        [0x00, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA]  // expected (reversed)
    ],
    [
        "i8x16.swizzle",
        [0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x00], // data vector
        [0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F], // index vector (even then odd)
        [0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0x00]  // expected
    ],
    [
        "i8x16.swizzle",
        [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10], // data vector
        [0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F], // index vector (all invalid >= 16)
        [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]  // expected (all zeros)
    ],
    [
        "i8x16.swizzle",
        [0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F], // data vector
        [0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06, 0x07, 0x07], // index vector (duplicates)
        [0xF0, 0xF0, 0xE1, 0xE1, 0xD2, 0xD2, 0xC3, 0xC3, 0xB4, 0xB4, 0xA5, 0xA5, 0x96, 0x96, 0x87, 0x87]  // expected (duplicated values)
    ]
];

await runSIMDTests(constructTests, verbose, "SIMD construct");
