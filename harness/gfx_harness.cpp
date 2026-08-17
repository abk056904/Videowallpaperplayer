// gfx_harness.cpp — minimal windowed D3D11 verification harness (dev-only, NOT shipped).
//
// Proves on THIS machine that the M2 renderer prerequisites hold:
//   - D3D11 hardware device creation (feature levels 11_1/11_0, BGRA support)
//   - Debug layer availability in Debug builds
//   - DXGI adapter/output enumeration matches reality (hybrid GPU: NVIDIA + AMD)
//   - Flip-model swap chain + vsync present (~60 FPS)
//   - Fullscreen-triangle render (UV gradient + per-frame pulse)
//
// Usage:
//   vw_gfx_harness [--list] [--adapter N] [--frames N] [--no-debug]
//     --list     enumerate adapters/outputs and exit (no window)
//     --adapter N  use adapter index N (default 0)
//     --frames N   auto-exit after N rendered frames (default 0 = until closed)
//     --no-debug   skip the D3D11 debug layer even in Debug builds
//
// NOTE: the shipped app bans busy loops; this harness is a dev tool and uses a
// vsync-blocked render loop so it can be driven from scripts / CI.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// ---- tiny helpers ---------------------------------------------------------

[[noreturn]] void fail(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fwprintf(stderr, L"gfx_harness: ");
    std::vfwprintf(stderr, fmt, args);
    std::fwprintf(stderr, L"\n");
    va_end(args);
    std::exit(1);
}

void logHr(const char* what, HRESULT hr) {
    std::printf("gfx_harness: %s: hr=0x%08X\n", what, static_cast<unsigned>(hr));
}

// ---- adapter / output enumeration ----------------------------------------

struct AdapterInfo {
    std::wstring description;
    UINT vendor = 0;
    UINT device = 0;
    uint64_t dedicatedVram = 0;
    uint64_t sharedSystem = 0;
};

void enumerateAdapters(std::vector<AdapterInfo>& out) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        fail(L"CreateDXGIFactory2 failed");
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        AdapterInfo info;
        info.description = desc.Description;
        info.vendor = desc.VendorId;
        info.device = desc.DeviceId;
        info.dedicatedVram = desc.DedicatedVideoMemory;
        info.sharedSystem = desc.SharedSystemMemory;
        out.push_back(std::move(info));
    }
}

void logOutputs(ComPtr<IDXGIAdapter1> adapter) {
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC od{};
        output->GetDesc(&od);
        // Current mode (resolution + refresh) via the display settings API.
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        std::wstring mode = L"?";
        if (::EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            mode = std::to_wstring(dm.dmPelsWidth) + L"x" + std::to_wstring(dm.dmPelsHeight) +
                   L" @" + std::to_wstring(dm.dmDisplayFrequency) + L" Hz";
        }
        std::wprintf(L"  output %u: %s  [%ld,%ld - %ld,%ld]  current=%s\n",
                     i, od.DeviceName, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                     od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, mode.c_str());
        output.Reset();
    }
}

void printAdapters(const std::vector<AdapterInfo>& adapters) {
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& a = adapters[i];
        std::wprintf(L"adapter %zu: %s  (vendor=0x%04X device=0x%04X, dedicated VRAM=%.1f GB, "
                     L"shared sys=%.1f GB)\n",
                     i, a.description.c_str(), a.vendor, a.device,
                     a.dedicatedVram / (1024.0 * 1024 * 1024),
                     a.sharedSystem / (1024.0 * 1024 * 1024));
    }
}

// ---- shader (compiled at runtime with D3DCompile; the shipped app uses
//      build-time fxc per M2) -----------------------------------------------

const char kShader[] = R"HLSL(
struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

cbuffer FrameCB : register(b0) {
    float4 tint;
    float4 pad;
};

// Fullscreen triangle via SV_VertexID — no vertex buffer, one draw call.
PSInput VSMain(uint id : SV_VertexID) {
    PSInput o;
    float2 uv = float2(float((id << 1) & 2), float(id & 2)); // (0,0),(2,0),(0,2)
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(PSInput i) : SV_Target {
    float4 base = float4(i.uv, 1.0 - i.uv.x, 1.0); // UV gradient
    return base * tint;                             // per-frame pulse
}
)HLSL";

