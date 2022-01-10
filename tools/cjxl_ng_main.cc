// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.
#include <stdint.h>

#include <iostream>
#include <sstream>
#include <vector>

#include "fetch_encoded.h"
#include "gflags/gflags.h"
#include "jxl/encode.h"
#include "jxl/thread_parallel_runner.h"
#include "lib/extras/codec.h"
#include "lib/extras/dec/apng.h"
#include "lib/extras/dec/color_hints.h"
#include "lib/extras/dec/gif.h"
#include "lib/extras/dec/jpg.h"
#include "lib/extras/dec/pgx.h"
#include "lib/extras/dec/pnm.h"
#include "lib/jxl/base/file_io.h"
// TODO(firsching): Remove this include once not needed for SizeConstraints
// anymore
#include "lib/jxl/codec_in_out.h"

DECLARE_bool(help);
DECLARE_bool(helpshort);
// The flag --version is owned by gflags itself.
DEFINE_bool(encoder_version, false,
            "Print encoder library version number and exit.");

DEFINE_bool(add_jpeg_frame, false,
            "Use JxlEncoderAddJPEGFrame to add a JPEG frame, "
            "rather than JxlEncoderAddImageFrame.");

DEFINE_bool(container, false,
            "Force using container format (default: use only if needed).");

DEFINE_bool(strip, false,
            "Do not encode using container format (strips "
            "Exif/XMP/JPEG bitstream reconstruction data).");

DEFINE_bool(progressive, false,  // TODO(tfish): Wire this up.
            "Enable progressive/responsive decoding.");

DEFINE_bool(progressive_ac, false,  // TODO(tfish): Wire this up.
            "Use progressive mode for AC.");

DEFINE_bool(qprogressive_ac,
            false,  // TODO(tfish): Wire this up.
                    // TODO(tfish): Clarify what this flag is about.
            "Use progressive mode for AC.");

DEFINE_bool(progressive_dc, false,  // TODO(tfish): Wire this up.
            "Use progressive mode for DC.");

DEFINE_bool(use_experimental_encoder_heuristics,
            false,  // TODO(tfish): Wire this up.
            "Use new and not yet ready encoder heuristics");

DEFINE_bool(jpeg_transcode, false,  // TODO(tfish): Wire this up.
            "Do lossy transcode of input JPEG file (decode to "
            "pixels instead of doing lossless transcode).");

DEFINE_bool(jpeg_transcode_disable_cfl, false,  // TODO(tfish): Wire this up.
            "Disable CFL for lossless JPEG recompression");

DEFINE_bool(premultiply, false,  // TODO(tfish): Wire this up.
            "Force premultiplied (associated) alpha.");

DEFINE_bool(centerfirst, false,  // TODO(tfish): Wire this up.
            "Put center groups first in the compressed file.");

// TODO(tfish): Clarify if this is indeed deprecated. Remove if it is.
// DEFINE_bool(noise, false,
//           "force disable/enable noise generation.");

DEFINE_bool(verbose, false,
            // TODO(tfish): Should be a verbosity-level.
            // Original cjxl also makes --help more verbose if this is on,
            // but with gflags, we do that differently...?
            "Verbose output.");

DEFINE_bool(already_downsampled, false,
            "Do not downsample the given input before encoding, "
            "but still signal that the decoder should upsample.");

DEFINE_bool(
    modular, false,
    // TODO(tfish): Flag up parameter meaning change.
    "Use modular mode (not provided = encoder chooses, 0 = enforce VarDCT, "
    "1 = enforce modular mode).");

DEFINE_bool(keep_invisible, false,
            "Force disable/enable preserving color of invisible "
            "pixels. (not provided = default, 0 = disable, 1 = enable).");

DEFINE_bool(dots, false,
            "Force disable/enable dots generation. "
            "(not provided = default, 0 = disable, 1 = enable).");

DEFINE_bool(patches, false,
            "Force disable/enable patches generation. "
            "(not provided = default, 0 = disable, 1 = enable).");

DEFINE_bool(gaborish, false,
            "Force disable/enable the gaborish filter. "
            "(not provided = default, 0 = disable, 1 = enable).");

DEFINE_bool(
    group_order, false,
    // TODO(tfish): This is a new flag. Check with team.
    "Order in which 256x256 regions are stored "
    "in the codestream for progressive rendering. "
    "Value not provided means 'encoder default', 0 means 'scanline order', "
    "1 means 'center-first order'.");
