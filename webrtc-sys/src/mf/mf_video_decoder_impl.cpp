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

// Media Foundation VideoDecoder implementation.
//
// Uses IMFTransform to hardware-decode H.264/HEVC bitstreams into NV12,
// then converts to I420 for WebRTC consumption.

#include "mf_video_decoder_impl.h"

#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace webrtc {

static const GUID kMFMediaType_Video = {
    0x73646976, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_NV12 = {
    0x3231564E, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_H264 = {
    0x34363248, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_HEVC = {
    0x43564548, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

MFVideoDecoderImpl::MFVideoDecoderImpl(const SdpVideoFormat& format)
    : format_(format), buffer_pool_(false) {}

MFVideoDecoderImpl::~MFVideoDecoderImpl() {
  Release();
}

VideoDecoder::DecoderInfo MFVideoDecoderImpl::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "MediaFoundation";
  info.is_hardware_accelerated = true;
  return info;
}

bool MFVideoDecoderImpl::Configure(const Settings& settings) {
  Release();

  uint32_t w = settings.max_render_resolution().Valid()
                   ? settings.max_render_resolution().Width()
                   : 1920;
  uint32_t h = settings.max_render_resolution().Valid()
                   ? settings.max_render_resolution().Height()
                   : 1080;

  if (!InitMFT(w, h)) {
    return false;
  }

  initialized_ = true;
  return true;
}

int32_t MFVideoDecoderImpl::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFVideoDecoderImpl::Release() {
  if (transform_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    transform_->Release();
    transform_ = nullptr;
  }
  initialized_ = false;
  buffer_pool_.Release();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MFVideoDecoderImpl::Decode(const EncodedImage& input_image,
                                   bool missing_frames,
                                   int64_t render_time_ms) {
  if (!initialized_ || !callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!input_image.data() || !input_image.size()) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  // Create input sample
  IMFSample* sample = nullptr;
  IMFMediaBuffer* buffer = nullptr;
  MFCreateSample(&sample);
  MFCreateMemoryBuffer(static_cast<DWORD>(input_image.size()), &buffer);

  BYTE* buf_ptr = nullptr;
  buffer->Lock(&buf_ptr, nullptr, nullptr);
  memcpy(buf_ptr, input_image.data(), input_image.size());
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(input_image.size()));
  sample->AddBuffer(buffer);
  buffer->Release();

  // Set timestamp (100ns units)
  sample->SetSampleTime(static_cast<LONGLONG>(input_image.RtpTimestamp()) * 10000LL);

  HRESULT hr = transform_->ProcessInput(input_stream_id_, sample, 0);
  sample->Release();

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MF Decoder: ProcessInput failed: 0x" << std::hex << hr;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Drain all available output
  DrainOutput(input_image.RtpTimestamp());

  return WEBRTC_VIDEO_CODEC_OK;
}

bool MFVideoDecoderImpl::InitMFT(uint32_t width, uint32_t height) {
  GUID input_subtype = (format_.name == "H265" || format_.name == "HEVC")
                           ? kMFVideoFormat_HEVC
                           : kMFVideoFormat_H264;

  // Enumerate hardware decoder MFTs
  MFT_REGISTER_TYPE_INFO input_type = {};
  input_type.guidMajorType = MFMediaType_Video;
  input_type.guidSubtype = input_subtype;

  IMFActivate** pp_activate = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_DECODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
      &input_type, nullptr, &pp_activate, &count);

  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_ERROR) << "MF Decoder: No hardware decoder found for " << format_.name;
    return false;
  }

  hr = pp_activate[0]->ActivateObject(IID_IMFTransform,
                                       reinterpret_cast<void**>(&transform_));
  for (UINT32 i = 0; i < count; i++) pp_activate[i]->Release();
  CoTaskMemFree(pp_activate);

  if (FAILED(hr) || !transform_) {
    RTC_LOG(LS_ERROR) << "MF Decoder: Failed to activate decoder MFT";
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

  // Set input type (compressed)
  IMFMediaType* in_mt = nullptr;
  MFCreateMediaType(&in_mt);
  in_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in_mt->SetGUID(MF_MT_SUBTYPE, input_subtype);
  in_mt->SetUINT64(MF_MT_FRAME_SIZE,
                   (static_cast<UINT64>(width) << 32) | height);

  hr = transform_->SetInputType(input_stream_id_, in_mt, 0);
  in_mt->Release();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MF Decoder: SetInputType failed: 0x" << std::hex << hr;
    return false;
  }

  // Set output type (NV12)
  IMFMediaType* out_mt = nullptr;
  MFCreateMediaType(&out_mt);
  out_mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_mt->SetGUID(MF_MT_SUBTYPE, kMFVideoFormat_NV12);
  out_mt->SetUINT64(MF_MT_FRAME_SIZE,
                    (static_cast<UINT64>(width) << 32) | height);

  hr = transform_->SetOutputType(output_stream_id_, out_mt, 0);
  out_mt->Release();
  if (FAILED(hr)) {
    // Try to negotiate output type from MFT's preferred list
    for (DWORD i = 0; ; i++) {
      IMFMediaType* avail_type = nullptr;
      hr = transform_->GetOutputAvailableType(output_stream_id_, i, &avail_type);
      if (FAILED(hr)) break;

      GUID subtype;
      avail_type->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (subtype == kMFVideoFormat_NV12) {
        hr = transform_->SetOutputType(output_stream_id_, avail_type, 0);
        avail_type->Release();
        if (SUCCEEDED(hr)) break;
      } else {
        avail_type->Release();
      }
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "MF Decoder: SetOutputType failed: 0x" << std::hex << hr;
      return false;
    }
  }

  width_ = width;
  height_ = height;

  // Enable low-latency mode
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

  RTC_LOG(LS_INFO) << "MF Decoder: Initialized " << format_.name
                   << " " << width << "x" << height;
  return true;
}

