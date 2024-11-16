// Load the compiled-to-JS reference interpreter for GC proposal.
import "./wast.js";

export function compile(wat) {
    const binary = WebAssemblyText.encode(wat);
    return new WebAssembly.Module(binary);
}

export function instantiate(wat, imports = {}) {
    print(wat);
    const module = compile(wat);
    print("Compiled\n");
    let r = new WebAssembly.Instance(module, imports);
    print("Got instance");
    return r;
}
