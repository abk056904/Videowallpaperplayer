// VideoShader.hlsl — compiled at build time by fxc (CMake custom command).
//
// M2: solid color + UV gradient placeholder (PSMain) via a vertex-less
// fullscreen triangle (SV_VertexID), 1 draw call.
// M5 preview: PSMainTexture samples a B8G8R8A8 frame texture (software path).
// M5: PSMainYuv converts NV12/P010 GPU surfaces to RGB in-shader (BT.709)
// with Fill/Fit/Stretch/Center scaling in the UV math (M5 review: ratios
// corrected + limited-range rescale; the scaling mapping is computed on the
// CPU in ScaleMath.h and applied identically on both paths).
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

// Software path: sample a decoded B8G8R8A8 frame. Applies the same scaling
// mapping as the hardware path (Fit/Center margins come back black via the
// BORDER sampler).
//
// VERTICAL ORIENTATION (M6 review fix, probe-verified): D3D11 textures have
// v=0 at the TOP row, but this vertex-less triangle maps screen-top to
// i.uv.y=1 — sampling v = uv.y*sy + oy directly would put the video's BOTTOM
// at the top of the screen (upside down; the checkerboard's even-cell
// symmetry hid it). v is therefore flipped: v' = 1 - (uv.y*sy + oy).
// Identity (sy=1, oy=0): screen top -> v=0 (video top), bottom -> v=1.
// Cropped Fill: the centered band [oy, oy+sy] still shows, top-to-bottom.
float4 PSMainTexture(PSInput i) : SV_Target {
    float2 texUv = float2(i.uv.x * scaleOffset.x + scaleOffset.z,
                          1.0 - i.uv.y * scaleOffset.y - scaleOffset.w);
    return videoTexture.Sample(linearSampler, texUv) * tint;
}

// Hardware path: NV12/P010 GPU surface -> RGB via the BT.709 matrix (HD video;
// the classic BT.601 matrix is the 8-bit fallback for SD sources). 8-bit NV12
// is normalized to [0,1] by the R8/R8G8 views; 10-bit P010 comes in as
// R16/R16G16 normalized the same way — both paths land Y/Cb/Cr in [0,1], so
// one matrix applies.
// Range (M5 review): H.264/HEVC decode output is LIMITED range (luma 16-235,
// chroma 16-240) — MFVideoNominalRange defaults to limited when unset — so
// Y/Cb/Cr are rescaled to full range BEFORE the matrix; applying the matrix
// directly would wash out blacks/whites. The 8-bit constants also fit 10-bit
// limited P010 (64-940) within 0.02% (imperceptible).
// Scaling (Fill default): the CPU computes scale/offset per mode (ScaleMath.h:
// Fill = cover/crop, Fit = contain/letterbox, Stretch = full frame, Center =
// 1:1) and the shader maps the fullscreen UV onto the texture UV.
float4 PSMainYuv(PSInput i) : SV_Target {
    // Same v-flip as PSMainTexture (D3D11 v=0 = texture top; screen top is
    // i.uv.y=1 — without the flip the video renders upside down).
    float2 texUv = float2(i.uv.x * scaleOffset.x + scaleOffset.z,
                          1.0 - i.uv.y * scaleOffset.y - scaleOffset.w);
    float y = yPlane.Sample(linearSampler, texUv).r;
    float2 uv = uvPlane.Sample(linearSampler, texUv).rg;

    // Limited -> full range: luma 16-235, chroma 16-240 (centered at 128).
    y = saturate((y - 16.0 / 255.0) * (255.0 / 219.0));
    float cb = (uv.r - 128.0 / 255.0) * (255.0 / 224.0);
    float cr = (uv.g - 128.0 / 255.0) * (255.0 / 224.0);

    // BT.709 (HD): Y'CbCr -> R'G'B' (full-range coefficients).
    float r = y + 1.5748 * cr;
    float g = y - 0.1873 * cb - 0.4681 * cr;
    float b = y + 1.8556 * cb;
    return float4(r, g, b, 1.0) * tint;
}