// TODO(tfish):
// --intensity_target,
// --saliency_num_progressive_steps, --saliency_map_filename,
// --saliency_threshold, --dec-hints, --override_bitdepth,
// --colortransform, --mquality, --iterations, --colorspace, --group-size,
// --predictor, --extra-properties, --lossy-palette, --pre-compact,
// --post-compact, --responsive, --quiet, --print_profile,

DEFINE_int32(store_jpeg_metadata, -1,
             "Store JPEG reconstruction metadata in the JPEG XL container. "
             "(-1 = default, 0 = disable, 1 = enable).");

DEFINE_int32(faster_decoding, 0,
             "Favour higher decoding speed. 0 = default, higher "
             "values give higher speed at the expense of quality");

DEFINE_int32(
    resampling, -1,
    // TODO(tfish): Discuss with team. The new docstring is from the C API
    // documentation. This differs from what the old docstring said.
    "Resampling. Default of -1 applies resampling only for low quality. "
    "Value 1 does no downsampling (1x1), 2 does 2x2 downsampling, "
    "4 is for 4x4 downsampling, and 8 for 8x8 downsampling.");

DEFINE_int32(
    ec_resampling, -1,
    // TODO(tfish): Discuss with team. The new docstring is from the C API
    // documentation. This differs from what the old docstring said.
    "Resampling for extra channels. Default of -1 applies resampling only "
    "for low quality. Value 1 does no downsampling (1x1), 2 does 2x2 "
    "downsampling, 4 is for 4x4 downsampling, and 8 for 8x8 downsampling.");

DEFINE_int32(
    epf, -1,
    "Edge preserving filter level, -1 to 3. "
    "Value -1 means: default (encoder chooses), 0 to 3 set a strength.");

DEFINE_int64(
    center_x, -1,
    // TODO(tfish): Clarify if this is really the comment we want here.
    "Determines the horizontal position of center for the center-first "
    "group order. The value -1 means 'use the middle of the image', "
    // TODO(tfish): Clarify if encode.h has an off-by-one in the
    // upper limit here.
    "other values 0..(xsize-1) set this to a particular coordinate.");

DEFINE_int64(center_y, -1,
             // TODO(tfish): Clarify if this is really the comment we want here.
             "Determines the vertical position of center for the center-first "
             "group order. The value -1 means 'use the middle of the image', "
             // TODO(tfish): Clarify if encode.h has an off-by-one in the
             // upper limit here.
             "other values 0..(ysize-1) set this to a particular coordinate.");

DEFINE_int64(num_threads, 0,
             // TODO(tfish): Sync with team about changed meaning of 0 -
             // was: No multithreaded workers. Is: use default number.
             "Number of worker threads (0 == use machine default).");

DEFINE_int64(num_reps, 1,  // TODO(tfish): wire this up.
                           // TODO(tfish): Clarify meaning of this docstring.
                           // Is this simply for benchmarking?
             "How many times to compress.");

DEFINE_int32(photon_noise, 0,
             // TODO(tfish): Discuss docstring change with team.
             // Also: This now is an int, no longer a float.
             "Adds noise to the image emulating photographic film noise. "
             "The higher the given number, the grainier the image will be. "
             "As an example, a value of 100 gives low noise whereas a value "
             "of 3200 gives a lot of noise. The default value is 0.");

DEFINE_double(
    distance, 1.0,  // TODO(tfish): wire this up.
    "Max. butteraugli distance, lower = higher quality. Range: 0 .. 25.\n"
    "    0.0 = mathematically lossless. Default for already-lossy input "
    "(JPEG/GIF).\n"
    "    1.0 = visually lossless. Default for other input.\n"
    "    Recommended range: 0.5 .. 3.0.");

DEFINE_int64(target_size, 0,  // TODO(tfish): wire this up.
             "Aim at file size of N bytes.\n"
             "    Compresses to 1 % of the target size in ideal conditions.\n"
             "    Runs the same algorithm as --target_bpp");

DEFINE_double(target_bpp, 0.0,  // TODO(tfish): wire this up.
              "Aim at file size that has N bits per pixel.\n"
              "    Compresses to 1 % of the target BPP in ideal conditions.");

