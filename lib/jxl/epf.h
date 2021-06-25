// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef LIB_JXL_EPF_H_
#define LIB_JXL_EPF_H_

// Fast SIMD "in-loop" edge preserving filter (adaptive, nonlinear).

#include <stddef.h>

#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/dec_cache.h"
#include "lib/jxl/filters.h"
#include "lib/jxl/passes_state.h"

namespace jxl {

// 4 * (sqrt(0.5)-1), so that Weight(sigma) = 0.5.
static constexpr float kInvSigmaNum = -1.1715728752538099024f;

// Fills the `state->filter_weights.sigma` image with the precomputed sigma
// values in the area inside `block_rect`. Accesses the AC strategy, quant field
// and epf_sharpness fields in the corresponding positions.
void ComputeSigma(const Rect& block_rect, PassesDecoderState* state);

// Same as ApplyFilters, but only prepares the pipeline (which is returned and
// must be run by the caller on -lf.Padding() to image_rect.ysize() +
// lf.Padding()).
FilterPipeline* PrepareFilterPipeline(
    PassesDecoderState* dec_state, const Rect& image_rect, const Image3F& input,
    const Rect& input_rect, size_t image_ysize, size_t thread,
    Image3F* JXL_RESTRICT out, const Rect& output_rect);

}  // namespace jxl

#endif  // LIB_JXL_EPF_H_
