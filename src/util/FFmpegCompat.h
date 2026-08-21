#pragma once

// FFmpeg compatibility layer for lazy-loaded DLLs.
// Include this INSTEAD OF direct FFmpeg headers in source files that call FFmpeg APIs.
// The macros redirect every function call through the FFmpegApi function pointer table,
// so the linker never needs to resolve FFmpeg symbols — they're resolved at runtime
// by LoadLibrary/GetProcAddress.
//
// Types (AVCodecContext, AVFrame, etc.) are still provided by the real FFmpeg headers
// included inside FFmpegApi.h. Only function *calls* are redirected.

#include "util/FFmpegApi.h"

// Shorthand: access the loaded API
#define FF vw::ffmpeg::ffmpegApi()

// ---- libavutil ----
#define av_buffer_ref               FF.buffer_ref
#define av_buffer_unref             FF.buffer_unref
#define av_frame_alloc              FF.frame_alloc
#define av_frame_free               FF.frame_free
#define av_frame_unref              FF.frame_unref
#define av_hwdevice_ctx_create      FF.hwdevice_ctx_create
#define av_hwdevice_ctx_create_derived FF.hwdevice_ctx_create_derived
#define av_hwframe_ctx_alloc        FF.hwframe_ctx_alloc
#define av_hwframe_ctx_init         FF.hwframe_ctx_init
#define av_hwframe_map              FF.hwframe_map
#define av_hwframe_transfer_data    FF.hwframe_transfer_data
#define av_packet_alloc             FF.packet_alloc
#define av_packet_free              FF.packet_free
#define av_packet_unref             FF.packet_unref
#define av_find_best_stream         FF.find_best_stream
#define av_rescale_q                FF.rescale_q
#define av_rescale_rnd              FF.rescale_rnd
#define av_strerror                 FF.strerror
#define av_get_bytes_per_sample     FF.get_bytes_per_sample

// ---- libavcodec ----
#define avcodec_alloc_context3          FF.codec_alloc_context3
#define avcodec_find_decoder            FF.codec_find_decoder
#define avcodec_find_decoder_by_name    FF.codec_find_decoder_by_name
#define avcodec_flush_buffers           FF.codec_flush_buffers
#define avcodec_free_context            FF.codec_free_context
#define avcodec_get_name                FF.codec_get_name
#define avcodec_open2                   FF.codec_open2
#define avcodec_parameters_to_context   FF.codec_parameters_to_context
#define avcodec_receive_frame           FF.codec_receive_frame
#define avcodec_send_packet             FF.codec_send_packet

// ---- libavformat ----
#define avformat_close_input        FF.format_close_input
#define avformat_find_stream_info   FF.format_find_stream_info
#define avformat_open_input         FF.format_open_input
#define avformat_seek_file          FF.format_seek_file
#define av_read_frame               FF.read_frame

// ---- libswresample ----
#define swr_alloc_set_opts2         FF.swr_alloc_set_opts2
#define swr_init                    FF.swr_init
#define swr_free                    FF.swr_free
#define swr_convert                 FF.swr_convert
#define swr_get_delay               FF.swr_get_delay

// ---- libswscale ----
#define sws_getContext               FF.sws_getContext
#define sws_scale                    FF.sws_scale
#define sws_freeContext              FF.sws_freeContext
