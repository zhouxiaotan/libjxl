// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "jxl/gain_map.h"

#include <jxl/encode.h>
#include <stdint.h>

#include <fstream>

#include "lib/jxl/base/span.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/fields.h"
#include "lib/jxl/test_utils.h"
#include "lib/jxl/testing.h"

namespace {
bool ColorEncodingsEqual(const JxlColorEncoding& lhs,
                         const JxlColorEncoding& rhs) {
  return lhs.color_space == rhs.color_space &&
         lhs.white_point == rhs.white_point &&
         std::memcmp(lhs.white_point_xy, rhs.white_point_xy,
                     sizeof(lhs.white_point_xy)) == 0 &&
         lhs.primaries == rhs.primaries &&
         std::memcmp(lhs.primaries_red_xy, rhs.primaries_red_xy,
                     sizeof(lhs.primaries_red_xy)) == 0 &&
         std::memcmp(lhs.primaries_green_xy, rhs.primaries_green_xy,
                     sizeof(lhs.primaries_green_xy)) == 0 &&
         std::memcmp(lhs.primaries_blue_xy, rhs.primaries_blue_xy,
                     sizeof(lhs.primaries_blue_xy)) == 0 &&
         lhs.transfer_function == rhs.transfer_function &&
         lhs.gamma == rhs.gamma && lhs.rendering_intent == rhs.rendering_intent;
}
}  // namespace

namespace jxl {
namespace {

TEST(GainMapTest, GainMapRoundtrip) {
  JxlMemoryManager* const memory_manager = jxl::test::MemoryManager();

  JxlGainMapBundle orig_bundle;

  // Initialize the bundle with some test data
  orig_bundle.jhgm_version = 0;

  // TODO(firsching): Replace with more realistic data
  const char* metadata_str =
      "placeholder gain map metadata, fill with actual example after (ISO "
      "21496-1) is finalized";
  std::vector<uint8_t> gain_map_metadata(metadata_str,
                                         metadata_str + strlen(metadata_str));
  orig_bundle.gain_map_metadata_size = gain_map_metadata.size();
  orig_bundle.gain_map_metadata = gain_map_metadata.data();

  JxlColorEncoding color_encoding = {};
  JxlColorEncodingSetToLinearSRGB(&color_encoding, /*is_gray=*/JXL_FALSE);

  orig_bundle.color_encoding = color_encoding;

  // Use the ICC profile from GetIccTestProfile
  jxl::IccBytes icc_profile = test::GetIccTestProfile();
  std::vector<uint8_t> alt_icc(icc_profile.begin(), icc_profile.end());
  orig_bundle.color_encoding_size = 1;
  orig_bundle.alt_icc = alt_icc.data();
  orig_bundle.alt_icc_size = alt_icc.size();

  const char* gain_map_str =
      "placeholder for an actual naked JPEG XL codestream";
  std::vector<uint8_t> gain_map(gain_map_str,
                                gain_map_str + strlen(gain_map_str));
  orig_bundle.gain_map_size = gain_map.size();
  orig_bundle.gain_map = gain_map.data();

  size_t bundle_size;
  size_t color_encoding_size;
  size_t compressed_icc_size;
  ASSERT_TRUE(JxlGainMapGetBundleSize(memory_manager, &orig_bundle,
                                      &bundle_size, &color_encoding_size,
                                      &compressed_icc_size));

  fprintf(stderr, "size without: %zu, bundle_size: %zu\n", color_encoding_size,
          bundle_size);
  ASSERT_TRUE(color_encoding_size < bundle_size);
  orig_bundle.color_encoding_size = color_encoding_size;

  EXPECT_EQ(bundle_size, 534);

  std::vector<uint8_t> buffer(bundle_size);
  size_t bytes_written;
  ASSERT_TRUE(JxlGainMapWriteBundle(memory_manager, &orig_bundle, buffer.data(),
                                    buffer.size(), &bytes_written));
  EXPECT_EQ(bytes_written, bundle_size);

  EXPECT_EQ(buffer[0], orig_bundle.jhgm_version);

  JxlGainMapBundle output_bundle;
  JxlGainMapGetBufferSizes(memory_manager, &output_bundle, buffer.data(),
                           buffer.size());
  EXPECT_EQ(output_bundle.gain_map_size, gain_map.size());
  EXPECT_EQ(output_bundle.gain_map_metadata_size, gain_map_metadata.size());
  EXPECT_EQ(output_bundle.alt_icc_size, icc_profile.size());
  std::vector<uint8_t> output_metadata(output_bundle.gain_map_metadata_size);
  std::vector<uint8_t> output_gain_map(output_bundle.gain_map_size);
  std::vector<uint8_t> output_alt_icc(output_bundle.alt_icc_size);
  output_bundle.gain_map_metadata = output_metadata.data();
  output_bundle.gain_map = output_gain_map.data();
  output_bundle.alt_icc = output_alt_icc.data();
  ASSERT_TRUE(JxlGainMapReadBundle(memory_manager, &output_bundle,
                                   buffer.data(), buffer.size(),
                                   /*bytes_read=*/nullptr));
  EXPECT_EQ(orig_bundle.jhgm_version, output_bundle.jhgm_version);
  EXPECT_EQ(orig_bundle.color_encoding_size, orig_bundle.color_encoding_size);
  EXPECT_TRUE(ColorEncodingsEqual(orig_bundle.color_encoding,
                                  output_bundle.color_encoding));
  EXPECT_TRUE(std::equal(gain_map_metadata.begin(), gain_map_metadata.end(),
                         output_metadata.begin()));
  EXPECT_TRUE(
      std::equal(gain_map.begin(), gain_map.end(), output_gain_map.begin()));
  EXPECT_TRUE(
      std::equal(alt_icc.begin(), alt_icc.end(), output_alt_icc.begin()));
}

}  // namespace
}  // namespace jxl
