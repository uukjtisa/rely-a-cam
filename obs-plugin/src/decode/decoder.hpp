// FFmpeg decoding for one phone: H.264/HEVC video to BGRA, AAC audio to float planar.
// FFmpeg's DLLs are the ones OBS ships; the plugin links their import libraries from obs-deps.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace rac {

/** One decoded picture, BGRA, straight off the sensor (not yet turned upright). */
struct Picture {
	int width = 0, height = 0, linesize = 0;
	std::vector<uint8_t> bgra;
};

class VideoDecoder {
public:
	~VideoDecoder();
	/** "h264" or "hevc". Returns false when FFmpeg has no such decoder. */
	bool open(const std::string &codec);
	/**
	 * Decodes one Annex B access unit. On a new picture fills [out] (full size) and, when
	 * [small] is not null, a copy scaled to [smallWidth] wide for the dock preview.
	 */
	bool decode(const uint8_t *data, size_t size, Picture &out, Picture *small, int smallWidth);
	void close();
	bool isOpen() const { return ctx_ != nullptr; }

private:
	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *pkt_ = nullptr;
	SwsContext *sws_ = nullptr;
	SwsContext *swsSmall_ = nullptr;
	int swsW_ = 0, swsH_ = 0, swsFmt_ = -1, smallW_ = 0;
};

class AudioDecoder {
public:
	~AudioDecoder();
	/** Opens AAC with the AudioSpecificConfig from the phone's config frame. */
	bool open(const uint8_t *config, size_t size, int rate, int channels);
	/** Decodes one AAC frame; calls [sink] with planar float channels. */
	void decode(const uint8_t *data, size_t size,
		    const std::function<void(const float *const *planes, int channels, int frames, int rate)> &sink);
	void close();
	bool isOpen() const { return ctx_ != nullptr; }

private:
	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *pkt_ = nullptr;
};

} // namespace rac
