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
//
// Usage:
//   vw_gfx_harness [--list] [--adapter N] [--frames N] [--no-debug]
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
#include <cwchar>
#include <string>

#include "graphics/D3D11DeviceManager.h"
#include "graphics/D3D11Renderer.h"

using vw::gfx::D3D11DeviceManager;
using vw::gfx::D3D11Renderer;

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
        params.tint[0] = 0.55f + 0.45f * static_cast<float>(std::sin(t * 1.2));
        params.tint[1] = 0.55f + 0.45f * static_cast<float>(std::cos(t * 0.8));
        params.tint[2] = 0.6f;
        params.tint[3] = 1.0f;

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
    std::printf("gfx_harness: done (%llu frames)\n", static_cast<unsigned long long>(frames));
    return 0;
}
