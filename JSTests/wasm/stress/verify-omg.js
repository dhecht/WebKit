import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
(module
    (import "m" "isOMG" (func $isOMG (result i32)))

    (func $test (export "test") (result i32)
      (call $isOMG)
    )
)
`

async function run() {

    const instance = await instantiate(wat, { m: { isOMG: $vm.omgTrue } })
    const { test } = instance.exports

    function checkForOMG() {
        const wasOMG = test();
        print("wasOMG =", wasOMG);
    }
    checkForOMG();
    checkForOMG();
}

await assert.asyncTest(run())
