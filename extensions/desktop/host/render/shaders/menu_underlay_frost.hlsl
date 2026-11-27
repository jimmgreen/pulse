// Half-resolution menu frost: exactly three radius-8 box passes, then 1.18 saturation.
// Compile at build time; the right-click path never invokes a shader compiler.
cbuffer Parameters : register(b0) {
    uint width;
    uint height;
    uint mode;
    uint unused;
};
Texture2D<float4> source : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    int2 pixel = int2(position.xy);
    if (mode == 2) {
        float3 rgb = round(source.Load(int3(pixel, 0)).rgb * 255.0);
        precise float luma = rgb.b * 0.114 + rgb.g * 0.587 + rgb.r * 0.299;
        precise float3 boosted = luma + (rgb - luma) * 1.18;
        return float4(floor(clamp(boosted, 0.0, 255.0)) / 255.0, 1);
    }
    uint3 sum = 0;
    int2 direction = mode == 0 ? int2(1, 0) : int2(0, 1);
    [unroll] for (int i = -8; i <= 8; ++i) {
        int2 p = clamp(pixel + i * direction, int2(0, 0), int2(width - 1, height - 1));
        sum += (uint3)round(source.Load(int3(p, 0)).rgb * 255.0);
    }
    return float4(((sum + 8u) / 17u) / 255.0, 1);
}
