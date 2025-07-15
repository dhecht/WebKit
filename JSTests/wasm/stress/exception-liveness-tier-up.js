import Builder from '../Builder.js'
import * as assert from '../assert.js'

{
    const b = new Builder();
    b.Type().End()
        .Import().Function("context", "callback", { params: [], ret: "i32" }).End()
        .Function().End()
        .Exception().Signature({ params: []}).End()
        .Export().Function("call")
        .End()
        .Code()
            .Function("call", { params: ["i32"], ret: "i32" })
                .GetLocal(0)
                .Loop("void")
                    .Call(2)
                    .Try("void")
                        .Throw(0)
                    .CatchAll()
                        .Call(0)
                        .I32Eqz()
                        .BrIf(1)
                    .End()
                .End()
            .End()
            .Function("wrapper", { params: [], ret: "void" })
            .End()
        .End()

    var counter = 1e5;
    const bin = b.WebAssembly().get();
    const module = new WebAssembly.Module(bin);
    const instance = new WebAssembly.Instance(module, { context: { callback: function() { if (!--counter) return true;} } });

    assert.eq(instance.exports.call(), 0);
}
