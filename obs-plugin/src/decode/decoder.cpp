// SPDX-License-Identifier: GPL-2.0-or-later
#include "decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace rac {

VideoDecoder::~VideoDecoder()
{
	close();
}

bool VideoDecoder::open(const std::string &codec)
{
	close();
	const AVCodec *c = avcodec_find_decoder(codec == "hevc" ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	if (!c)
		return false;
	ctx_ = avcodec_alloc_context3(c);
	if (!ctx_)
		return false;
	// Low latency: slice threads only (frame threads add a frame of delay per thread).
	ctx_->thread_type = FF_THREAD_SLICE;
	ctx_->thread_count = 0;
	ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
	if (avcodec_open2(ctx_, c, nullptr) < 0) {
		avcodec_free_context(&ctx_);
		return false;
	}
	frame_ = av_frame_alloc();
	pkt_ = av_packet_alloc();
	return true;
}

void VideoDecoder::close()
{
	if (sws_)
		sws_freeContext(sws_);
	if (swsSmall_)
		sws_freeContext(swsSmall_);
	sws_ = swsSmall_ = nullptr;
	swsW_ = swsH_ = smallW_ = 0;
	swsFmt_ = -1;
	if (pkt_)
		av_packet_free(&pkt_);
	if (frame_)
		av_frame_free(&frame_);
	if (ctx_)
		avcodec_free_context(&ctx_);
}

bool VideoDecoder::decode(const uint8_t *data, size_t size, Picture &out, Picture *small, int smallWidth)
{
	if (!ctx_)
		return false;
	if (av_new_packet(pkt_, int(size)) < 0)
		return false;
	memcpy(pkt_->data, data, size);
	int r = avcodec_send_packet(ctx_, pkt_);
	av_packet_unref(pkt_);
	if (r < 0 && r != AVERROR(EAGAIN))
		return false;
	bool got = false;
	while (avcodec_receive_frame(ctx_, frame_) == 0) {
		const int w = frame_->width, h = frame_->height;
		if (w <= 0 || h <= 0)
			continue;
		if (!sws_ || w != swsW_ || h != swsH_ || frame_->format != swsFmt_) {
			if (sws_)
				sws_freeContext(sws_);
			if (swsSmall_)
				sws_freeContext(swsSmall_);
			swsSmall_ = nullptr;
			sws_ = sws_getContext(w, h, AVPixelFormat(frame_->format), w, h, AV_PIX_FMT_BGRA, SWS_POINT, nullptr,
					      nullptr, nullptr);
			swsW_ = w;
			swsH_ = h;
			swsFmt_ = frame_->format;
			smallW_ = 0;
		}
		if (!sws_)
			continue;
		out.width = w;
		out.height = h;
		out.linesize = w * 4;
		out.bgra.resize(size_t(out.linesize) * h);
		uint8_t *dst[4] = {out.bgra.data(), nullptr, nullptr, nullptr};
		int dstStride[4] = {out.linesize, 0, 0, 0};
		sws_scale(sws_, frame_->data, frame_->linesize, 0, h, dst, dstStride);

		if (small && smallWidth > 0) {
			int sw = std::min(smallWidth, w);
			int sh = std::max(2, int(int64_t(h) * sw / w) & ~1);
			if (!swsSmall_ || smallW_ != sw) {
				if (swsSmall_)
					sws_freeContext(swsSmall_);
				swsSmall_ = sws_getContext(w, h, AVPixelFormat(frame_->format), sw, sh, AV_PIX_FMT_BGRA,
							   SWS_BILINEAR, nullptr, nullptr, nullptr);
				smallW_ = sw;
			}
			if (swsSmall_) {
				small->width = sw;
				small->height = sh;
				small->linesize = sw * 4;
				small->bgra.resize(size_t(small->linesize) * sh);
				uint8_t *sd[4] = {small->bgra.data(), nullptr, nullptr, nullptr};
				int ss[4] = {small->linesize, 0, 0, 0};
				sws_scale(swsSmall_, frame_->data, frame_->linesize, 0, h, sd, ss);
			}
		}
		got = true;
	}
	return got;
}

AudioDecoder::~AudioDecoder()
{
	close();
}

bool AudioDecoder::open(const uint8_t *config, size_t size, int rate, int channels)
{
	close();
	const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_AAC);
	if (!c)
		return false;
	ctx_ = avcodec_alloc_context3(c);
	if (!ctx_)
		return false;
	ctx_->sample_rate = rate;
	av_channel_layout_default(&ctx_->ch_layout, channels);
	if (config && size) {
		ctx_->extradata = (uint8_t *)av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(ctx_->extradata, config, size);
		ctx_->extradata_size = int(size);
	}
	if (avcodec_open2(ctx_, c, nullptr) < 0) {
		avcodec_free_context(&ctx_);
		return false;
	}
	frame_ = av_frame_alloc();
	pkt_ = av_packet_alloc();
	return true;
}

void AudioDecoder::close()
{
	if (pkt_)
		av_packet_free(&pkt_);
	if (frame_)
		av_frame_free(&frame_);
	if (ctx_)
		avcodec_free_context(&ctx_);
}

void AudioDecoder::decode(const uint8_t *data, size_t size,
			  const std::function<void(const float *const *, int, int, int)> &sink)
{
	if (!ctx_ || av_new_packet(pkt_, int(size)) < 0)
		return;
	memcpy(pkt_->data, data, size);
	int r = avcodec_send_packet(ctx_, pkt_);
	av_packet_unref(pkt_);
	if (r < 0)
		return;
	while (avcodec_receive_frame(ctx_, frame_) == 0) {
		if (frame_->format != AV_SAMPLE_FMT_FLTP)
			continue;
		sink(reinterpret_cast<const float *const *>(frame_->extended_data), frame_->ch_layout.nb_channels,
		     frame_->nb_samples, frame_->sample_rate);
	}
}

} // namespace rac