bool MFVideoDecoderImpl::HandleStreamChange() {
  // Get new output type after stream change
  IMFMediaType* new_type = nullptr;
  HRESULT hr = transform_->GetOutputAvailableType(output_stream_id_, 0, &new_type);
  if (FAILED(hr)) return false;

  hr = transform_->SetOutputType(output_stream_id_, new_type, 0);
  if (FAILED(hr)) {
    new_type->Release();
    return false;
  }

  // Update dimensions
  UINT64 frame_size = 0;
  new_type->GetUINT64(MF_MT_FRAME_SIZE, &frame_size);
  width_ = static_cast<uint32_t>(frame_size >> 32);
  height_ = static_cast<uint32_t>(frame_size & 0xFFFFFFFF);
  new_type->Release();

  RTC_LOG(LS_INFO) << "MF Decoder: Stream changed to " << width_ << "x" << height_;
  return true;
}

bool MFVideoDecoderImpl::DrainOutput(uint32_t rtp_timestamp) {
  while (true) {
    // Check if MFT provides its own output samples
    MFT_OUTPUT_STREAM_INFO stream_info = {};
    transform_->GetOutputStreamInfo(output_stream_id_, &stream_info);

    IMFSample* out_sample = nullptr;
    bool mft_provides_samples =
        (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    if (!mft_provides_samples) {
      IMFMediaBuffer* out_buffer = nullptr;
      DWORD buf_size = stream_info.cbSize;
      if (buf_size == 0) buf_size = width_ * height_ * 3 / 2;

      MFCreateSample(&out_sample);
      MFCreateMemoryBuffer(buf_size, &out_buffer);
      out_sample->AddBuffer(out_buffer);
      out_buffer->Release();
    }

    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = output_stream_id_;
    output_data.pSample = out_sample;

    DWORD status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &output_data, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (out_sample) out_sample->Release();
      break;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (out_sample) out_sample->Release();
      if (!HandleStreamChange()) break;
      continue;
    }

    if (FAILED(hr)) {
      if (out_sample) out_sample->Release();
      break;
    }

    // Use MFT-provided sample if applicable
    IMFSample* result_sample = mft_provides_samples ? output_data.pSample : out_sample;
    if (!result_sample) break;

    // Extract NV12 data and convert to I420
    IMFMediaBuffer* contiguous = nullptr;
    result_sample->ConvertToContiguousBuffer(&contiguous);

    BYTE* data_ptr = nullptr;
    DWORD data_len = 0;
    contiguous->Lock(&data_ptr, nullptr, &data_len);

    uint32_t stride = width_;
    // Some MFTs may use aligned stride; detect from buffer size
    uint32_t expected_nv12_size = width_ * height_ * 3 / 2;
    if (data_len > expected_nv12_size && height_ > 0) {
      stride = static_cast<uint32_t>((data_len * 2) / (height_ * 3));
    }

    rtc::scoped_refptr<I420Buffer> i420_buffer =
        buffer_pool_.CreateI420Buffer(width_, height_);

    libyuv::NV12ToI420(
        data_ptr, stride,
        data_ptr + stride * height_, stride,
        i420_buffer->MutableDataY(), i420_buffer->StrideY(),
        i420_buffer->MutableDataU(), i420_buffer->StrideU(),
        i420_buffer->MutableDataV(), i420_buffer->StrideV(),
        width_, height_);

    contiguous->Unlock();
    contiguous->Release();

    VideoFrame decoded_frame = VideoFrame::Builder()
                                   .set_video_frame_buffer(i420_buffer)
                                   .set_timestamp_rtp(rtp_timestamp)
                                   .build();

    callback_->Decoded(decoded_frame);

    result_sample->Release();
    if (output_data.pEvents) output_data.pEvents->Release();
  }

  return true;
}

}  // namespace webrtc
