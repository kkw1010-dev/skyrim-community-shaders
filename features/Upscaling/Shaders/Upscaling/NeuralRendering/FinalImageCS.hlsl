// Neural Rendering on the final image: after upscaling and tone mapping, on DLSS-G's HUD-less frame at
// output resolution, the way the user's other upscaler applies it.
//
// Prepare: copies the HUD-less frame into the runtime's input (it is already display-referred, the domain
// the model expects) and builds output-resolution guides from the render-resolution depth and motion that
// were shared for DLSS-G. Composite: writes the model's output back into the HUD-less frame, mixed with
// the original by the user's share; the UI is composed on top afterwards.

cbuffer FinalImage : register(b0)
{
	uint OutputWidth;
	uint OutputHeight;
	uint RenderWidth;   // the rendered region in the top-left of the shared depth and motion
	uint RenderHeight;
	float Mix;          // share of the model's output, 0..1
	float3 Pad;
};

Texture2D<float4> Hudless : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float2> SceneMotion : register(t2);

RWTexture2D<float4> InputColor : register(u0);
RWTexture2D<float> GuideDepth : register(u1);
RWTexture2D<float2> GuideMotion : register(u2);

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= OutputWidth || id.y >= OutputHeight)
		return;
	InputColor[id.xy] = float4(saturate(Hudless[id.xy].rgb), 1.0);
	// Nearest render pixel: depth must not blend across edges. Motion is in UV units, the same at any resolution.
	const float2 scale = float2(RenderWidth, RenderHeight) / float2(OutputWidth, OutputHeight);
	const uint2 source = min(uint2((float2(id.xy) + 0.5) * scale), uint2(RenderWidth, RenderHeight) - 1);
	GuideDepth[id.xy] = SceneDepth[source];
	GuideMotion[id.xy] = SceneMotion[source];
}

Texture2D<float4> Original : register(t0);
Texture2D<float4> Neural : register(t1);

RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void Composite(uint3 id : SV_DispatchThreadID) {
	if (id.x >= OutputWidth || id.y >= OutputHeight)
		return;
	const float4 original = Original[id.xy];
	float3 neural = Neural[id.xy].rgb;
	// A failed or empty sample keeps the frame as it was.
	if (!all(isfinite(neural)))
		neural = original.rgb;
	Output[id.xy] = float4(lerp(original.rgb, saturate(neural), saturate(Mix)), original.a);
}
