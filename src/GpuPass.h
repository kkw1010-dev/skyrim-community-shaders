#pragma once

// Minimal stand-in for Open Shaders' GpuPass.h, for the code ported from it (Neural Rendering).
// The 05-29 tree has no GPU-pass registry or profiler, so a pass is only a Tracy D3D11 zone,
// the same instrumentation 05-29's own passes use.

#include "State.h"

/// Scoped GPU zone for the enclosing block; `name` must be a string literal.
#define CS_GPU_PASS(name) TracyD3D11Zone(globals::state->tracyCtx, name)

#define CS_GPU_PASS_CONCAT_INNER(a, b) a##b
#define CS_GPU_PASS_CONCAT(a, b) CS_GPU_PASS_CONCAT_INNER(a, b)

/// Scoped GPU zone named by one of two string literals, picked at run time. Tracy's transient
/// zone takes the name per call, so both branches report under their own name.
#define CS_GPU_PASS_SELECT(cond, name1, name2) \
	TracyD3D11ZoneTransient(globals::state->tracyCtx, CS_GPU_PASS_CONCAT(cs_gpu_pass_, __LINE__), (cond) ? (name1) : (name2), true)
