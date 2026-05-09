#ifndef VAAPI_DECODER_IMPL_H_
#define VAAPI_DECODER_IMPL_H_

#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/sdp_video_format.h"
#include "common_video/include/video_frame_buffer_pool.h"

#include <va/va.h>

namespace webrtc {

class VaapiDecoderImpl : public VideoDecoder {
 public:
  explicit VaapiDecoderImpl(const SdpVideoFormat& format);
  ~VaapiDecoderImpl() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  bool InitVaapi(uint32_t width, uint32_t height);
  bool CreateSurfaces(uint32_t width, uint32_t height);
  void DestroySurfaces();
  bool OutputFrame(VASurfaceID surface, uint32_t rtp_timestamp);

  SdpVideoFormat format_;
  DecodedImageCallback* callback_ = nullptr;
  VideoFrameBufferPool buffer_pool_;

  int drm_fd_ = -1;
  VADisplay va_display_ = nullptr;
  VAConfigID va_config_ = VA_INVALID_ID;
  VAContextID va_context_ = VA_INVALID_ID;

  static constexpr int kNumSurfaces = 8;
  VASurfaceID surfaces_[kNumSurfaces] = {};
  int current_surface_ = 0;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;
};

}  // namespace webrtc

#endif  // VAAPI_DECODER_IMPL_H_
