// Neural Rendering on the final image: after upscaling and tone mapping, on DLSS-G's HUD-less frame at
// output resolution, the way the user's other upscaler applies it.
//
// Prepare: copies the HUD-less frame into the runtime's input (it is already display-referred, the domain
// the model expects), at the working size, and builds working-size guides from the render-resolution depth
// and motion that were shared for DLSS-G. Composite: adds the model's edit back onto the full-size HUD-less
// frame, scaled by the user's share; the UI is composed on top afterwards.
//
// Working scale: the model runs at WorkWidth x WorkHeight (a share of the output size, to cut its cost, which
// grows with the pixel count). Only its edit is upscaled, base + up(NR(low)) - up(low), so the frame keeps its
// own full-resolution detail (melee-unlocked / DLSSNR-Cost-Scaler). At full size this is the model's output.

cbuffer FinalImage : register(b0)
{
	uint OutputWidth;
	uint OutputHeight;
	uint RenderWidth;  // the rendered region in the top-left of the shared depth and motion
	uint RenderHeight;
	float Mix;         // share of the model's edit, 0..1
	uint WorkWidth;    // the model's working size
	uint WorkHeight;
	float Pad;
};

SamplerState LinearClamp : register(s0);

Texture2D<float4> Hudless : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float2> SceneMotion : register(t2);

RWTexture2D<float4> InputColor : register(u0);
RWTexture2D<float> GuideDepth : register(u1);
RWTexture2D<float2> GuideMotion : register(u2);

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= WorkWidth || id.y >= WorkHeight)
		return;
	const float2 uv = (float2(id.xy) + 0.5) / float2(WorkWidth, WorkHeight);
	// At full size the sample lands on a texel centre and reads it exactly.
	InputColor[id.xy] = float4(saturate(Hudless.SampleLevel(LinearClamp, uv, 0).rgb), 1.0);
	// Nearest render pixel: depth must not blend across edges. Motion is in UV units, the same at any resolution.
	const uint2 source = min(uint2(uv * float2(RenderWidth, RenderHeight)), uint2(RenderWidth, RenderHeight) - 1);
	GuideDepth[id.xy] = SceneDepth[source];
	GuideMotion[id.xy] = SceneMotion[source];
}

Texture2D<float4> Original : register(t0);
Texture2D<float4> Neural : register(t1);
Texture2D<float4> NeuralInput : register(t2);

RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void Composite(uint3 id : SV_DispatchThreadID) {
	if (id.x >= OutputWidth || id.y >= OutputHeight)
		return;
	const float4 original = Original[id.xy];
	const float2 uv = (float2(id.xy) + 0.5) / float2(OutputWidth, OutputHeight);
	const float3 neural = Neural.SampleLevel(LinearClamp, uv, 0).rgb;
	// A failed or empty sample keeps the frame as it was.
	if (!all(isfinite(neural))) {
		Output[id.xy] = original;
		return;
	}
	const float3 input = NeuralInput.SampleLevel(LinearClamp, uv, 0).rgb;
	Output[id.xy] = float4(saturate(original.rgb + (saturate(neural) - input) * saturate(Mix)), original.a);
}
