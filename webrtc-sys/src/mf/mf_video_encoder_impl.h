/*
 * Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Media Foundation VideoEncoder implementation for WebRTC.

#ifndef MF_VIDEO_ENCODER_IMPL_H_
#define MF_VIDEO_ENCODER_IMPL_H_

#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"

#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>

namespace webrtc {

class MFVideoEncoderImpl : public VideoEncoder {
 public:
  MFVideoEncoderImpl(const Environment& env, const SdpVideoFormat& format);
  ~MFVideoEncoderImpl() override;

  int InitEncode(const VideoCodec* codec_settings,
                 const Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  bool InitMFT(uint32_t width, uint32_t height, uint32_t bitrate,
               uint32_t framerate);
  bool EncodeFrame(const VideoFrame& frame, bool force_key);
  void DrainOutput();

  SdpVideoFormat format_;
  EncodedImageCallback* callback_ = nullptr;
  IMFTransform* transform_ = nullptr;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t bitrate_bps_ = 0;
  uint32_t framerate_ = 0;
  bool initialized_ = false;
  int64_t frame_count_ = 0;
};

}  // namespace webrtc

#endif  // MF_VIDEO_ENCODER_IMPL_H_
