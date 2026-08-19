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

    // H.264/HEVC: prefer MF (proven zero-copy), then NVDEC, then software
    if (isH264Hevc && renderDevice) {
        log.info(L"decoder factory: trying Media Foundation for {}", codec);
        auto mfDecoder = std::make_unique<DecoderManager>();
        mfDecoder->setD3DDevice(renderDevice);
        auto result = mfDecoder->open(path);
        if (result) {
            log.info(L"decoder factory: Media Foundation succeeded ({})", mfDecoder->decoderName());
            if (selectedAdapter) {
                selectedAdapter->name = L"Media Foundation";
                selectedAdapter->isRenderDevice = true;
            }
            return mfDecoder;
        }
        log.info(L"decoder factory: Media Foundation failed ({}), trying NVDEC", result.error());
    }

    // VP9/AV1 or MF failure: try NVDEC via FFmpeg
    {
        log.info(L"decoder factory: trying FFmpeg NVDEC for {}", codec);
        auto ffmpegDecoder = std::make_unique<FFmpegDecoder>();
        ffmpegDecoder->setD3DDevice(renderDevice);
        auto result = ffmpegDecoder->open(path);
        if (result) {
            if (ffmpegDecoder->isHardwareDecoding()) {
                log.info(L"decoder factory: FFmpeg HW decode succeeded ({})", ffmpegDecoder->decoderName());
                if (selectedAdapter) {
                    selectedAdapter->name = ffmpegDecoder->decoderName();
                    selectedAdapter->hasNvdec = true;
                }
            } else {
                log.info(L"decoder factory: FFmpeg software decode ({})", ffmpegDecoder->decoderName());
                if (selectedAdapter) {
                    selectedAdapter->name = L"FFmpeg software";
                }
            }
            return ffmpegDecoder;
        }
        log.info(L"decoder factory: FFmpeg failed ({}), trying pure software", result.error());
    }

    // Final fallback: FFmpeg software only (no HW device)
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
