// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/extras/enc/jxl.h"

#include "jxl/encode_cxx.h"

namespace jxl {
namespace extras {

JxlEncoderStatus SetOption(const JXLOption& opt,
                           JxlEncoderFrameSettings* settings) {
  return opt.is_float
             ? JxlEncoderFrameSettingsSetFloatOption(settings, opt.id, opt.fval)
             : JxlEncoderFrameSettingsSetOption(settings, opt.id, opt.ival);
}

bool SetFrameOptions(const std::vector<JXLOption>& options, size_t frame_index,
                     size_t* option_idx, JxlEncoderFrameSettings* settings) {
  while (*option_idx < options.size()) {
    const auto& opt = options[*option_idx];
    if (opt.frame_index > frame_index) {
      break;
    }
    if (JXL_ENC_SUCCESS != SetOption(opt, settings)) {
      fprintf(stderr, "Setting option id %d failed.\n", opt.id);
      return false;
    }
    (*option_idx)++;
  }
  return true;
}

bool EncodeImageJXL(const JXLCompressParams& params, const PackedPixelFile& ppf,
                    const std::vector<uint8_t>* jpeg_bytes,
                    std::vector<uint8_t>* compressed) {
  auto encoder = JxlEncoderMake(/*memory_manager=*/nullptr);
  JxlEncoder* enc = encoder.get();

  if (params.runner_opaque != nullptr &&
      JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc, params.runner,
                                                     params.runner_opaque)) {
    fprintf(stderr, "JxlEncoderSetParallelRunner failed\n");
    return false;
  }

  auto settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
  size_t option_idx = 0;
  if (!SetFrameOptions(params.options, 0, &option_idx, settings)) {
    return false;
  }
  if (JXL_ENC_SUCCESS !=
      JxlEncoderSetFrameDistance(settings, params.distance)) {
    fprintf(stderr, "Setting frame distance failed.\n");
    return false;
  }

  bool use_container = params.use_container;
  if (!ppf.metadata.exif.empty() || !ppf.metadata.xmp.empty() ||
      !ppf.metadata.jumbf.empty() || !ppf.metadata.iptc.empty() ||
      (jpeg_bytes && params.jpeg_store_metadata)) {
    use_container = true;
  }

  if (JXL_ENC_SUCCESS !=
      JxlEncoderUseContainer(enc, static_cast<int>(use_container))) {
    fprintf(stderr, "JxlEncoderUseContainer failed.\n");
    return false;
  }

  if (jpeg_bytes) {
    if (params.jpeg_store_metadata &&
        JXL_ENC_SUCCESS != JxlEncoderStoreJPEGMetadata(enc, JXL_TRUE)) {
      fprintf(stderr, "Storing JPEG metadata failed.\n");
      return false;
    }
    if (JXL_ENC_SUCCESS != JxlEncoderAddJPEGFrame(settings, jpeg_bytes->data(),
                                                  jpeg_bytes->size())) {
      fprintf(stderr, "JxlEncoderAddJPEGFrame() failed.\n");
      return false;
    }
  } else {
    size_t num_alpha_channels = 0;  // Adjusted below.
    JxlBasicInfo basic_info = ppf.info;
    if (basic_info.alpha_bits > 0) num_alpha_channels = 1;
    if (params.intensity_target > 0) {
      basic_info.intensity_target = params.intensity_target;
    }
    basic_info.num_extra_channels = num_alpha_channels;
    basic_info.num_color_channels = ppf.info.num_color_channels;
    const bool lossless = params.distance == 0;
    basic_info.uses_original_profile = lossless;
    if (params.override_bitdepth != 0) {
      basic_info.bits_per_sample = params.override_bitdepth;
      basic_info.exponent_bits_per_sample =
          params.override_bitdepth == 32 ? 8 : 0;
    }
    if (JXL_ENC_SUCCESS !=
        JxlEncoderSetCodestreamLevel(enc, params.codestream_level)) {
      fprintf(stderr, "Setting --codestream_level failed.\n");
      return false;
    }
    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc, &basic_info)) {
      fprintf(stderr, "JxlEncoderSetBasicInfo() failed.\n");
      return false;
    }
    if (lossless &&
        JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(settings, JXL_TRUE)) {
      fprintf(stderr, "JxlEncoderSetFrameLossless() failed.\n");
      return false;
    }
    if (!ppf.icc.empty()) {
      if (JXL_ENC_SUCCESS !=
          JxlEncoderSetICCProfile(enc, ppf.icc.data(), ppf.icc.size())) {
        fprintf(stderr, "JxlEncoderSetICCProfile() failed.\n");
        return false;
      }
    } else {
      if (JXL_ENC_SUCCESS !=
          JxlEncoderSetColorEncoding(enc, &ppf.color_encoding)) {
        fprintf(stderr, "JxlEncoderSetColorEncoding() failed.\n");
        return false;
      }
    }

