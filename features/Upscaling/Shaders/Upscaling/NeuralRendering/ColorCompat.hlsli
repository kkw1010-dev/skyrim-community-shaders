#ifndef __NR_COLOR_COMPAT_HLSLI__
#define __NR_COLOR_COMPAT_HLSLI__

// The 05-29 Common/Color.hlsli predates these definitions; Open Shaders keeps them in Color.hlsli.
// Kept here so the 05-29 shared includes stay unchanged.
namespace Color
{
	static const float3 kRec709LuminanceWeights = float3(0.2125, 0.7154, 0.0721);

	float RGBToLuminance(float3 color, float3 luminanceWeights)
	{
		return dot(color, luminanceWeights);
	}
}

#endif  // __NR_COLOR_COMPAT_HLSLI__
