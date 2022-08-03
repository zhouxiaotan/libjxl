// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef LIB_EXTRAS_EXIF_H_
#define LIB_EXTRAS_EXIF_H_

#include <stdint.h>

#include <vector>

#include "jxl/codestream_header.h"

namespace jxl {

// Sets the Exif orientation to the identity, to avoid repeated orientation
void ResetExifOrientation(std::vector<uint8_t>& exif);

void InterpretExif(const std::vector<uint8_t>& exif, JxlBasicInfo* info);

}  // namespace jxl

#endif  // LIB_EXTRAS_EXIF_H_
