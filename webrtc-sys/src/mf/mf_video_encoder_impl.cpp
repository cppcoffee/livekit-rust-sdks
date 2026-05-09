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

// Media Foundation VideoEncoder implementation.

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>

#include "mf_video_encoder_impl.h"

#include "api/video/i420_buffer.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace webrtc {

static const GUID kMFVideoFormat_NV12 = {
    0x3231564E, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_H264 = {
    0x34363248, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_HEVC = {
    0x43564548, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

MFVideoEncoderImpl::MFVideoEncoderImpl(const Environment& env,
                                       const SdpVideoFormat& format)
    : format_(format) {}

MFVideoEncoderImpl::~MFVideoEncoderImpl() {
  Release();
}

int MFVideoEncoderImpl::InitEncode(const VideoCodec* codec_settings,
                                   const Settings& settings) {
  if (!codec_settings) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

  width_ = codec_settings->width;
  height_ = codec_settings->height;
  bitrate_bps_ = codec_settings->startBitrate * 1000;
  framerate_ = codec_settings->maxFramerate;

  if (!InitMFT(width_, height_, bitrate_bps_, framerate_)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  initialized_ = true;
  frame_count_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFVideoEncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFVideoEncoderImpl::Release() {
  if (transform_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    transform_->Release();
    transform_ = nullptr;
  }
  initialized_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFVideoEncoderImpl::Encode(
    const VideoFrame& frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!initialized_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  bool force_key = false;
  if (frame_types) {
    for (auto ft : *frame_types) {
      if (ft == VideoFrameType::kVideoFrameKey) {
        force_key = true;
        break;
      }
    }
  }

  if (!EncodeFrame(frame, force_key)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  DrainOutput();
  return WEBRTC_VIDEO_CODEC_OK;
}

void MFVideoEncoderImpl::SetRates(const RateControlParameters& parameters) {
  bitrate_bps_ = parameters.bitrate.get_sum_bps();
  framerate_ = static_cast<uint32_t>(parameters.framerate_fps);

  // Dynamically update bitrate via ICodecAPI if available
  if (transform_) {
    ICodecAPI* codec_api = nullptr;
    if (SUCCEEDED(transform_->QueryInterface(IID_ICodecAPI,
                                             reinterpret_cast<void**>(&codec_api)))) {
      VARIANT var = {};
      var.vt = VT_UI4;
      var.ulVal = bitrate_bps_;
      codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
      codec_api->Release();
    }
  }
}

VideoEncoder::EncoderInfo MFVideoEncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.implementation_name = "MediaFoundation";
  info.is_hardware_accelerated = true;
  info.supports_native_handle = false;
  return info;
}

// ---------------------------------------------------------------------------
// Private implementation
// ---------------------------------------------------------------------------

bool MFVideoEncoderImpl::InitMFT(uint32_t width, uint32_t height,
                                  uint32_t bitrate, uint32_t framerate) {
  GUID output_subtype = (format_.name == "H265" || format_.name == "HEVC")
                            ? kMFVideoFormat_HEVC
                            : kMFVideoFormat_H264;

  // Enumerate hardware encoder MFTs
  MFT_REGISTER_TYPE_INFO output_type = {};
  output_type.guidMajorType = MFMediaType_Video;
  output_type.guidSubtype = output_subtype;

  IMFActivate** pp_activate = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
      nullptr, &output_type, &pp_activate, &count);

  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_ERROR) << "MF: No hardware encoder found for " << format_.name;
    return false;
  }

  // Activate first available MFT
  hr = pp_activate[0]->ActivateObject(IID_IMFTransform,
                                       reinterpret_cast<void**>(&transform_));
  for (UINT32 i = 0; i < count; i++) pp_activate[i]->Release();
  CoTaskMemFree(pp_activate);

  if (FAILED(hr) || !transform_) {
    RTC_LOG(LS_ERROR) << "MF: Failed to activate encoder MFT";
    return false;
  }

  // Get stream IDs
  DWORD in_ids[1], out_ids[1];
  hr = transform_->GetStreamIDs(1, in_ids, 1, out_ids);
  if (SUCCEEDED(hr)) {
    input_stream_id_ = in_ids[0];
    output_stream_id_ = out_ids[0];
  } else {
    input_stream_id_ = 0;
    output_stream_id_ = 0;
  }

  // Set output type
  IMFMediaType* out_mt = nullptr;
  MFCreateMediaType(&out_mt);
  out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_mt->SetGUID(MF_MT_SUBTYPE, output_subtype);
  out_mt->SetUINT64(MF_MT_FRAME_SIZE,
                    (static_cast<UINT64>(width) << 32) | height);
  out_mt->SetUINT64(MF_MT_FRAME_RATE,
                    (static_cast<UINT64>(framerate) << 32) | 1);
  out_mt->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
  out_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr = transform_->SetOutputType(output_stream_id_, out_mt, 0);
  out_mt->Release();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MF: SetOutputType failed: 0x" << std::hex << hr;
    return false;
  }

  // Set input type (NV12)
  IMFMediaType* in_mt = nullptr;
  MFCreateMediaType(&in_mt);
  in_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_mt->SetGUID(MF_MT_SUBTYPE, kMFVideoFormat_NV12);
  in_mt->SetUINT64(MF_MT_FRAME_SIZE,
                   (static_cast<UINT64>(width) << 32) | height);
  in_mt->SetUINT64(MF_MT_FRAME_RATE,
                   (static_cast<UINT64>(framerate) << 32) | 1);
  in_mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr = transform_->SetInputType(input_stream_id_, in_mt, 0);
  in_mt->Release();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MF: SetInputType failed: 0x" << std::hex << hr;
    return false;
  }

  // Configure low-latency mode via ICodecAPI
  ICodecAPI* codec_api = nullptr;
  if (SUCCEEDED(transform_->QueryInterface(IID_ICodecAPI,
                                           reinterpret_cast<void**>(&codec_api)))) {
    VARIANT var = {};
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
    codec_api->Release();
  }

  // Begin streaming
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  RTC_LOG(LS_INFO) << "MF: Encoder initialized " << width << "x" << height
                   << " @ " << framerate << "fps, " << bitrate / 1000 << "kbps";
  return true;
}

bool MFVideoEncoderImpl::EncodeFrame(const VideoFrame& frame, bool force_key) {
  // Request keyframe if needed
  if (force_key) {
    ICodecAPI* codec_api = nullptr;
    if (SUCCEEDED(transform_->QueryInterface(IID_ICodecAPI,
                                             reinterpret_cast<void**>(&codec_api)))) {
      VARIANT var = {};
      var.vt = VT_UI4;
      var.ulVal = 1;
      codec_api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
      codec_api->Release();
    }
  }

  // Convert I420 to NV12 for MFT input
  webrtc::scoped_refptr<I420BufferInterface> i420_buf =
      frame.video_frame_buffer()->ToI420();
  uint32_t nv12_size = width_ * height_ * 3 / 2;

  IMFSample* sample = nullptr;
  IMFMediaBuffer* buffer = nullptr;
  MFCreateSample(&sample);
  MFCreateMemoryBuffer(nv12_size, &buffer);

  BYTE* buf_ptr = nullptr;
  buffer->Lock(&buf_ptr, nullptr, nullptr);

  // Copy Y plane
  const uint8_t* src_y = i420_buf->DataY();
  int src_stride_y = i420_buf->StrideY();
  for (uint32_t row = 0; row < height_; row++) {
    memcpy(buf_ptr + row * width_, src_y + row * src_stride_y, width_);
  }

  // Interleave U and V into NV12 UV plane
  const uint8_t* src_u = i420_buf->DataU();
  const uint8_t* src_v = i420_buf->DataV();
  int src_stride_u = i420_buf->StrideU();
  int src_stride_v = i420_buf->StrideV();
  uint8_t* dst_uv = buf_ptr + width_ * height_;
  uint32_t uv_height = height_ / 2;
  uint32_t uv_width = width_ / 2;

  for (uint32_t row = 0; row < uv_height; row++) {
    for (uint32_t col = 0; col < uv_width; col++) {
      dst_uv[row * width_ + col * 2] = src_u[row * src_stride_u + col];
      dst_uv[row * width_ + col * 2 + 1] = src_v[row * src_stride_v + col];
    }
  }

  buffer->Unlock();
  buffer->SetCurrentLength(nv12_size);
  sample->AddBuffer(buffer);
  buffer->Release();

  // Set timestamps
  int64_t sample_time = frame_count_ * 10000000LL / framerate_;
  int64_t duration = 10000000LL / framerate_;
  sample->SetSampleTime(sample_time);
  sample->SetSampleDuration(duration);

  HRESULT hr = transform_->ProcessInput(input_stream_id_, sample, 0);
  sample->Release();

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MF: ProcessInput failed: 0x" << std::hex << hr;
    return false;
  }

  frame_count_++;
  return true;
}

