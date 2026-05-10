#include "vaapi_decoder_impl.h"

#include <fcntl.h>
#include <unistd.h>
#include <va/va_drm.h>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

namespace webrtc {

VaapiDecoderImpl::VaapiDecoderImpl(const SdpVideoFormat& format)
    : format_(format), buffer_pool_(false) {}

VaapiDecoderImpl::~VaapiDecoderImpl() {
  Release();
}

VideoDecoder::DecoderInfo VaapiDecoderImpl::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "VAAPI";
  info.is_hardware_accelerated = true;
  return info;
}

int32_t VaapiDecoderImpl::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

bool VaapiDecoderImpl::Configure(const Settings& settings) {
  Release();

  uint32_t w = settings.max_render_resolution().Valid()
                   ? settings.max_render_resolution().Width()
                   : 1920;
  uint32_t h = settings.max_render_resolution().Valid()
                   ? settings.max_render_resolution().Height()
                   : 1080;

  if (!InitVaapi(w, h)) {
    return false;
  }

  initialized_ = true;
  return true;
}

int32_t VaapiDecoderImpl::Release() {
  DestroySurfaces();

  if (va_context_ != VA_INVALID_ID && va_display_) {
    vaDestroyContext(va_display_, va_context_);
    va_context_ = VA_INVALID_ID;
  }
  if (va_config_ != VA_INVALID_ID && va_display_) {
    vaDestroyConfig(va_display_, va_config_);
    va_config_ = VA_INVALID_ID;
  }
  if (va_display_) {
    vaTerminate(va_display_);
    va_display_ = nullptr;
  }
  if (drm_fd_ >= 0) {
    close(drm_fd_);
    drm_fd_ = -1;
  }

  initialized_ = false;
  buffer_pool_.Release();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t VaapiDecoderImpl::Decode(const EncodedImage& input_image,
                                 bool missing_frames,
                                 int64_t render_time_ms) {
  if (!initialized_ || !callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!input_image.data() || !input_image.size()) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  VASurfaceID surface = surfaces_[current_surface_];
  current_surface_ = (current_surface_ + 1) % kNumSurfaces;

  // Create decode buffer
  VABufferID slice_data_buf;
  VAStatus status = vaCreateBuffer(
      va_display_, va_context_, VASliceDataBufferType,
      input_image.size(), 1, const_cast<uint8_t*>(input_image.data()),
      &slice_data_buf);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI: vaCreateBuffer(SliceData) failed: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Create minimal slice parameter buffer
  // For H.264/HEVC, the VA driver parses the NAL units internally when
  // using VASliceDataBufferType with the full AU.
  VABufferID slice_param_buf;
  if (format_.name == "H265" || format_.name == "HEVC") {
    VASliceParameterBufferHEVC slice_param = {};
    slice_param.slice_data_size = input_image.size();
    slice_param.slice_data_offset = 0;
    slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    status = vaCreateBuffer(
        va_display_, va_context_, VASliceParameterBufferType,
        sizeof(slice_param), 1, &slice_param, &slice_param_buf);
  } else {
    VASliceParameterBufferH264 slice_param = {};
    slice_param.slice_data_size = input_image.size();
    slice_param.slice_data_offset = 0;
    slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    status = vaCreateBuffer(
        va_display_, va_context_, VASliceParameterBufferType,
        sizeof(slice_param), 1, &slice_param, &slice_param_buf);
  }
  if (status != VA_STATUS_SUCCESS) {
    vaDestroyBuffer(va_display_, slice_data_buf);
    RTC_LOG(LS_ERROR) << "VAAPI: vaCreateBuffer(SliceParam) failed: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Begin picture
  status = vaBeginPicture(va_display_, va_context_, surface);
  if (status != VA_STATUS_SUCCESS) {
    vaDestroyBuffer(va_display_, slice_data_buf);
    vaDestroyBuffer(va_display_, slice_param_buf);
    RTC_LOG(LS_ERROR) << "VAAPI: vaBeginPicture failed: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Render picture
  VABufferID buffers[] = {slice_param_buf, slice_data_buf};
  status = vaRenderPicture(va_display_, va_context_, buffers, 2);
  if (status != VA_STATUS_SUCCESS) {
    vaEndPicture(va_display_, va_context_);
    RTC_LOG(LS_ERROR) << "VAAPI: vaRenderPicture failed: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // End picture
  status = vaEndPicture(va_display_, va_context_);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI: vaEndPicture failed: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Sync and output
  status = vaSyncSurface(va_display_, surface);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI: vaSyncSurface failed: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!OutputFrame(surface, input_image.RtpTimestamp())) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

bool VaapiDecoderImpl::InitVaapi(uint32_t width, uint32_t height) {
  static const char* drm_paths[] = {"/dev/dri/renderD128",
                                    "/dev/dri/renderD129", nullptr};
  for (int i = 0; drm_paths[i]; i++) {
    drm_fd_ = open(drm_paths[i], O_RDWR);
    if (drm_fd_ >= 0) break;
  }
  if (drm_fd_ < 0) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: Failed to open DRM device";
    return false;
  }

  va_display_ = vaGetDisplayDRM(drm_fd_);
  if (!va_display_) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaGetDisplayDRM failed";
    return false;
  }

  int major, minor;
  VAStatus status = vaInitialize(va_display_, &major, &minor);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaInitialize failed: " << status;
    return false;
  }

  // Select profile
  VAProfile profile;
  if (format_.name == "H265" || format_.name == "HEVC") {
    profile = VAProfileHEVCMain;
  } else {
    profile = VAProfileH264High;
  }

  // Create config
  VAConfigAttrib attrib = {};
  attrib.type = VAConfigAttribRTFormat;
  vaGetConfigAttributes(va_display_, profile, VAEntrypointVLD, &attrib, 1);

  status = vaCreateConfig(va_display_, profile, VAEntrypointVLD,
                          &attrib, 1, &va_config_);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaCreateConfig failed: " << status;
    return false;
  }

  if (!CreateSurfaces(width, height)) {
    return false;
  }

  // Create context
  status = vaCreateContext(va_display_, va_config_, width, height,
                           VA_PROGRESSIVE, surfaces_, kNumSurfaces,
                           &va_context_);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaCreateContext failed: " << status;
    return false;
  }

  width_ = width;
  height_ = height;

  RTC_LOG(LS_INFO) << "VAAPI Decoder: Initialized " << format_.name
                   << " " << width << "x" << height;
  return true;
}

