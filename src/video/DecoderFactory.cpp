#include "video/DecoderFactory.h"

#include "logging/Logger.h"
#include "video/DecoderManager.h"
#include "video/FFmpegDecoder.h"

namespace vw::video {

std::unique_ptr<IVideoDecoder> CreateBestDecoder(
    ID3D11Device* renderDevice,
    const std::wstring& path,
    AdapterInfo* selectedAdapter)
{
    auto& log = log::Logger::instance();

    // Probe the file to determine codec
    auto meta = FFmpegDecoder::probeMetadata(path);
    if (!meta) {
        log.warn(L"decoder factory: cannot probe file: {}", meta.error());
        return nullptr;
    }

    const std::wstring& codec = meta->codec;
    log.info(L"decoder factory: codec = {}, {}x{} @ {:.1f} fps", codec, meta->width, meta->height, meta->fps);

    // Codec-aware selection
    bool isH264Hevc = (codec == L"H.264" || codec == L"HEVC" || codec == L"h264" || codec == L"hevc");
    // VP9/AV1: NVDEC is the only HW option, no MF support
    // (isVp9Av1 handled implicitly by falling through to FFmpeg path)

    // --- Step 1: try MF hardware for H.264/HEVC (proven zero-copy path) ---
    std::unique_ptr<DecoderManager> mfSoftwareFallback;
    if (isH264Hevc && renderDevice) {
        log.info(L"decoder factory: trying Media Foundation HW for {}", codec);
        auto mfDecoder = std::make_unique<DecoderManager>();
        mfDecoder->setD3DDevice(renderDevice);
        auto result = mfDecoder->open(path);
        if (result && mfDecoder->isHardwareDecoding()) {
            log.info(L"decoder factory: Media Foundation HW succeeded ({})", mfDecoder->decoderName());
            if (selectedAdapter) {
                selectedAdapter->name = L"Media Foundation";
                selectedAdapter->isRenderDevice = true;
            }
            return mfDecoder;
        }
        if (result && !mfDecoder->isHardwareDecoding()) {
            // MF opened but fell back to software — save as last resort,
            // but still try FFmpeg HW decoders (D3D11VA/CUDA) first.
            log.info(L"decoder factory: MF software fallback ({}), trying FFmpeg HW first",
                     mfDecoder->decoderName());
            mfSoftwareFallback = std::move(mfDecoder);
        } else {
            log.info(L"decoder factory: MF HW failed ({}), trying FFmpeg", result.error());
        }
    }

    // --- Step 2: try FFmpeg with HW acceleration (D3D11VA / CUDA) ---
    {
        log.info(L"decoder factory: trying FFmpeg HW (D3D11VA/CUDA) for {}", codec);
        auto ffmpegDecoder = std::make_unique<FFmpegDecoder>();
        ffmpegDecoder->setD3DDevice(renderDevice);
        auto result = ffmpegDecoder->open(path);
        if (result && ffmpegDecoder->isHardwareDecoding()) {
            log.info(L"decoder factory: FFmpeg HW decode succeeded ({})", ffmpegDecoder->decoderName());
            if (selectedAdapter) {
                selectedAdapter->name = ffmpegDecoder->decoderName();
                selectedAdapter->hasNvdec = true;
            }
            return ffmpegDecoder;
        }
        if (result) {
            log.info(L"decoder factory: FFmpeg fell back to software ({})", ffmpegDecoder->decoderName());
        } else {
            log.info(L"decoder factory: FFmpeg HW failed ({}), trying software", result.error());
        }
    }

    // --- Step 3: accept MF software fallback if available ---
    if (mfSoftwareFallback) {
        log.info(L"decoder factory: using MF software fallback ({})", mfSoftwareFallback->decoderName());
        if (selectedAdapter) {
            selectedAdapter->name = mfSoftwareFallback->decoderName();
            selectedAdapter->isRenderDevice = true;
        }
        return mfSoftwareFallback;
    }

    // --- Step 4: FFmpeg software (no HW device) ---
    {
        log.info(L"decoder factory: trying FFmpeg software-only for {}", codec);
        auto swDecoder = std::make_unique<FFmpegDecoder>();
        // No D3D device set → software only
        auto result = swDecoder->open(path);
        if (result) {
            log.info(L"decoder factory: FFmpeg software succeeded ({})", swDecoder->decoderName());
            if (selectedAdapter) {
                selectedAdapter->name = L"FFmpeg software";
            }
            return swDecoder;
        }
        log.warn(L"decoder factory: all backends failed for {}", path);
    }

    return nullptr;
}

} // namespace vw::video