void MFVideoEncoderImpl::DrainOutput() {
  while (true) {
    IMFSample* out_sample = nullptr;
    IMFMediaBuffer* out_buffer = nullptr;
    MFCreateSample(&out_sample);
    MFCreateMemoryBuffer(width_ * height_, &out_buffer);
    out_sample->AddBuffer(out_buffer);
    out_buffer->Release();

    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = output_stream_id_;
    output_data.pSample = out_sample;

    DWORD status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &output_data, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      out_sample->Release();
      break;
    }

    if (FAILED(hr)) {
      out_sample->Release();
      break;
    }

    // Extract encoded data
    IMFMediaBuffer* contiguous = nullptr;
    output_data.pSample->ConvertToContiguousBuffer(&contiguous);

    BYTE* data_ptr = nullptr;
    DWORD data_len = 0;
    contiguous->Lock(&data_ptr, nullptr, &data_len);

    // Check if keyframe
    UINT32 is_key = 0;
    output_data.pSample->GetUINT32(MFSampleExtension_CleanPoint, &is_key);

    // Deliver to WebRTC
    if (callback_ && data_len > 0) {
      EncodedImage encoded;
      encoded.SetEncodedData(EncodedImageBuffer::Create(data_ptr, data_len));
      encoded._encodedWidth = width_;
      encoded._encodedHeight = height_;
      encoded._frameType = is_key ? VideoFrameType::kVideoFrameKey
                                  : VideoFrameType::kVideoFrameDelta;
      encoded.SetRtpTimestamp(static_cast<uint32_t>(frame_count_));

      CodecSpecificInfo codec_info;
      if (format_.name == "H265" || format_.name == "HEVC") {
        codec_info.codecType = kVideoCodecH265;
      } else {
        codec_info.codecType = kVideoCodecH264;
      }

      callback_->OnEncodedImage(encoded, &codec_info);
    }

    contiguous->Unlock();
    contiguous->Release();
    out_sample->Release();
  }
}

}  // namespace webrtc
