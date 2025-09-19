//@ requireOptions("--useWasmSIMD=1")
//@ skip if !$isSIMDPlatform
import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    (memory (export "memory") 1)

    (func $swap_v128 (param $vec1 v128) (param $vec2 v128) (result v128 v128)
        (local.get $vec2)  ;; Return second argument first
        (local.get $vec1)  ;; Return first argument second
    )

    (func (export "test_v128_call") (param $dst1 i32) (param $dst2 i32)
        (local $temp1 v128)
        (local $temp2 v128)

        (call $swap_v128
            (v128.const i32x4 10 20 30 40)       ;; First v128 argument
            (v128.const i32x4 100 200 300 400)   ;; Second v128 argument
        )

        (local.set $temp2)  ;; Pop second return value
        (local.set $temp1)  ;; Pop first return value

        ;; Store to memory
        (v128.store (local.get $dst1) (local.get $temp1))
        (v128.store (local.get $dst2) (local.get $temp2))
    )
)
`

async function test() {
    const instance = await instantiate(wat, {}, { simd: true });
    const { memory, test_v128_call } = instance.exports;

    // Create typed array views for easy data manipulation
    const i32View = new Int32Array(memory.buffer);

    // Helper function to get i32x4 data at byte offset
    function getI32x4(byteOffset) {
        const i32Offset = byteOffset / 4;
        return [i32View[i32Offset], i32View[i32Offset + 1], i32View[i32Offset + 2], i32View[i32Offset + 3]];
    }

    // Test v128 function call with return values
    {
        const dst1Addr = 0;
        const dst2Addr = 16;

        // Call function that passes two v128 values and gets them back swapped
        test_v128_call(dst1Addr, dst2Addr);

        // Verify first destination gets the swapped second argument [100, 200, 300, 400]
        const result1 = getI32x4(dst1Addr);
        assert.eq(result1[0], 100);
        assert.eq(result1[1], 200);
        assert.eq(result1[2], 300);
        assert.eq(result1[3], 400);

        // Verify second destination gets the swapped first argument [10, 20, 30, 40]
        const result2 = getI32x4(dst2Addr);
        assert.eq(result2[0], 10);
        assert.eq(result2[1], 20);
        assert.eq(result2[2], 30);
        assert.eq(result2[3], 40);
    }
}

await assert.asyncTest(test())