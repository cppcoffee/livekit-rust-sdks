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

// Media Foundation VideoDecoder implementation for WebRTC.

#ifndef MF_VIDEO_DECODER_IMPL_H_
#define MF_VIDEO_DECODER_IMPL_H_

#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/sdp_video_format.h"
#include "common_video/include/video_frame_buffer_pool.h"

#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>

namespace webrtc {

class MFVideoDecoderImpl : public VideoDecoder {
 public:
  explicit MFVideoDecoderImpl(const SdpVideoFormat& format);
  ~MFVideoDecoderImpl() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  bool InitMFT(uint32_t width, uint32_t height);
  bool HandleStreamChange();
  bool DrainOutput(uint32_t rtp_timestamp);

  SdpVideoFormat format_;
  DecodedImageCallback* callback_ = nullptr;
  IMFTransform* transform_ = nullptr;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;
  VideoFrameBufferPool buffer_pool_;
};

}  // namespace webrtc

#endif  // MF_VIDEO_DECODER_IMPL_H_
