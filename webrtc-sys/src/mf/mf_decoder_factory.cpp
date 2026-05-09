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

// Media Foundation Video Decoder Factory implementation.

#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>

#include "mf_decoder_factory.h"
#include "mf_video_decoder_impl.h"

#include "modules/video_coding/codecs/h264/include/h264.h"
#include "rtc_base/logging.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace webrtc {

static const GUID kMFVideoFormat_H264 = {
    0x34363248, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_HEVC = {
    0x43564548, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static bool HasHardwareDecoderForSubtype(const GUID& subtype) {
  MFT_REGISTER_TYPE_INFO input_type = {};
  input_type.guidMajorType = MFMediaType_Video;
  input_type.guidSubtype = subtype;

  IMFActivate** pp_activate = nullptr;
  UINT32 count = 0;

  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_DECODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
      &input_type,
      nullptr,
      &pp_activate,
      &count);

  if (SUCCEEDED(hr) && pp_activate) {
    for (UINT32 i = 0; i < count; i++) {
      pp_activate[i]->Release();
    }
    CoTaskMemFree(pp_activate);
  }

  return SUCCEEDED(hr) && count > 0;
}

MFVideoDecoderFactory::MFVideoDecoderFactory() {
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFStartup failed in decoder factory";
    return;
  }

  if (HasHardwareDecoderForSubtype(kMFVideoFormat_H264)) {
    for (const SdpVideoFormat& format : SupportedH264DecoderCodecs()) {
      supported_formats_.push_back(format);
    }
    RTC_LOG(LS_INFO) << "MF: H.264 hardware decoder available";
  }

  if (HasHardwareDecoderForSubtype(kMFVideoFormat_HEVC)) {
    supported_formats_.push_back(SdpVideoFormat("H265"));
    supported_formats_.push_back(SdpVideoFormat("HEVC"));
    RTC_LOG(LS_INFO) << "MF: HEVC hardware decoder available";
  }
}

MFVideoDecoderFactory::~MFVideoDecoderFactory() {}

bool MFVideoDecoderFactory::IsSupported() {
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) return false;

  bool supported = HasHardwareDecoderForSubtype(kMFVideoFormat_H264) ||
                   HasHardwareDecoderForSubtype(kMFVideoFormat_HEVC);
  MFShutdown();
  return supported;
}

std::vector<SdpVideoFormat> MFVideoDecoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::unique_ptr<VideoDecoder> MFVideoDecoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  for (const auto& supported : supported_formats_) {
    if (format.IsSameCodec(supported)) {
      RTC_LOG(LS_INFO) << "Creating MF hardware decoder for " << format.name;
      return std::make_unique<MFVideoDecoderImpl>(format);
    }
  }
  return nullptr;
}

}  // namespace webrtc
