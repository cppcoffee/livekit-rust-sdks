#include "vaapi_decoder_factory.h"

#include <dlfcn.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>

#include <memory>

#include "vaapi_decoder_impl.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "rtc_base/logging.h"

namespace webrtc {

static bool CheckDecodeSupport(VADisplay va_display, VAProfile profile) {
  int num_entrypoints = vaMaxNumEntrypoints(va_display);
  std::vector<VAEntrypoint> entrypoints(num_entrypoints);

  VAStatus status = vaQueryConfigEntrypoints(
      va_display, profile, entrypoints.data(), &num_entrypoints);
  if (status != VA_STATUS_SUCCESS) return false;

  for (int i = 0; i < num_entrypoints; i++) {
    if (entrypoints[i] == VAEntrypointVLD) return true;
  }
  return false;
}

struct VaapiProbeResult {
  bool h264 = false;
  bool hevc = false;
};

static VaapiProbeResult ProbeVaapiDecoding() {
  VaapiProbeResult result;

  static const char* drm_paths[] = {"/dev/dri/renderD128",
                                    "/dev/dri/renderD129", nullptr};
  for (int i = 0; drm_paths[i]; i++) {
    int fd = open(drm_paths[i], O_RDWR);
    if (fd < 0) continue;

    VADisplay va_display = vaGetDisplayDRM(fd);
    if (!va_display) {
      close(fd);
      continue;
    }

    int major, minor;
    if (vaInitialize(va_display, &major, &minor) != VA_STATUS_SUCCESS) {
      close(fd);
      continue;
    }

    // Check H.264 decode
    VAProfile h264_profiles[] = {VAProfileH264High, VAProfileH264Main,
                                 VAProfileH264ConstrainedBaseline};
    for (auto p : h264_profiles) {
      if (CheckDecodeSupport(va_display, p)) {
        result.h264 = true;
        break;
      }
    }

    // Check HEVC decode
    VAProfile hevc_profiles[] = {VAProfileHEVCMain, VAProfileHEVCMain10};
    for (auto p : hevc_profiles) {
      if (CheckDecodeSupport(va_display, p)) {
        result.hevc = true;
        break;
      }
    }

    vaTerminate(va_display);
    close(fd);

    if (result.h264 || result.hevc) break;
  }

  return result;
}

VAAPIVideoDecoderFactory::VAAPIVideoDecoderFactory() {
  auto probe = ProbeVaapiDecoding();

  if (probe.h264) {
    for (const SdpVideoFormat& format : SupportedH264DecoderCodecs()) {
      supported_formats_.push_back(format);
    }
    RTC_LOG(LS_INFO) << "VAAPI: H.264 hardware decoder available";
  }

  if (probe.hevc) {
    supported_formats_.push_back(SdpVideoFormat("H265"));
    supported_formats_.push_back(SdpVideoFormat("HEVC"));
    RTC_LOG(LS_INFO) << "VAAPI: HEVC hardware decoder available";
  }
}

VAAPIVideoDecoderFactory::~VAAPIVideoDecoderFactory() {}

bool VAAPIVideoDecoderFactory::IsSupported() {
  void* libva = dlopen("libva.so.2", RTLD_LAZY);
  if (!libva) return false;
  dlclose(libva);

  void* libva_drm = dlopen("libva-drm.so.2", RTLD_LAZY);
  if (!libva_drm) return false;
  dlclose(libva_drm);

  auto probe = ProbeVaapiDecoding();
  return probe.h264 || probe.hevc;
}

std::vector<SdpVideoFormat> VAAPIVideoDecoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::unique_ptr<VideoDecoder> VAAPIVideoDecoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  for (const auto& supported : supported_formats_) {
    if (format.IsSameCodec(supported)) {
      RTC_LOG(LS_INFO) << "Creating VAAPI hardware decoder for " << format.name;
      return std::make_unique<VaapiDecoderImpl>(format);
    }
  }
  return nullptr;
}

}  // namespace webrtc