    for (size_t num_frame = 0; num_frame < ppf.frames.size(); ++num_frame) {
      const jxl::extras::PackedFrame& pframe = ppf.frames[num_frame];
      const jxl::extras::PackedImage& pimage = pframe.color;
      JxlPixelFormat ppixelformat = pimage.format;
      if (JXL_ENC_SUCCESS !=
          JxlEncoderSetFrameHeader(settings, &pframe.frame_info)) {
        fprintf(stderr, "JxlEncoderSetFrameHeader() failed.\n");
        return false;
      }
      if (!SetFrameOptions(params.options, num_frame, &option_idx, settings)) {
        return false;
      }
      if (num_alpha_channels > 0) {
        JxlExtraChannelInfo extra_channel_info;
        JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &extra_channel_info);
        if (JXL_ENC_SUCCESS !=
            JxlEncoderSetExtraChannelInfo(enc, 0, &extra_channel_info)) {
          fprintf(stderr, "JxlEncoderSetExtraChannelInfo() failed.\n");
          return false;
        }
        if (params.premultiply != -1) {
          if (params.premultiply != 0 && params.premultiply != 1) {
            fprintf(stderr, "premultiply must be one of: -1, 0, 1.\n");
            return false;
          }
          extra_channel_info.alpha_premultiplied = params.premultiply;
        }
        // We take the extra channel blend info frame_info, but don't do
        // clamping.
        JxlBlendInfo extra_channel_blend_info =
            pframe.frame_info.layer_info.blend_info;
        extra_channel_blend_info.clamp = JXL_FALSE;
        JxlEncoderSetExtraChannelBlendInfo(settings, 0,
                                           &extra_channel_blend_info);
      }
      if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(settings, &ppixelformat,
                                                     pimage.pixels(),
                                                     pimage.pixels_size)) {
        fprintf(stderr, "JxlEncoderAddImageFrame() failed.\n");
        return false;
      }
      // Only set extra channel buffer if it is provided non-interleaved.
      if (!pframe.extra_channels.empty() &&
          JXL_ENC_SUCCESS != JxlEncoderSetExtraChannelBuffer(
                                 settings, &ppixelformat,
                                 pframe.extra_channels[0].pixels(),
                                 pframe.extra_channels[0].stride *
                                     pframe.extra_channels[0].ysize,
                                 0)) {
        fprintf(stderr, "JxlEncoderSetExtraChannelBuffer() failed.\n");
        return false;
      }
    }
  }
  JxlEncoderCloseInput(enc);
  // Reading compressed output
  compressed->clear();
  compressed->resize(4096);
  uint8_t* next_out = compressed->data();
  size_t avail_out = compressed->size() - (next_out - compressed->data());
  JxlEncoderStatus result = JXL_ENC_NEED_MORE_OUTPUT;
  while (result == JXL_ENC_NEED_MORE_OUTPUT) {
    result = JxlEncoderProcessOutput(enc, &next_out, &avail_out);
    if (result == JXL_ENC_NEED_MORE_OUTPUT) {
      size_t offset = next_out - compressed->data();
      compressed->resize(compressed->size() * 2);
      next_out = compressed->data() + offset;
      avail_out = compressed->size() - offset;
    }
  }
  compressed->resize(next_out - compressed->data());
  if (result != JXL_ENC_SUCCESS) {
    fprintf(stderr, "JxlEncoderProcessOutput failed.\n");
    return false;
  }
  return true;
}

}  // namespace extras
}  // namespace jxl
