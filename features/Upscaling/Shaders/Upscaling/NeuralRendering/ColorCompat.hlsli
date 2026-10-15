#ifndef __NR_COLOR_COMPAT_HLSLI__
#define __NR_COLOR_COMPAT_HLSLI__

// Jiaye's Common/Color.hlsli lacks these definitions; Open Shaders keeps them in Color.hlsli.
// Kept here so the shared includes stay unchanged.
namespace Color
{
	static const float3 kRec709LuminanceWeights = float3(0.2125, 0.7154, 0.0721);

	float RGBToLuminance(float3 color, float3 luminanceWeights)
	{
		return dot(color, luminanceWeights);
	}

	// 10-03 moved the game gamma into TransferFunctions; NR keeps the 05-29 names.
	float3 SkyrimGammaToLinear(float3 color)
	{
		return TransferFunctions::GameGammaToLinear(color);
	}

	float3 LinearToSkyrimGamma(float3 color)
	{
		return TransferFunctions::LinearToGameGamma(color);
	}

	float LinearToSkyrimGamma(float color)
	{
		return TransferFunctions::LinearToGameGamma(color);
	}
}

namespace NR
{
	// 10-03 turns linear lighting and ACEScg into compile-time defines (empty when on, absent when off)
	// instead of SharedData flags, so NR reads them through these constants.
#if defined(ENABLE_LL)
	static const bool kLinearLighting = true;
#else
	static const bool kLinearLighting = false;
#endif
#if defined(ENABLE_ACESCG)
	static const bool kACEScg = true;
#else
	static const bool kACEScg = false;
#endif
}

#endif  // __NR_COLOR_COMPAT_HLSLI__
