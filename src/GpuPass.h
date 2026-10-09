#pragma once

// Minimal stand-in for Open Shaders' GpuPass.h, for the code ported from it (Neural Rendering).
// The 05-29 tree has no GPU-pass registry or profiler, so a pass is only a Tracy D3D11 zone,
// the same instrumentation 05-29's own passes use.

#include "State.h"

/// Scoped GPU zone for the enclosing block; `name` must be a string literal.
#define CS_GPU_PASS(name) TracyD3D11Zone(globals::state->tracyCtx, name)
