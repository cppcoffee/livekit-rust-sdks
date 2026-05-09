#ifndef VAAPI_VIDEO_DECODER_FACTORY_H_
#define VAAPI_VIDEO_DECODER_FACTORY_H_

#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder_factory.h"

namespace webrtc {

class VAAPIVideoDecoderFactory : public VideoDecoderFactory {
 public:
  VAAPIVideoDecoderFactory();
  ~VAAPIVideoDecoderFactory() override;

  static bool IsSupported();

  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::unique_ptr<VideoDecoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;

 private:
  std::vector<SdpVideoFormat> supported_formats_;
};

}  // namespace webrtc

#endif  // VAAPI_VIDEO_DECODER_FACTORY_H_
