// VideoShader.hlsl — compiled at build time by fxc (CMake custom command).
//
// M2 placeholder: solid color + UV gradient via a vertex-less fullscreen
// triangle (SV_VertexID), 1 draw call. M5 extends this to sample video
// textures (NV12/P010) with scaling; the constant buffer keeps the per-frame
// data interface stable.

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

cbuffer FrameCB : register(b0) {
    float4 tint;   // per-frame tint/pulse (proves frames advance)
    float4 pad;
};

// Fullscreen triangle — no vertex buffer, one Draw(3, 0) call.
PSInput VSMain(uint id : SV_VertexID) {
    PSInput o;
    float2 uv = float2(float((id << 1) & 2), float(id & 2)); // (0,0),(2,0),(0,2)
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(PSInput i) : SV_Target {
    float4 base = float4(i.uv, 1.0 - i.uv.x, 1.0); // UV gradient
    return base * tint;
}
