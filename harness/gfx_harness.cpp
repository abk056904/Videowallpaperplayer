// gfx_harness.cpp — minimal windowed D3D11 verification harness (dev-only, NOT shipped).
//
// Drives the real M2 modules (D3D11DeviceManager + D3D11Renderer) to prove on
// THIS machine:
//   - device creation on any adapter (feature levels 11_1/11_0, BGRA, debug layer)
//   - DXGI adapter/output enumeration matches reality (hybrid GPU: AMD + NVIDIA)
//   - flip-model swap chain + vsync present (~60 FPS)
//   - fullscreen-triangle render (UV gradient + per-frame pulse) via the
//     build-time-compiled, embedded shader
//   - the device-loss plumbing stub (scheduleRecreate / consumeRecreateRequest)
//   - M5 preview: --video <path> decodes ONE frame via Media Foundation (RGB32
//     output through the color converter), uploads it to a D3D11 texture, and
//     renders it through the textured pixel shader (proves the texture path;
//     the M5 production path does NV12/P010 on the GPU instead)
//
// Usage:
//   vw_gfx_harness [--list] [--adapter N] [--frames N] [--no-debug]
//                   [--video <path>] [--scaling fill|fit|stretch|center]
//
// NOTE: the shipped app bans busy loops; this harness is a dev tool and uses a
// vsync-blocked render loop so it can be driven from scripts / CI.

#include <windows.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "graphics/D3D11DeviceManager.h"
#include "graphics/D3D11Renderer.h"
#include "graphics/TextureManager.h"
#include "wallpaper/WallpaperManager.h"

using vw::gfx::D3D11DeviceManager;
using vw::gfx::D3D11Renderer;
using vw::wallpaper::WallpaperManager;

namespace {

[[noreturn]] void fail(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fwprintf(stderr, L"gfx_harness: ");
    std::vfwprintf(stderr, fmt, args);
    std::fwprintf(stderr, L"\n");
    va_end(args);
    std::exit(1);
}

void printAdapters(const std::vector<vw::gfx::AdapterInfo>& adapters) {
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& a = adapters[i];
        std::wprintf(L"adapter %zu: %s  (vendor=0x%04X device=0x%04X, dedicated VRAM=%.1f GB, "
                     L"shared sys=%.1f GB)\n",
                     i, a.description.c_str(), a.vendor, a.device,
                     a.dedicatedVram / (1024.0 * 1024 * 1024),
                     a.sharedSystem / (1024.0 * 1024 * 1024));
    }
}

void printOutputs(const std::vector<vw::gfx::OutputInfo>& outputs) {
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& o = outputs[i];
        std::wprintf(L"  output %zu: %s  [%ld,%ld - %ld,%ld]  current=%ldx%ld @%ld Hz\n", i,
                     o.deviceName.c_str(), o.left, o.top, o.right, o.bottom, o.width, o.height,
                     o.refreshHz);
    }
}

// ---- M5-preview: decode one frame to RGB32 via Media Foundation -------------

struct DecodedFrame {
    std::vector<uint8_t> bytes; // tightly packed w*4 per row
    UINT width = 0;
    UINT height = 0;
    float displayAspect = 0.0f; // SAR-corrected aspect (0 = square pixels)
};

std::wstring formatHr(HRESULT hr) {
    return std::format(L"hr=0x{:08X}", static_cast<unsigned>(hr));
}