ComPtr<ID3DBlob> compileStage(const char* entry, const char* target) {
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> blob, err;
    const HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "VideoShader.hlsl", nullptr,
                                  nullptr, entry, target, flags, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            std::fprintf(stderr, "shader compile failed (%s): %.*s\n", entry,
                         static_cast<int>(err->GetBufferSize()),
                         static_cast<const char*>(err->GetBufferPointer()));
        } else {
            std::fprintf(stderr, "shader compile failed (%s): hr=0x%08X\n", entry,
                         static_cast<unsigned>(hr));
        }
        std::exit(1);
    }
    return blob;
}

// ---- device + swap chain --------------------------------------------------

struct Device {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level{};
    bool debug = false;
};

Device createDevice(IDXGIAdapter1* adapter, bool wantDebug) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    Device out;
    if (wantDebug) {
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       flags | D3D11_CREATE_DEVICE_DEBUG, levels, 2,
                                       D3D11_SDK_VERSION, &out.dev, &out.level, &out.ctx);
        if (SUCCEEDED(hr)) {
            out.debug = true;
            return out;
        }
        logHr("debug-layer device creation failed (falling back)", hr);
    }
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &out.dev, &out.level, &out.ctx);
    if (FAILED(hr)) fail(L"D3D11CreateDevice failed (hr=0x%08X)", static_cast<unsigned>(hr));
    return out;
}

struct Pipeline {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> rs;
};

Pipeline createPipeline(ID3D11Device* dev) {
    Pipeline p;
    auto vsBlob = compileStage("VSMain", "vs_5_0");
    auto psBlob = compileStage("PSMain", "ps_5_0");
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &p.vs);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &p.ps);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32; // two float4
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &p.cb);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // fullscreen triangle winding is arbitrary
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &p.rs);
    return p;
}

// ---- window ---------------------------------------------------------------

