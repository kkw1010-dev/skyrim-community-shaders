// Scene key for Neural Rendering's proxy exposure when Post Processing publishes no auto exposure.
//
// Reduce: one group per 64x64 tile sums log2 luminance of every 4th pixel each way, with the centre
// weighted as Post Processing's AdaptArea does. Adapt: one group folds the tiles into the frame's
// log-average and eases the stored key toward it, so the proxy exposure follows the scene the way an
// eye adaptation does. ColorTransferCS then exposes the proxy by 0.18 / key (SceneExposure::Evaluate).

cbuffer SceneKey : register(b0)
{
	uint Width;
	uint Height;
	uint EyeOffsetX;
	uint TileCount;
	uint TilesX;
	float AdaptLerp;
	float2 Pad;
};

Texture2D<float4> Original : register(t0);
RWStructuredBuffer<float2> Tiles : register(u0);      // x: weighted sum of log2 luminance, y: weight
RWStructuredBuffer<float> Adaptation : register(u1);  // the adapted key, linear luminance

static const uint kGroupSize = 16;
static const uint kStride = 4;
static const float3 kLuminanceWeights = float3(0.2125, 0.7154, 0.0721);  // Rec.709, as the NR proxy uses
static const float kMinLog2 = -24.0;                                    // floor far below any lit pixel
static const float kCentreHalfExtent = 0.3;                             // the middle 60%, as AdaptArea's default
static const float kOutsideWeight = 0.1;

groupshared float2 partial[kGroupSize * kGroupSize];

void SumGroup(uint gidx)
{
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint s = kGroupSize * kGroupSize / 2; s > 0; s >>= 1)
	{
		if (gidx < s)
			partial[gidx] += partial[gidx + s];
		GroupMemoryBarrierWithGroupSync();
	}
}

[numthreads(kGroupSize, kGroupSize, 1)] void Reduce(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gidx : SV_GroupIndex)
{
	const uint2 pixel = (gid.xy * kGroupSize + gtid.xy) * kStride;
	float2 contribution = 0.0;
	if (all(pixel < uint2(Width, Height))) {
		const float luminance = dot(max(Original[pixel + uint2(EyeOffsetX, 0)].rgb, 0.0), kLuminanceWeights);
		const float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);
		const float weight = all(abs(uv - 0.5) < kCentreHalfExtent) ? 1.0 : kOutsideWeight;
		if (isfinite(luminance))
			contribution = float2(max(log2(max(luminance, 1e-30)), kMinLog2) * weight, weight);
	}
	partial[gidx] = contribution;
	SumGroup(gidx);
	if (gidx == 0)
		Tiles[gid.y * TilesX + gid.x] = partial[0];
}

[numthreads(kGroupSize * kGroupSize, 1, 1)] void Adapt(uint gidx : SV_GroupIndex)
{
	float2 sum = 0.0;
	for (uint i = gidx; i < TileCount; i += kGroupSize * kGroupSize)
		sum += Tiles[i];
	partial[gidx] = sum;
	SumGroup(gidx);
	if (gidx == 0 && partial[0].y > 0.0) {
		const float key = exp2(partial[0].x / partial[0].y);
		const float previous = Adaptation[0];
		// A first frame, or a stored value that is not a key, takes this frame's key directly.
		const bool seeded = isfinite(previous) && previous > 0.0;
		Adaptation[0] = seeded ? exp2(lerp(log2(previous), log2(key), AdaptLerp)) : key;
	}
}
