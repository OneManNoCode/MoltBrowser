// Fixture for tools/sbom/molt_sbom_test.py: the registry layout of
// src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc.
const ModelRegistryEntry kModelRegistry[] = {
    // TinyLlama — runs on any machine. Apache 2.0.
    {"tinyllama-1.1b",
     "TinyLlama 1.1B Chat",
     "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
     "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     /*revision=*/"fe8a4ea1ffedaf415f4da2f062534de366a451e6",
     /*sha256=*/"f45760d86c0c5abf036f826be8431d323fefe074f64e36ce18859917e6a076ef",
     /*size=*/20ULL,  // ~0.6GB
     2147483648ULL,          // 2GB RAM
     1,
     "Q4_K_M",
     "TinyLlama",
     "apache-2.0",
     false},

    // An unpinned entry; "a quoted" comment must not confuse the parser.
    {"qwen3.5-4b",
     "Qwen3.5 4B",
     "Qwen/Qwen3.5-4B-Instruct-GGUF",  // TODO(pin): "confirm" repo name
     "Qwen3.5-4B-Instruct-Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/0ULL,
     6442450944ULL,
     4,
     "Q4_K_M",
     "Qwen",
     "",
     true},
};