const wchar_t kWinClass[] = L"VwGfxHarnessWindow";

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_SIZE: {
            // Flag resize; handled in the render loop (needs the context).
            auto* p = reinterpret_cast<bool*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (p && wParam != SIZE_MINIMIZED) *p = true;
            return 0;
        }
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct FrameCB {
    float tint[4];
    float pad[4];
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Per-monitor DPI awareness (same as the app) so sizes are physical pixels.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Debug layer is Debug-build-only (matches M2: "Release has no debug layer").
#ifdef _DEBUG
    bool wantDebug = true;
#else
    bool wantDebug = false;
#endif
    bool listOnly = false;
    UINT adapterIndex = 0;
    uint64_t maxFrames = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--list") listOnly = true;
        else if (a == L"--no-debug") wantDebug = false;
        else if (a == L"--adapter" && i + 1 < argc) adapterIndex = static_cast<UINT>(std::wcstoul(argv[++i], nullptr, 10));
        else if (a == L"--frames" && i + 1 < argc) maxFrames = std::wcstoull(argv[++i], nullptr, 10);
        else fail(L"unknown argument: %s", a.c_str());
    }

    // 1. Enumerate and log adapters + outputs (M2: "adapter/output log matches reality").
    std::vector<AdapterInfo> adapters;
    enumerateAdapters(adapters);
    std::printf("gfx_harness: found %zu adapter(s):\n", adapters.size());
    printAdapters(adapters);
    if (adapters.empty()) fail(L"no DXGI adapters found");

    if (adapterIndex >= adapters.size()) {
        fail(L"--adapter %u out of range (have %zu)", adapterIndex, adapters.size());
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) fail(L"CreateDXGIFactory2 failed");
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(adapterIndex, &adapter))) fail(L"EnumAdapters1 failed");
    std::wprintf(L"gfx_harness: using adapter %u: %s\n", adapterIndex,
                 adapters[adapterIndex].description.c_str());
    logOutputs(adapter);

    if (listOnly) {
        std::printf("gfx_harness: --list done\n");
        return 0;
    }

    // 2. Window.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kWinClass;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);

    bool resizePending = false;
    HWND hwnd = ::CreateWindowExW(0, kWinClass, L"Video Wallpaper — gfx harness", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 800, 450, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    if (!hwnd) fail(L"CreateWindowExW failed");
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&resizePending));
    ::ShowWindow(hwnd, SW_SHOW);

    // 3. Device + swap chain + pipeline.
    Device device = createDevice(adapter.Get(), wantDebug);
    std::printf("gfx_harness: device created, feature level=%X, debug layer=%s\n",
                static_cast<unsigned>(device.level), device.debug ? "ON" : "off");

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    UINT w = static_cast<UINT>(rc.right - rc.left);
    UINT h = static_cast<UINT>(rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.Stereo = FALSE;
    scd.SampleDesc = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = factory->CreateSwapChainForHwnd(device.dev.Get(), hwnd, &scd, nullptr, nullptr,
                                                 &swapChain);
    if (FAILED(hr)) fail(L"CreateSwapChainForHwnd failed (hr=0x%08X)", static_cast<unsigned>(hr));
    // Don't Alt+Tab to a fullscreen-exclusive state.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    Pipeline pipeline = createPipeline(device.dev.Get());

    // 4. Render loop (vsync-blocked; harness-only).
    ComPtr<ID3D11RenderTargetView> rtv;
    auto makeRtv = [&]() {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) fail(L"GetBuffer failed");
        device.dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    };
    makeRtv();

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    device.ctx->RSSetViewports(1, &vp);

    uint64_t frames = 0;
    double fpsAccum = 0.0;
    const auto tStart = std::chrono::steady_clock::now();
    auto lastReport = tStart;

    bool running = true;
    MSG msg{};
    while (running) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (resizePending) {
            resizePending = false;
            ::GetClientRect(hwnd, &rc);
            w = static_cast<UINT>(rc.right - rc.left);
            h = static_cast<UINT>(rc.bottom - rc.top);
            if (w > 0 && h > 0) {
                rtv.Reset();
                swapChain->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
                makeRtv();
                vp.Width = static_cast<float>(w);
                vp.Height = static_cast<float>(h);
                device.ctx->RSSetViewports(1, &vp);
            }
        }

        const float clear[4] = {0.04f, 0.05f, 0.09f, 1.0f}; // solid base
        device.ctx->ClearRenderTargetView(rtv.Get(), clear);
        device.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
        FrameCB cb{};
        cb.tint[0] = 0.55f + 0.45f * static_cast<float>(std::sin(t * 1.2));
        cb.tint[1] = 0.55f + 0.45f * static_cast<float>(std::cos(t * 0.8));
        cb.tint[2] = 0.6f;
        cb.tint[3] = 1.0f;
        device.ctx->UpdateSubresource(pipeline.cb.Get(), 0, nullptr, &cb, 0, 0);

        device.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device.ctx->IASetInputLayout(nullptr);
        device.ctx->VSSetShader(pipeline.vs.Get(), nullptr, 0);
        device.ctx->PSSetShader(pipeline.ps.Get(), nullptr, 0);
        device.ctx->VSSetConstantBuffers(0, 1, pipeline.cb.GetAddressOf());
        device.ctx->PSSetConstantBuffers(0, 1, pipeline.cb.GetAddressOf());
        device.ctx->RSSetState(pipeline.rs.Get());
        device.ctx->Draw(3, 0);

        const HRESULT presentHr = swapChain->Present(1, 0); // vsync
        if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
            fail(L"device lost during present (hr=0x%08X) — M2 device-loss plumbing will handle "
                 L"this in the real renderer", static_cast<unsigned>(presentHr));
        } else if (FAILED(presentHr)) {
            logHr("present failed", presentHr);
        }

        ++frames;
        fpsAccum += 1.0;
        const auto now = std::chrono::steady_clock::now();
        const double sinceReport = std::chrono::duration<double>(now - lastReport).count();
        if (sinceReport >= 1.0) {
            const double fps = fpsAccum / sinceReport;
            std::wstring title = L"Video Wallpaper — gfx harness — " + adapters[adapterIndex].description +
                                 L" — " + std::to_wstring(static_cast<int>(fps + 0.5)) + L" FPS";
            ::SetWindowTextW(hwnd, title.c_str());
            std::printf("gfx_harness: %d FPS over %.1f s (frames=%llu)\n",
                        static_cast<int>(fps + 0.5), sinceReport,
                        static_cast<unsigned long long>(frames));
            fpsAccum = 0.0;
            lastReport = now;
        }

        if (maxFrames > 0 && frames >= maxFrames) {
            std::printf("gfx_harness: reached --frames %llu, exiting cleanly\n",
                        static_cast<unsigned long long>(maxFrames));
            break;
        }
    }

    // Clean shutdown: window destroyed via WM_DESTROY or loop break.
    if (hwnd) ::DestroyWindow(hwnd);
    // COM/WRL handles release on scope exit.
    std::printf("gfx_harness: done (%llu frames)\n", static_cast<unsigned long long>(frames));
    return 0;
}