bool VaapiDecoderImpl::CreateSurfaces(uint32_t width, uint32_t height) {
  VAStatus status = vaCreateSurfaces(
      va_display_, VA_RT_FORMAT_YUV420, width, height,
      surfaces_, kNumSurfaces, nullptr, 0);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaCreateSurfaces failed: " << status;
    return false;
  }
  return true;
}

void VaapiDecoderImpl::DestroySurfaces() {
  if (va_display_ && surfaces_[0] != VA_INVALID_ID) {
    vaDestroySurfaces(va_display_, surfaces_, kNumSurfaces);
    for (int i = 0; i < kNumSurfaces; i++) surfaces_[i] = VA_INVALID_ID;
  }
}

bool VaapiDecoderImpl::OutputFrame(VASurfaceID surface, uint32_t rtp_timestamp) {
  VAImage va_image = {};
  VAStatus status = vaDeriveImage(va_display_, surface, &va_image);
  if (status != VA_STATUS_SUCCESS) {
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaDeriveImage failed: " << status;
    return false;
  }

  void* mapped = nullptr;
  status = vaMapBuffer(va_display_, va_image.buf, &mapped);
  if (status != VA_STATUS_SUCCESS) {
    vaDestroyImage(va_display_, va_image.image_id);
    RTC_LOG(LS_ERROR) << "VAAPI Decoder: vaMapBuffer failed: " << status;
    return false;
  }

  uint8_t* data = static_cast<uint8_t*>(mapped);
  rtc::scoped_refptr<I420Buffer> i420_buffer =
      buffer_pool_.CreateI420Buffer(width_, height_);

  // NV12 → I420 conversion
  uint8_t* y_plane = data + va_image.offsets[0];
  uint8_t* uv_plane = data + va_image.offsets[1];
  int y_stride = va_image.pitches[0];
  int uv_stride = va_image.pitches[1];

  libyuv::NV12ToI420(
      y_plane, y_stride,
      uv_plane, uv_stride,
      i420_buffer->MutableDataY(), i420_buffer->StrideY(),
      i420_buffer->MutableDataU(), i420_buffer->StrideU(),
      i420_buffer->MutableDataV(), i420_buffer->StrideV(),
      width_, height_);

  vaUnmapBuffer(va_display_, va_image.buf);
  vaDestroyImage(va_display_, va_image.image_id);

  VideoFrame decoded_frame = VideoFrame::Builder()
                                 .set_video_frame_buffer(i420_buffer)
                                 .set_timestamp_rtp(rtp_timestamp)
                                 .build();

  callback_->Decoded(decoded_frame);
  return true;
}

}  // namespace webrtc
