// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package org.jpeg.jpegxl.wrapper;

public enum PixelFormat {
  RGBA_8888(false), // 0
  RGBA_F16(true), // 1
  RGB_888(false), // 2
  RGB_F16(true); // 3

  public final boolean isF16;

  PixelFormat(boolean isF16) {
    this.isF16 = isF16;
  }
}
