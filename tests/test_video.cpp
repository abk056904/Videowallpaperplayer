#include "doctest.h"
#include <cstdio>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include "graphics/TextureManager.h"
#include "video/DecoderManager.h"
#include "video/FrameQueue.h"
#include "video/VideoMetadata.h"

namespace {

// The Source Reader needs Media Foundation started. MFCreateMediaType alone
// does not, but the decode tests do — one scope per test that needs it.
struct MfScope {
    bool ok = false;
    MfScope() { ok = SUCCEEDED(::MFStartup(MF_VERSION)); }
    ~MfScope() {
        if (ok) {
            ::MFShutdown();
        }
    }
};

// Real decode tests need a clip on disk (this dev machine has a clips
// folder). No clips -> the test reports skipped, not failed.
std::filesystem::path findTestClip() {
    const std::filesystem::path dir = L"C:/Users/mbk43/Videos/bgcmp";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return {};
    }
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == L".mp4") {
            return e.path();
        }
    }
    return {};
}

} // namespace

TEST_CASE("VideoMetadata subtype mapping") {
    using vw::video::VideoMetadata;
    CHECK(VideoMetadata::subtypeName(MFVideoFormat_H264) == L"H.264");
    CHECK(VideoMetadata::subtypeName(MFVideoFormat_HEVC) == L"HEVC");
    CHECK(VideoMetadata::subtypeName(MFVideoFormat_AV1) == L"AV1");
    CHECK(VideoMetadata::subtypeName(MFVideoFormat_NV12) == L"NV12");
    CHECK(VideoMetadata::subtypeName(MFVideoFormat_P010) == L"P010");
    CHECK(VideoMetadata::subtypeName(MFVideoFormat_RGB32) == L"RGB32");
    // Unknown GUIDs never crash and map to a stable string.
    GUID mystery{0x12345678, 0xabcd, 0xef01, {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}};
    CHECK(VideoMetadata::subtypeName(mystery) == L"unknown");

    CHECK(VideoMetadata::bitDepthFromSubtype(MFVideoFormat_NV12) == 8);
    CHECK(VideoMetadata::bitDepthFromSubtype(MFVideoFormat_H264) == 8);
    CHECK(VideoMetadata::bitDepthFromSubtype(MFVideoFormat_P010) == 10);
    CHECK(VideoMetadata::bitDepthFromSubtype(MFVideoFormat_P016) == 10);
}

TEST_CASE("VideoMetadata::fillFromMediaType with synthetic types") {
    using vw::video::VideoMetadata;

    SUBCASE("H.264 1920x1080 @ 30000/1001, 8-bit SDR") {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        REQUIRE(SUCCEEDED(::MFCreateMediaType(&type)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)));
        REQUIRE(SUCCEEDED(::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, 1920, 1080)));
        REQUIRE(SUCCEEDED(::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 30000, 1001)));

        VideoMetadata meta;
        REQUIRE(VideoMetadata::fillFromMediaType(type.Get(), meta));
        CHECK(meta.codec == L"H.264");
        CHECK(meta.width == 1920);
        CHECK(meta.height == 1080);
        CHECK(meta.fps == doctest::Approx(30000.0 / 1001.0));
        CHECK(meta.bitDepth == 8); // inferred from subtype
        CHECK_FALSE(meta.hdr);
    }

    SUBCASE("P010 10-bit HDR (BT.2020 primaries + PQ transfer)") {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        REQUIRE(SUCCEEDED(::MFCreateMediaType(&type)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_P010)));
        REQUIRE(SUCCEEDED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020)));
        REQUIRE(SUCCEEDED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084)));

        VideoMetadata meta;
        REQUIRE(VideoMetadata::fillFromMediaType(type.Get(), meta));
        CHECK(meta.codec == L"P010");
        CHECK(meta.bitDepth == 10);
        CHECK(meta.hdr);
    }

    SUBCASE("HDR signalled by transfer function alone (HLG on an 8-bit H.264 type)") {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        REQUIRE(SUCCEEDED(::MFCreateMediaType(&type)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)));
        REQUIRE(SUCCEEDED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_HLG)));

        VideoMetadata meta;
        REQUIRE(VideoMetadata::fillFromMediaType(type.Get(), meta));
        CHECK(meta.bitDepth == 8); // inferred from the H.264 subtype
        CHECK(meta.hdr);           // ...but the HLG transfer function marks it HDR
    }

    SUBCASE("Missing frame size is zeroed, not an error") {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        REQUIRE(SUCCEEDED(::MFCreateMediaType(&type)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)));
        REQUIRE(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)));

        VideoMetadata meta;
        REQUIRE(VideoMetadata::fillFromMediaType(type.Get(), meta));
        CHECK(meta.width == 0);
        CHECK(meta.height == 0);
        CHECK(meta.codec == L"H.264");
    }

    SUBCASE("Null media type is a graceful error") {
        VideoMetadata meta;
        CHECK_FALSE(VideoMetadata::fillFromMediaType(nullptr, meta));
    }
}

