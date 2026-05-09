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

// Media Foundation Video Encoder Factory implementation.

// WIN32_LEAN_AND_MEAN (defined by libwebrtc) excludes MF headers from
// <windows.h>. We must include the MF headers explicitly after windows.h.
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>

#include "mf_encoder_factory.h"
#include "mf_video_encoder_impl.h"

#include "rtc_base/logging.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace webrtc {

// MF video format GUIDs
static const GUID kMFVideoFormat_H264 = {
    0x34363248, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kMFVideoFormat_HEVC = {
    0x43564548, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static bool HasHardwareEncoderForSubtype(const GUID& subtype) {
  MFT_REGISTER_TYPE_INFO output_type = {};
  output_type.guidMajorType = MFMediaType_Video;
  output_type.guidSubtype = subtype;

  IMFActivate** pp_activate = nullptr;
  UINT32 count = 0;

  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT,
      nullptr,
      &output_type,
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

MFVideoEncoderFactory::MFVideoEncoderFactory() {
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFStartup failed in encoder factory";
    return;
  }

  if (HasHardwareEncoderForSubtype(kMFVideoFormat_H264)) {
    std::map<std::string, std::string> params = {
        {"profile-level-id", "42e01f"},
        {"level-asymmetry-allowed", "1"},
        {"packetization-mode", "1"},
    };
    supported_formats_.push_back(SdpVideoFormat("H264", params));
    RTC_LOG(LS_INFO) << "MF: H.264 hardware encoder available";
  }

  if (HasHardwareEncoderForSubtype(kMFVideoFormat_HEVC)) {
    supported_formats_.push_back(SdpVideoFormat("H265"));
    supported_formats_.push_back(SdpVideoFormat("HEVC"));
    RTC_LOG(LS_INFO) << "MF: HEVC hardware encoder available";
  }
}

MFVideoEncoderFactory::~MFVideoEncoderFactory() {}

bool MFVideoEncoderFactory::IsSupported() {
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) return false;

  bool supported = HasHardwareEncoderForSubtype(kMFVideoFormat_H264) ||
                   HasHardwareEncoderForSubtype(kMFVideoFormat_HEVC);
  MFShutdown();
  return supported;
}

std::vector<SdpVideoFormat> MFVideoEncoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::unique_ptr<VideoEncoder> MFVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  for (const auto& supported : supported_formats_) {
    if (format.IsSameCodec(supported)) {
      RTC_LOG(LS_INFO) << "Creating MF hardware encoder for " << format.name;
      return std::make_unique<MFVideoEncoderImpl>(env, format);
    }
  }
  return nullptr;
}

}  // namespace webrtc
