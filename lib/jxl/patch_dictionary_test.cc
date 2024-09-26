// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <jxl/cms.h>
#include <jxl/memory_manager.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lib/extras/codec.h"
#include "lib/extras/packed_image_convert.h"
#include "lib/jxl/base/override.h"
#include "lib/jxl/base/span.h"
#include "lib/jxl/enc_params.h"
#include "lib/jxl/image_test_utils.h"
#include "lib/jxl/test_memory_manager.h"
#include "lib/jxl/test_utils.h"
#include "lib/jxl/testing.h"

namespace jxl {
namespace {

using ::jxl::test::ButteraugliDistance;
using ::jxl::test::GetImage;
using ::jxl::test::ReadTestData;
using ::jxl::test::Roundtrip;

TEST(PatchDictionaryTest, GrayscaleModular) {
  const std::vector<uint8_t> orig = ReadTestData("jxl/grayscale_patches.png");
  extras::PackedPixelFile ppf;
  jxl::extras::ColorHints color_hints;
  ASSERT_TRUE(DecodeBytes(Bytes(orig), color_hints, &ppf));

  extras::JXLCompressParams cparams = jxl::test::CompressParamsForLossless();
  cparams.AddOption(JXL_ENC_FRAME_SETTING_PATCHES, 1);

  extras::PackedPixelFile ppf2;
  // Without patches: ~25k
  size_t compressed_size = Roundtrip(ppf, cparams, {}, nullptr, &ppf2);
  JXL_EXPECT_OK(&compressed_size);
  EXPECT_LE(compressed_size, 8000u);
  JXL_TEST_ASSIGN_OR_DIE(ImageF rgb, GetImage(ppf));
  JXL_TEST_ASSIGN_OR_DIE(ImageF rgb2, GetImage(ppf));
  JXL_TEST_ASSERT_OK(VerifyRelativeError(rgb, rgb2, 1e-7f, 0, _));
}

TEST(PatchDictionaryTest, GrayscaleVarDCT) {
  JxlMemoryManager* memory_manager = jxl::test::MemoryManager();
  const std::vector<uint8_t> orig = ReadTestData("jxl/grayscale_patches.png");
  CodecInOut io{memory_manager};
  ASSERT_TRUE(SetFromBytes(Bytes(orig), &io));

  CompressParams cparams;
  cparams.patches = jxl::Override::kOn;

  CodecInOut io2{memory_manager};
  // Without patches: ~47k
  size_t compressed_size;
  JXL_EXPECT_OK(Roundtrip(&io, cparams, {}, &io2, _, &compressed_size));
  EXPECT_LE(compressed_size, 14000u);
  // Without patches: ~1.2
  EXPECT_LE(ButteraugliDistance(io.frames, io2.frames, ButteraugliParams(),
                                *JxlGetDefaultCms(),
                                /*distmap=*/nullptr),
            1.1);
}

}  // namespace
}  // namespace jxl