TEST_CASE("FrameQueue capacity and blocking semantics") {
    using vw::video::DecodedFrame;
    using vw::video::FrameQueue;
    FrameQueue q(2);
    CHECK(q.capacity() == 2);
    CHECK(q.size() == 0);

    DecodedFrame f;
    f.width = 4;
    f.height = 4;
    f.bytes.resize(64, 0xAB);
    CHECK(q.tryPush(f));
    CHECK(q.tryPush(f));
    CHECK_FALSE(q.tryPush(f)); // full

    DecodedFrame out;
    CHECK(q.tryPop(out));
    CHECK(out.width == 4);
    CHECK(out.bytes.size() == 64);
    CHECK(q.size() == 1);

    q.clear();
    CHECK(q.size() == 0);
    CHECK_FALSE(q.tryPop(out)); // empty after clear
}

TEST_CASE("FrameQueue close unblocks a blocked push") {
    using vw::video::DecodedFrame;
    using vw::video::FrameQueue;
    FrameQueue q(1);
    DecodedFrame f;
    f.bytes.resize(4);
    q.push(f); // full now

    std::thread producer([&] {
        DecodedFrame g;
        g.bytes.resize(4);
        q.push(g); // blocks until close() (or a pop) — must not hang forever
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.close();
    producer.join();
    // The blocked push was rejected (returned false), not enqueued.
    CHECK(q.size() == 1);
    // Closed queue rejects further pushes.
    CHECK_FALSE(q.tryPush(f));
}

TEST_CASE("FrameQueue producer/consumer hand-off across threads") {
    using vw::video::DecodedFrame;
    using vw::video::FrameQueue;
    FrameQueue q(3);

    std::thread producer([&] {
        for (int i = 0; i < 50; ++i) {
            DecodedFrame f;
            f.timestamp = i;
            f.bytes.resize(4);
            q.push(std::move(f));
        }
    });

    int got = 0;
    long long last = -1;
    while (got < 50) {
        DecodedFrame out;
        if (q.tryPop(out)) {
            CHECK(out.timestamp > last); // FIFO order preserved
            last = out.timestamp;
            ++got;
        } else {
            std::this_thread::yield(); // wait for the producer
        }
    }
    producer.join();
    CHECK(got == 50);
}

TEST_CASE("DecoderManager opens a real file and decodes a frame") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping real-file test");
        return;
    }
    const auto clip = findTestClip();
    if (clip.empty()) {
        MESSAGE("no test clips found — skipping real-file decode test");
        return;
    }

    vw::video::DecoderManager dec;
    auto opened = dec.open(clip.wstring());
    REQUIRE(opened);

    // Real metadata, not the file extension.
    const auto& meta = dec.metadata();
    CHECK(meta.width > 0);
    CHECK(meta.height > 0);
    CHECK(meta.fps > 0.0);
    CHECK_FALSE(meta.codec.empty());
    CHECK(meta.duration100ns > 0);

    // Decode one frame and verify it matches the metadata dimensions.
    vw::video::FrameQueue q(2);
    REQUIRE(dec.start(&q));
    vw::video::DecodedFrame frame;
    bool gotFrame = false;
    for (int i = 0; i < 200 && !gotFrame; ++i) {
        if (q.tryPop(frame)) {
            if (frame.endOfStream) {
                break; // shouldn't happen this early, but don't hang the test
            }
            gotFrame = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(gotFrame);
    if (gotFrame) {
        CHECK(frame.width == meta.width);
        CHECK(frame.height == meta.height);
        CHECK(frame.bytes.size() ==
              static_cast<size_t>(meta.width) * static_cast<size_t>(meta.height) * 4);
    }
    dec.stop();
}

TEST_CASE("DecoderManager hardware path: GPU surfaces or clean fallback") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping hardware test");
        return;
    }
    const auto clip = findTestClip();
    if (clip.empty()) {
        MESSAGE("no test clips found — skipping hardware test");
        return;
    }

    // A real D3D11 device on the default adapter (hardware decode target).
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    // VIDEO_SUPPORT is required for the MF hardware decode path (the app's
    // D3D11DeviceManager includes it — mirror it here).
    const HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                                           &context);
    if (FAILED(hr)) {
        MESSAGE("no D3D11 hardware device available — skipping hardware test");
        return;
    }

    vw::video::DecoderManager dec;
    dec.setD3DDevice(device.Get());
    auto opened = dec.open(clip.wstring());
    if (!opened) {
        std::fprintf(stderr, "[hw] open failed: %ls\n", opened.error().c_str());
    }
    REQUIRE(opened);

    vw::video::FrameQueue q(2);
    REQUIRE(dec.start(&q));
    vw::video::DecodedFrame frame;
    bool gotFrame = false;
    for (int i = 0; i < 400 && !gotFrame; ++i) {
        if (q.tryPop(frame)) {
            if (frame.endOfStream) {
                break;
            }
            gotFrame = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    CHECK(gotFrame);
    if (gotFrame) {
        if (frame.hardware) {
            // GPU surface path: the frame IS the decoder's planar texture.
            REQUIRE(frame.texture);
            D3D11_TEXTURE2D_DESC desc{};
            frame.texture->GetDesc(&desc);
            CHECK((desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010));
            CHECK(desc.Width == frame.width);
            CHECK(desc.Height == frame.height);
            // The documented two-views pattern must produce legal SRVs.
            const bool p010 = desc.Format == DXGI_FORMAT_P010;
            auto y = vw::gfx::TextureManager::createPlaneSrv(
                device.Get(), frame.texture.Get(),
                p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM);
            CHECK(y);
            auto uv = vw::gfx::TextureManager::createPlaneSrv(
                device.Get(), frame.texture.Get(),
                p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM);
            CHECK(uv);
            // The decoder name is read from the registry for the active MFT's
            // CLSID — must be present and never fabricated.
            CHECK_FALSE(dec.decoderName().empty());
        } else {
            // Clean software fallback is acceptable on machines without HW.
            CHECK_FALSE(frame.bytes.empty());
            CHECK(frame.width > 0);
        }
    }
    dec.stop();
}

TEST_CASE("DecoderManager fails gracefully on a corrupt file") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping corrupt-file test");
        return;
    }
    const auto path = std::filesystem::temp_directory_path() / L"vw_corrupt_test.bin";
    {
        std::ofstream out(path, std::ios::binary);
        const char junk[] = "this is definitely not a media file\x00\x01\x02\xff\xfe\x00\x00";
        out.write(junk, static_cast<std::streamsize>(sizeof(junk)));
    }

    vw::video::DecoderManager dec;
    auto opened = dec.open(path.wstring());
    // Graceful error Result — no crash, no hang, with a diagnostic attached.
    REQUIRE_FALSE(opened);
    CHECK_FALSE(opened.error().empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
