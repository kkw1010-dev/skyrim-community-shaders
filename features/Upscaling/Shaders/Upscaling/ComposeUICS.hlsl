Texture2D<float4> HudlessTexture : register(t0);
Texture2D<float4> UITexture : register(t1);

RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint2 size;
	OutputTexture.GetDimensions(size.x, size.y);
	if (any(dispatchID.xy >= size))
		return;

	float4 ui = UITexture[dispatchID.xy];
	OutputTexture[dispatchID.xy] = float4(ui.rgb + (1.0 - ui.a) * HudlessTexture[dispatchID.xy].rgb, 1.0);
}
