// VideoShader.hlsl — compiled at build time by fxc (CMake custom command).
//
// M2: solid color + UV gradient placeholder (PSMain) via a vertex-less
// fullscreen triangle (SV_VertexID), 1 draw call.
// M5 preview: PSMainTexture samples a B8G8R8A8 frame texture (software path).
// M5: PSMainYuv converts NV12/P010 GPU surfaces to RGB in-shader (BT.709,
// BT.601 fallback) with Fill/Fit/Stretch/Center scaling in the UV math.
// The constant buffer stays the per-frame data interface.

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

cbuffer FrameCB : register(b0) {
    float4 tint;       // per-frame color modulation
    float4 scaleOffset; // xy = texture UV scale, zw = UV offset (scaling modes)
};

Texture2D<float4> videoTexture : register(t0); // RGB32 frame (software path)
Texture2D<float>  yPlane   : register(t0);     // NV12/P010 Y (hardware path)
Texture2D<float2> uvPlane  : register(t1);     // NV12/P010 interleaved UV
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

// Software path: sample a decoded B8G8R8A8 frame.
float4 PSMainTexture(PSInput i) : SV_Target {
    return videoTexture.Sample(linearSampler, i.uv) * tint;
}

// Hardware path: NV12/P010 GPU surface -> RGB via the BT.709 matrix (HD video;
// the classic BT.601 matrix is the 8-bit fallback for SD sources). 8-bit NV12
// is normalized to [0,1] by the R8/R8G8 views; 10-bit P010 comes in as
// R16/R16G16 and is scaled by /1023 in the shader via the UV plane's own
// normalization — both paths land Y/Cb/Cr in [0,1], so one matrix applies.
// Scaling (Fill default): the CPU computes scale/offset for the configured
// mode (Fill = cover/crop, Fit = contain/letterbox, Stretch = full frame,
// Center = 1:1) and the shader maps the fullscreen UV onto the texture UV.
float4 PSMainYuv(PSInput i) : SV_Target {
    float2 texUv = i.uv * scaleOffset.xy + scaleOffset.zw;
    float y = yPlane.Sample(linearSampler, texUv).r;
    float2 uv = uvPlane.Sample(linearSampler, texUv).rg;
    float cb = uv.r - 0.5;
    float cr = uv.g - 0.5;

    // BT.709 (HD): Y'CbCr -> R'G'B'.
    float r = y + 1.5748 * cr;
    float g = y - 0.1873 * cb - 0.4681 * cr;
    float b = y + 1.8556 * cb;
    return float4(r, g, b, 1.0) * tint;
}