DEFINE_double(
    quality, 100.0,  // TODO(tfish): wire this up.
    "Quality setting (is remapped to --distance). Range: -inf .. 100.\n"
    "    100 = mathematically lossless. Default for already-lossy input "
    "(JPEG/GIF).\n    Positive quality values roughly match libjpeg "
    "quality.");

DEFINE_int64(
    effort, 7,
    // TODO(tfish): Clarify discrepancy with team:
    // Documentation says default==squirrel(7) here:
    // https://libjxl.readthedocs.io/en/latest/api_encoder.html#_CPPv424JxlEncoderFrameSettingId
    // but enc_params.h has kFalcon=7.
    "Encoder effort setting. Range: 1 .. 9.\n"
    "    Default: 7. Higher number is more effort (slower).");

namespace {

// RAII-wraps the C-API encoder.
class ManagedJxlEncoder {
 public:
  explicit ManagedJxlEncoder(size_t num_worker_threads)
      : encoder_(JxlEncoderCreate(NULL)),
        encoder_frame_settings_(JxlEncoderFrameSettingsCreate(encoder_, NULL)) {
    if (num_worker_threads > 1) {
      parallel_runner_ = JxlThreadParallelRunnerCreate(
          /*memory_manager=*/nullptr, num_worker_threads);
    }
  }
  ~ManagedJxlEncoder() {
    if (parallel_runner_ != nullptr) {
      JxlThreadParallelRunnerDestroy(parallel_runner_);
    }
    JxlEncoderDestroy(encoder_);
    if (compressed_buffer_) {
      free(compressed_buffer_);
    }
  }