// MF_SOURCE_READER_FIRST_VIDEO_STREAM is a signed enum constant in the SDK
// headers; DWORD parameters need an explicit cast (C4245-safe).
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// Decodes the first video frame as RGB32 (B,G,R,A byte order, matches
// DXGI_FORMAT_B8G8R8A8_UNORM so the bytes upload verbatim).
bool decodeFirstFrame(const std::wstring& path, DecodedFrame& out, std::wstring& err) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 1);
    if (SUCCEEDED(hr)) {
        // Documented YUV->RGB32 path: route conversion through the Video
        // Processor MFT rather than the decoder's own converter (the latter
        // rejects RGB32 output with MF_E_INVALIDMEDIATYPE on some codecs).
        hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) {
        hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader);
    }
    if (FAILED(hr)) {
        err = L"MFCreateSourceReaderFromURL failed: " + formatHr(hr);
        return false;
    }

    ComPtr<IMFMediaType> native;
    hr = reader->GetCurrentMediaType(kFirstVideoStream, &native);
    if (FAILED(hr)) {
        err = L"no video stream in file";
        return false;
    }
    // PROBE: report the native subtype so RGB32-negotiation failures are diagnosable.
    GUID nativeSubtype{};
    if (SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &nativeSubtype))) {
        std::wprintf(L"gfx_harness: native video subtype = {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
                     nativeSubtype.Data1, nativeSubtype.Data2, nativeSubtype.Data3,
                     nativeSubtype.Data4[0], nativeSubtype.Data4[1], nativeSubtype.Data4[2],
                     nativeSubtype.Data4[3], nativeSubtype.Data4[4], nativeSubtype.Data4[5],
                     nativeSubtype.Data4[6], nativeSubtype.Data4[7]);
    }
    UINT32 w = 0, h = 0;
    hr = ::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        err = L"could not read frame size";
        return false;
    }

    // Ask for RGB32 — the source reader inserts a color converter (CPU). The
    // real M5 path does NV12/P010 on the GPU; this preview only proves the
    // texture upload + sampling pipeline. The color converter needs the frame
    // size and sample attributes set explicitly (else MF_E_INVALIDMEDIATYPE).
    ComPtr<IMFMediaType> rgb;
    hr = ::MFCreateMediaType(&rgb);
    if (SUCCEEDED(hr)) {
        hr = rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(hr)) {
        hr = rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(hr)) {
        hr = rgb->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(hr)) {
        hr = rgb->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = rgb->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    }
    if (SUCCEEDED(hr)) {
        hr = ::MFSetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, w, h);
    }
    // The converter requires an explicit stride on the requested RGB32 type
    // (computing it from the FOURCC + width is the standard approach).
    if (SUCCEEDED(hr)) {
        LONG stride = 0;
        if (SUCCEEDED(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride)) &&
            stride > 0) {
            hr = rgb->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
        }
    }
    if (FAILED(hr)) {
        err = L"could not build RGB32 media type";
        return false;
    }
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, rgb.Get());
    if (FAILED(hr)) {
        err = L"RGB32 output unsupported for this file (codec/color-converter): " + formatHr(hr);
        return false;
    }

    ComPtr<IMFSample> sample;
    DWORD flags = 0;
    hr = reader->ReadSample(kFirstVideoStream, 0, nullptr, &flags, nullptr, &sample);
    if (FAILED(hr) || !sample) {
        err = L"ReadSample failed: " + formatHr(hr);
        return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        err = L"no frame available (end of stream)";
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        err = L"GetBufferByIndex failed: " + formatHr(hr);
        return false;
    }

    // Row pitch: prefer the 2D buffer's pitch (may exceed w*4); fall back to
    // tight packing on the plain media buffer.
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        hr = buffer2d->Lock2D(&scanline0, &pitch);
        if (FAILED(hr)) {
            err = L"Lock2D failed: " + formatHr(hr);
            return false;
        }
    } else {
        DWORD len = 0;
        hr = buffer->Lock(&scanline0, nullptr, &len);
        if (FAILED(hr)) {
            err = L"media buffer Lock failed: " + formatHr(hr);
            return false;
        }
        pitch = static_cast<LONG>(w) * 4; // assume tight packing
    }

    const size_t srcPitch = (pitch > 0 ? static_cast<size_t>(pitch)
                                       : static_cast<size_t>(w) * 4);
    out.width = w;
    out.height = h;
    out.bytes.resize(static_cast<size_t>(h) * static_cast<size_t>(w) * 4);
    for (UINT y = 0; y < h; ++y) {
        std::memcpy(out.bytes.data() + static_cast<size_t>(y) * w * 4,
                    scanline0 + static_cast<size_t>(y) * srcPitch, static_cast<size_t>(w) * 4);
    }

    if (buffer2d) {
        buffer2d->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return true;
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

} // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Debug layer is Debug-build-only (matches M2: "Release has no debug layer").
#ifdef VW_DEBUG
    bool wantDebug = true;
#else
    bool wantDebug = false;
