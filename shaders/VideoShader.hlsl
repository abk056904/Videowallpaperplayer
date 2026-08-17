// VideoShader.hlsl — compiled at build time by fxc (CMake custom command).
//
// M2: solid color + UV gradient placeholder (PSMain) via a vertex-less
// fullscreen triangle (SV_VertexID), 1 draw call. PSMainTexture is the M5
// preview path: samples a B8G8R8A8 frame texture (the M5 production path
// replaces this with NV12/P010 sampling + scaling; the constant buffer keeps
// the per-frame data interface stable).

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

cbuffer FrameCB : register(b0) {
    float4 tint;   // per-frame tint/pulse (proves frames advance)
    float4 pad;
};

Texture2D<float4> videoTexture : register(t0);
SamplerState linearSampler : register(s0);

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

// M5 preview: sample a decoded B8G8R8A8 frame (linear stretch to the window).
float4 PSMainTexture(PSInput i) : SV_Target {
    return videoTexture.Sample(linearSampler, i.uv) * tint;
}
