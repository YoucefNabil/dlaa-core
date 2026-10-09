#pragma once
#include <cstddef>
#include <cstdint>

namespace dlaa_core::shaders {

// Stable 64-bit id of a shader's bytecode (the DXBC checksum when present), so
// passes can be recognized across runs and game restarts.
uint64_t HashBytecode(const void* bytecode, size_t length);

// stage: "vs" / "ps" / "cs". With [Debug] DumpShaders=1 the bytecode is also
// written once to <prefix>_shaders\<hash>.<stage>.cso for disassembly.
// Returns the shader's hash (0 for a null shader or bytecode).
uint64_t Register(const void* shader, const void* bytecode, size_t length, const char* stage);
uint64_t Lookup(const void* shader);  // 0 if unknown / null

}  // namespace dlaa_core::shaders