#endif
    bool listOnly = false;
    bool wallpaperMode = false;
    UINT adapterIndex = 0;
    uint64_t maxFrames = 0;
    std::wstring videoPath;
    D3D11Renderer::Scaling videoScaling = D3D11Renderer::Scaling::Fill; // app default
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--list") listOnly = true;
        else if (a == L"--wallpaper") wallpaperMode = true;
        else if (a == L"--no-debug") wantDebug = false;
        else if (a == L"--adapter" && i + 1 < argc) adapterIndex = static_cast<UINT>(std::wcstoul(argv[++i], nullptr, 10));
        else if (a == L"--frames" && i + 1 < argc) maxFrames = std::wcstoull(argv[++i], nullptr, 10);
        else if (a == L"--video" && i + 1 < argc) videoPath = argv[++i];
        else if (a == L"--scaling" && i + 1 < argc) {
            const std::wstring s = argv[++i];
            if (s == L"fit") videoScaling = D3D11Renderer::Scaling::Fit;
            else if (s == L"stretch") videoScaling = D3D11Renderer::Scaling::Stretch;
            else if (s == L"center") videoScaling = D3D11Renderer::Scaling::Center;
            else if (s == L"fill") videoScaling = D3D11Renderer::Scaling::Fill;
            else fail(L"unknown --scaling mode: %s", s.c_str());
        }
        else fail(L"unknown argument: %s", a.c_str());
    }
    if (wallpaperMode && listOnly) fail(L"--wallpaper and --list are mutually exclusive");

    // M5 preview: Media Foundation must be started before decoding.
    bool mfStarted = false;
    if (!videoPath.empty()) {
        const HRESULT mfHr = ::MFStartup(MF_VERSION);
        if (FAILED(mfHr)) fail(L"MFStartup failed: hr=0x%08X", static_cast<unsigned>(mfHr));
        mfStarted = true;
    }

    // 1. Enumerate and log adapters + outputs (M2: "adapter/output log matches reality").
    auto adapters = D3D11DeviceManager::enumerateAdapters();
    if (!adapters) fail(L"%s", adapters.error().c_str());
    std::printf("gfx_harness: found %zu adapter(s):\n", adapters->size());
    printAdapters(*adapters);

    if (adapterIndex >= adapters->size()) {
        fail(L"--adapter %u out of range (have %zu)", adapterIndex, adapters->size());
    }
    auto adapter = D3D11DeviceManager::getAdapter(adapterIndex);
    if (!adapter) fail(L"%s", adapter.error().c_str());
    std::wprintf(L"gfx_harness: using adapter %u: %s\n", adapterIndex,
                 (*adapters)[adapterIndex].description.c_str());
    auto outputs = D3D11DeviceManager::enumerateOutputs(adapter->Get());
    if (outputs) {
        printOutputs(*outputs);
    } else {
        std::fwprintf(stderr, L"gfx_harness: %s\n", outputs.error().c_str());
    }

    if (listOnly) {
        std::printf("gfx_harness: --list done\n");
        return 0;
    }

    // M3 wallpaper mode: drive the real WallpaperManager (discovery, hosts,
    // checkerboard) from a script, without the full app. The debug layer
    // follows the build (like the app) — --no-debug does not apply here.
    if (wallpaperMode) {
        std::printf("gfx_harness: wallpaper mode (adapter %u)\n", adapterIndex);
        WallpaperManager wallpaper;
        auto startResult = wallpaper.start(adapter->Get());
        if (!startResult) {
            fail(L"wallpaper start failed: %s", startResult.error().c_str());
        }
        std::wprintf(L"gfx_harness: wallpaper running: %zu host(s), %zu monitor(s), %s\n",
                     wallpaper.hostCount(), wallpaper.monitors().size(),
                     wallpaper.layer().description.c_str());
        for (const auto& m : wallpaper.monitors()) {
            std::wprintf(L"  monitor %s: %ux%u @ %u Hz%s\n", m.id.c_str(), m.width, m.height,
                         m.refreshRateNumerator, m.primary ? L" (primary)" : L"");
        }

        uint64_t frames = 0;
        const auto tStart = std::chrono::steady_clock::now();
        auto lastTick = tStart;
        bool running = true;
        MSG msg{};
        while (running) {
            // Host windows are on this thread: pump their messages.
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) running = false;
            }
            if (!running) break;

            auto rendered = wallpaper.renderAll();
            if (!rendered) {
                std::fwprintf(stderr, L"gfx_harness: wallpaper render failed: %s\n",
                              rendered.error().c_str());
                break;
            }
            ++frames;

            // Same 1 Hz Explorer-restart validity check the app runs.
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTick >= std::chrono::seconds(1)) {
                wallpaper.onTick();
                lastTick = now;
            }

            if (maxFrames > 0 && frames >= maxFrames) {
                std::printf("gfx_harness: wallpaper reached --frames %llu, exiting cleanly\n",
                            static_cast<unsigned long long>(maxFrames));
                break;
            }
        }

        wallpaper.shutdown();
        std::printf("gfx_harness: wallpaper done (%llu frames)\n",
                    static_cast<unsigned long long>(frames));
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

    // 3. Device + swap chain + renderer (the real M2 modules).
    D3D11DeviceManager deviceManager;
    auto deviceResult = deviceManager.createDevice(adapter->Get(), wantDebug);
    if (!deviceResult) fail(L"device creation failed: %s", deviceResult.error().c_str());
    std::printf("gfx_harness: device created, feature level=0x%04X, debug layer=%s\n",
                static_cast<unsigned>(deviceManager.featureLevel()),
                deviceManager.debugLayer() ? "ON" : "off");

    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    UINT w = static_cast<UINT>(rc.right - rc.left);
    UINT h = static_cast<UINT>(rc.bottom - rc.top);

    auto swapChain = deviceManager.createSwapChain(hwnd, w, h);
    if (!swapChain) fail(L"%s", swapChain.error().c_str());

    D3D11Renderer renderer;
    auto initResult = renderer.init(deviceManager.device(), swapChain->Get(), w, h);
    if (!initResult) fail(L"renderer init failed: %s", initResult.error().c_str());

    // M5 preview: decode one frame, upload to a D3D11 texture, bind via SRV.
    bool videoMode = false;
    if (!videoPath.empty()) {
        DecodedFrame frame;
        std::wstring decodeErr;
        if (!decodeFirstFrame(videoPath, frame, decodeErr)) {
            fail(L"decode failed: %s", decodeErr.c_str());
        }
        auto texture = vw::gfx::TextureManager::createTexture(
            deviceManager.device(), DXGI_FORMAT_B8G8R8A8_UNORM, frame.width, frame.height, true);
        if (!texture) fail(L"texture creation failed: %s", texture.error().c_str());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        deviceManager.context()->Map(texture->Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        for (UINT y = 0; y < frame.height; ++y) {
            std::memcpy(static_cast<BYTE*>(mapped.pData) +
                            static_cast<size_t>(y) * mapped.RowPitch,
                        frame.bytes.data() + static_cast<size_t>(y) * frame.width * 4,
                        static_cast<size_t>(frame.width) * 4);
        }
        deviceManager.context()->Unmap(texture->Get(), 0);
        auto srv = vw::gfx::TextureManager::createSrv(deviceManager.device(), texture->Get());
        if (!srv) fail(L"SRV creation failed: %s", srv.error().c_str());
        const float videoAspect =
            vw::gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
        auto setResult = renderer.setVideoTexture(srv->Get(), videoAspect, videoScaling);
        if (!setResult) fail(L"setVideoTexture failed: %s", setResult.error().c_str());
        videoMode = true;
        std::wprintf(L"gfx_harness: video frame decoded: %ux%u RGB32 -> texture (scaling=%d, "
                     L"aspect=%.3f)\n",
                     frame.width, frame.height, static_cast<int>(videoScaling), videoAspect);
    }

    // 4. Render loop (vsync-blocked; harness-only).
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
                auto resized = renderer.resize(deviceManager.device(), w, h);
                if (!resized) {
                    std::fwprintf(stderr, L"gfx_harness: resize failed: %s\n",
                                  resized.error().c_str());
                }
            }
        }

        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
        D3D11Renderer::FrameParams params{};
        if (videoMode) {
            // True colors for the video frame — no tint pulse.
            params.tint[0] = params.tint[1] = params.tint[2] = params.tint[3] = 1.0f;
        } else {
            params.tint[0] = 0.55f + 0.45f * static_cast<float>(std::sin(t * 1.2));
            params.tint[1] = 0.55f + 0.45f * static_cast<float>(std::cos(t * 0.8));
            params.tint[2] = 0.6f;
            params.tint[3] = 1.0f;
        }

        auto rendered = renderer.render(deviceManager.context(), params);
        if (!rendered) {
            // M2 device-loss plumbing stub: log + schedule recreate, then exit.
            std::fwprintf(stderr, L"gfx_harness: render failed: %s\n", rendered.error().c_str());
            deviceManager.scheduleRecreate();
            const bool pending = deviceManager.consumeRecreateRequest();
            std::printf("gfx_harness: recreate request recorded (pending=%d) — full recreate "
                        "logic lands in M12\n", pending ? 1 : 0);
            break;
        }

        ++frames;
        fpsAccum += 1.0;
        const auto now = std::chrono::steady_clock::now();
        const double sinceReport = std::chrono::duration<double>(now - lastReport).count();
        if (sinceReport >= 1.0) {
            const double fps = fpsAccum / sinceReport;
            std::wstring title = L"Video Wallpaper — gfx harness — " + (*adapters)[adapterIndex].description +
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

    if (hwnd) ::DestroyWindow(hwnd);
    if (mfStarted) ::MFShutdown();
    std::printf("gfx_harness: done (%llu frames)\n", static_cast<unsigned long long>(frames));
    return 0;
}