  JxlEncoder* encoder_;
  JxlEncoderFrameSettings* encoder_frame_settings_;
  uint8_t* compressed_buffer_ = nullptr;
  size_t compressed_buffer_size_ = 0;
  size_t compressed_buffer_used_ = 0;
  void* parallel_runner_ = nullptr;  // TODO(tfish): fix type.
};

bool ProcessTristateFlag(const char* flag_name, const bool flag_value,
                         JxlEncoderFrameSettings* frame_settings,
                         JxlEncoderFrameSettingId encoder_option) {
  gflags::CommandLineFlagInfo flag_info =
      gflags::GetCommandLineFlagInfoOrDie(flag_name);
  if (!flag_info.is_default) {
    JxlEncoderFrameSettingsSetOption(frame_settings, encoder_option,
                                     static_cast<int32_t>(flag_value));
  }
  return true;
}
// XXX this mimicks SetFromBytes in cjxl.cc
jxl::Status LoadInput(const char* filename_in,
                      jxl::extras::PackedPixelFile& ppf) {
  // Any valid encoding is larger (ensures codecs can read the first few bytes).
  constexpr size_t kMinBytes = 9;

  jxl::PaddedBytes image_data;
  jxl::Status status = ReadFile(filename_in, &image_data);
  if (!status) {
    return status;
  }
  if (image_data.size() < kMinBytes) return JXL_FAILURE("Input too small.");
  jxl::Span<const uint8_t> encoded(image_data);

  // Default values when not set by decoders.
  ppf.info.uses_original_profile = true;
  ppf.info.orientation = JXL_ORIENT_IDENTITY;
  jxl::extras::ColorHints color_hints;
  jxl::SizeConstraints size_constraints;

  jxl::extras::Codec codec;
  (void)codec;
#if JPEGXL_ENABLE_APNG
  if (jxl::extras::DecodeImageAPNG(encoded, color_hints, size_constraints,
                                   &ppf)) {
    codec = jxl::extras::Codec::kPNG;
  } else
#endif
      if (jxl::extras::DecodeImagePGX(encoded, color_hints, size_constraints,
                                      &ppf)) {
    codec = jxl::extras::Codec::kPGX;
  } else if (jxl::extras::DecodeImagePNM(encoded, color_hints, size_constraints,
                                         &ppf)) {
    codec = jxl::extras::Codec::kPNM;
  }
#if JPEGXL_ENABLE_GIF
  else if (jxl::extras::DecodeImageGIF(encoded, color_hints, size_constraints,
                                       &ppf)) {
    codec = jxl::extras::Codec::kGIF;
  }
#endif
#if JPEGXL_ENABLE_JPEG
  else if (jxl::extras::DecodeImageJPG(encoded, color_hints, size_constraints,
                                       &ppf)) {
    codec = jxl::extras::Codec::kJPG;
  }
#endif
  else {  // TODO(tfish): Bring back EXR and PSD.
    return JXL_FAILURE("Codecs failed to decode input.");
  }
  // TODO(tfish): Migrate this:
  // if (!skip_ppf_conversion) {
  //   JXL_RETURN_IF_ERROR(ConvertPackedPixelFileToCodecInOut(ppf, pool, io));
  // }
  return true;
}

}  // namespace
// tristate flag not necessary, because we can use
// gflags::GetCommandLineFlagInfoOrDie(const char* name).is_default
int main(int argc, char** argv) {
  std::cerr << "Warning: This is work in progress, consider using cjxl "
               "instead!\n";
  gflags::SetUsageMessage(
      "JPEG XL-encodes an image.\n"
      " Input format can be one of: "
#if JPEGXL_ENABLE_APNG
      "PNG, APNG, "
#endif
#if JPEGXL_ENABLE_GIF
      "GIF, "
#endif
#if JPEGXL_ENABLE_JPEG
      "JPEG, "
#endif
      "PPM, PFM, PGX.\n  Sample usage:\n" +
      std::string(argv[0]) +
      " <source_image_filename> <target_image_filename>");
  uint32_t version = JxlEncoderVersion();

  gflags::SetVersionString(std::to_string(version / 1000000) + "." +
                           std::to_string((version / 1000) % 1000) + "." +
                           std::to_string(version % 1000));
  // TODO(firsching): rethink --help handling
  gflags::ParseCommandLineNonHelpFlags(&argc, &argv, /*remove_flags=*/true);
  if (FLAGS_help) {
    FLAGS_help = false;
    FLAGS_helpshort = true;
  }
  gflags::HandleCommandLineHelpFlags();

  if (argc != 3) {
    FLAGS_help = false;
    FLAGS_helpshort = true;
    gflags::HandleCommandLineHelpFlags();
    return EXIT_FAILURE;
  }
  const char* filename_in = argv[1];
  const char* filename_out = argv[2];

  size_t num_worker_threads = JxlThreadParallelRunnerDefaultNumWorkerThreads();
  {
    int64_t flag_num_worker_threads = FLAGS_num_threads;
    if (flag_num_worker_threads != 0) {
      num_worker_threads = flag_num_worker_threads;
    }
  }
  ManagedJxlEncoder managed_jxl_encoder = ManagedJxlEncoder(num_worker_threads);
  JxlEncoder* jxl_encoder = managed_jxl_encoder.encoder_;

  const int32_t store_jpeg_metadata = FLAGS_store_jpeg_metadata;
  if (!(-1 <= store_jpeg_metadata && store_jpeg_metadata <= 1)) {
    std::cerr
        << "Invalid --store_jpeg_metadata. Valid values are {-1, 0, 1}.\n";
    return EXIT_FAILURE;
  }
  if (store_jpeg_metadata != -1) {
    if (JXL_ENC_SUCCESS !=
        JxlEncoderStoreJPEGMetadata(jxl_encoder, store_jpeg_metadata != 0)) {
      std::cerr << "JxlEncoderStoreJPEGMetadata failed\n";
      return EXIT_FAILURE;
    }
  }

  if (managed_jxl_encoder.parallel_runner_ != nullptr) {
    if (JXL_ENC_SUCCESS !=
        JxlEncoderSetParallelRunner(
            managed_jxl_encoder.encoder_,
            // TODO(tfish): Flag up the need to have the parameter below
            // documented better in the encode.h API docs.
            JxlThreadParallelRunner, managed_jxl_encoder.parallel_runner_)) {
      std::cerr << "JxlEncoderSetParallelRunner failed\n";
      return EXIT_FAILURE;
    }
  }

  JxlEncoderFrameSettings* jxl_encoder_frame_settings =
      managed_jxl_encoder.encoder_frame_settings_;

  {  // Processing tuning flags.
    bool use_container = FLAGS_container;
    // TODO(tfish): Set use_container according to need of encoded data.
    // This will likely require moving this piece out of flags-processing.
    if (FLAGS_strip) {
      use_container = false;
    }
    JxlEncoderUseContainer(jxl_encoder, use_container);

    ProcessTristateFlag("modular", FLAGS_modular, jxl_encoder_frame_settings,
                        JXL_ENC_FRAME_SETTING_MODULAR);
    ProcessTristateFlag("keep_invisible", FLAGS_keep_invisible,
                        jxl_encoder_frame_settings,
                        JXL_ENC_FRAME_SETTING_KEEP_INVISIBLE);
    ProcessTristateFlag("dots", FLAGS_dots, jxl_encoder_frame_settings,
                        JXL_ENC_FRAME_SETTING_DOTS);
    ProcessTristateFlag("patches", FLAGS_patches, jxl_encoder_frame_settings,
                        JXL_ENC_FRAME_SETTING_PATCHES);
    ProcessTristateFlag("gaborish", FLAGS_gaborish, jxl_encoder_frame_settings,
                        JXL_ENC_FRAME_SETTING_GABORISH);
    ProcessTristateFlag("group_order", FLAGS_group_order,
                        jxl_encoder_frame_settings,
                        JXL_ENC_FRAME_SETTING_GROUP_ORDER);

    const int32_t flag_effort = FLAGS_effort;
    if (!(1 <= flag_effort && flag_effort <= 9)) {
      // Strictly speaking, custom gflags parsing would integrate
      // more nicely with gflags, but the boilerplate cost of
      // handling invalid calls is substantially higher than
      // this lightweight approach here.
      std::cerr << "Invalid --effort. Valid range is {1, 2, ..., 9}.\n";
      return EXIT_FAILURE;
    }
    JxlEncoderFrameSettingsSetOption(jxl_encoder_frame_settings,
                                     JXL_ENC_FRAME_SETTING_EFFORT, flag_effort);

    const int32_t flag_epf = FLAGS_epf;
    if (!(-1 <= flag_epf && flag_epf <= 3)) {
      std::cerr << "Invalid --epf. Valid range is {-1, 0, 1, 2, 3}.\n";
      return EXIT_FAILURE;
    }
    if (flag_epf != -1) {
      JxlEncoderFrameSettingsSetOption(jxl_encoder_frame_settings,
                                       JXL_ENC_FRAME_SETTING_EPF, flag_epf);
    }

    const int32_t flag_faster_decoding = FLAGS_faster_decoding;
    if (!(0 <= flag_faster_decoding && flag_faster_decoding <= 4)) {
      std::cerr << "Invalid --faster_decoding. "
                   "Valid range is {0, 1, 2, 3, 4}.\n";
      return EXIT_FAILURE;
    }
    JxlEncoderFrameSettingsSetOption(jxl_encoder_frame_settings,
                                     JXL_ENC_FRAME_SETTING_DECODING_SPEED,
                                     flag_faster_decoding);

    const int32_t flag_resampling = FLAGS_resampling;
    if (flag_resampling != -1) {
      if (!(((flag_resampling & (flag_resampling - 1)) == 0) &&
            flag_resampling <= 8)) {
        std::cerr << "Invalid --resampling. "
                     "Valid values are {-1, 1, 2, 4, 8}.\n";
        return EXIT_FAILURE;
      }
      JxlEncoderFrameSettingsSetOption(jxl_encoder_frame_settings,
                                       JXL_ENC_FRAME_SETTING_RESAMPLING,
                                       flag_resampling);
    }
    const int32_t flag_ec_resampling = FLAGS_ec_resampling;
    if (flag_ec_resampling != -1) {
      if (!(((flag_ec_resampling & (flag_ec_resampling - 1)) == 0) &&
            flag_ec_resampling <= 8)) {
        std::cerr << "Invalid --ec_resampling. "
                     "Valid values are {-1, 1, 2, 4, 8}.\n";
        return EXIT_FAILURE;
      }
      JxlEncoderFrameSettingsSetOption(
          jxl_encoder_frame_settings,
          JXL_ENC_FRAME_SETTING_EXTRA_CHANNEL_RESAMPLING, flag_ec_resampling);
    }

    JxlEncoderFrameSettingsSetOption(jxl_encoder_frame_settings,
                                     JXL_ENC_FRAME_SETTING_ALREADY_DOWNSAMPLED,
                                     FLAGS_already_downsampled);

    JxlEncoderFrameSettingsSetOption(jxl_encoder_frame_settings,
                                     JXL_ENC_FRAME_SETTING_PHOTON_NOISE,
                                     FLAGS_photon_noise);
    // Removed: --noise (superseded by: --photon_noise).

    JxlEncoderSetFrameDistance(jxl_encoder_frame_settings, FLAGS_distance);

    const int32_t flag_center_x = FLAGS_center_x;
    if (flag_center_x != -1) {
      JxlEncoderFrameSettingsSetOption(
          jxl_encoder_frame_settings,
          JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_X, flag_center_x);
    }
    const int32_t flag_center_y = FLAGS_center_y;
    if (flag_center_y != -1) {
      JxlEncoderFrameSettingsSetOption(
          jxl_encoder_frame_settings,
          JXL_ENC_FRAME_SETTING_GROUP_ORDER_CENTER_Y, flag_center_y);
    }
  }  // Processing flags.

  if (FLAGS_add_jpeg_frame) {
    jxl::PaddedBytes jpeg_data;
    if (!ReadFile(filename_in, &jpeg_data)) {
      std::cerr << "Reading image data failed.\n";
      return EXIT_FAILURE;
    }
    if (JXL_ENC_SUCCESS != JxlEncoderAddJPEGFrame(jxl_encoder_frame_settings,
                                                  jpeg_data.data(),
                                                  jpeg_data.size())) {
      std::cerr << "JxlEncoderAddJPEGFrame() failed.\n";
      return EXIT_FAILURE;
    }
  } else {  // Do JxlEncoderAddImageFrame().
    jxl::extras::PackedPixelFile ppf;
    jxl::Status status = LoadInput(filename_in, ppf);
    if (!status) {
      // TODO(tfish): Fix such status handling throughout.  We should
      // have more detail available about what went wrong than what we
      // currently share with the caller.
      std::cerr << "Loading input file failed.\n";
      return EXIT_FAILURE;
    }
    if (ppf.frames.size() < 1) {
      std::cerr << "No frames on input file.\n";
      return EXIT_FAILURE;
    }
    const jxl::extras::PackedFrame& pframe = ppf.frames[0];
    const jxl::extras::PackedImage& pimage = pframe.color;
    JxlPixelFormat ppixelformat = pimage.format;

    {  // JxlEncoderSetBasicInfo
      JxlBasicInfo basic_info;
      JxlEncoderInitBasicInfo(&basic_info);
      basic_info.xsize = pimage.xsize;
      basic_info.ysize = pimage.ysize;
      basic_info.bits_per_sample = 32;
      basic_info.exponent_bits_per_sample = 8;
      basic_info.uses_original_profile = JXL_FALSE;
      if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(jxl_encoder, &basic_info)) {
        std::cerr << "JxlEncoderSetBasicInfo() failed.\n";
        return EXIT_FAILURE;
      }
    }
    {  // JxlEncoderSetColorEncoding
      JxlColorEncoding color_encoding = {};
      JxlColorEncodingSetToSRGB(&color_encoding,
                                /*is_gray=*/ppixelformat.num_channels < 3);
      if (JXL_ENC_SUCCESS !=
          JxlEncoderSetColorEncoding(jxl_encoder, &color_encoding)) {
        std::cerr << "JxlEncoderSetColorEncoding() failed.\n";
        return EXIT_FAILURE;
      }
    }
    jxl::Status enc_status =
        JxlEncoderAddImageFrame(jxl_encoder_frame_settings, &ppixelformat,
                                pimage.pixels(), pimage.pixels_size);
    if (JXL_ENC_SUCCESS != enc_status) {
      // TODO(tfish): Fix such status handling throughout.  We should
      // have more detail available about what went wrong than what we
      // currently share with the caller.
      std::cerr << "JxlEncoderAddImageFrame() failed.\n";
      return EXIT_FAILURE;
    }
  }
  JxlEncoderCloseInput(jxl_encoder);
  if (!fetch_jxl_encoded_image(jxl_encoder,
                               &managed_jxl_encoder.compressed_buffer_,
                               &managed_jxl_encoder.compressed_buffer_size_,
                               &managed_jxl_encoder.compressed_buffer_used_)) {
    std::cerr << "Fetching encoded image failed.\n";
    return EXIT_FAILURE;
  }
  std::cerr << "DDD fetched image. Buffer size="
            << managed_jxl_encoder.compressed_buffer_size_
            << ", used=" << managed_jxl_encoder.compressed_buffer_used_
            << std::endl;
  if (!write_jxl_file(managed_jxl_encoder.compressed_buffer_,
                      managed_jxl_encoder.compressed_buffer_used_,
                      filename_out)) {
    std::cerr << "Writing output file failed: " << filename_out << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
