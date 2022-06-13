// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jxl/render_pipeline/stage_from_linear.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jxl/render_pipeline/stage_from_linear.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jxl/fast_math-inl.h"
#include "lib/jxl/sanitizers.h"
#include "lib/jxl/transfer_functions-inl.h"

HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {

template <typename Op>
struct PerChannelOp {
  explicit PerChannelOp(Op op) : op(op) {}
  template <typename D, typename T>
  void Transform(D d, T* r, T* g, T* b) const {
    *r = op.Transform(d, *r);
    *g = op.Transform(d, *g);
    *b = op.Transform(d, *b);
  }

  Op op;
};
template <typename Op>
PerChannelOp<Op> MakePerChannelOp(Op&& op) {
  return PerChannelOp<Op>(std::forward<Op>(op));
}

struct OpLinear {
  template <typename D, typename T>
  T Transform(D d, const T& linear) const {
    return linear;
  }
};

struct OpRgb {
  template <typename D, typename T>
  T Transform(D d, const T& linear) const {
#if JXL_HIGH_PRECISION
    return TF_SRGB().EncodedFromDisplay(d, linear);
#else
    return FastLinearToSRGB(d, linear);
#endif
  }
};

struct OpPq {
  template <typename D, typename T>
  T Transform(D d, const T& linear) const {
    return TF_PQ().EncodedFromDisplay(d, linear);
  }
};

struct OpHlg {
  explicit OpHlg(const float luminances[3], const float intensity_target)
      : luminances(luminances), exponent(1.0f) {
    if (295 <= intensity_target && intensity_target <= 305) {
      apply_inverse_ootf = false;
      return;
    }
    exponent =
        (1 / 1.2f) * std::pow(1.111f, -std::log2(intensity_target * 1e-3f)) - 1;
  }
  template <typename D, typename T>
  void Transform(D d, T* r, T* g, T* b) const {
    if (apply_inverse_ootf) {
      const T luminance = Set(d, luminances[0]) * *r +
                          Set(d, luminances[1]) * *g +
                          Set(d, luminances[2]) * *b;
      const T ratio =
          Min(FastPowf(d, luminance, Set(d, exponent)), Set(d, 1e9));
      *r *= ratio;
      *g *= ratio;
      *b *= ratio;
    }
    *r = TF_HLG().EncodedFromDisplay(d, *r);
    *g = TF_HLG().EncodedFromDisplay(d, *g);
    *b = TF_HLG().EncodedFromDisplay(d, *b);
  }
  bool apply_inverse_ootf = true;
  const float* luminances;
  float exponent;
};

struct Op709 {
  template <typename D, typename T>
  T Transform(D d, const T& linear) const {
    return TF_709().EncodedFromDisplay(d, linear);
  }
};

struct OpGamma {
  const float inverse_gamma;
  template <typename D, typename T>
  T Transform(D d, const T& linear) const {
    return IfThenZeroElse(linear <= Set(d, 1e-5f),
                          FastPowf(d, linear, Set(d, inverse_gamma)));
  }
};

template <typename Op>
class FromLinearStage : public RenderPipelineStage {
 public:
  explicit FromLinearStage(Op op)
      : RenderPipelineStage(RenderPipelineStage::Settings()),
        op_(std::move(op)) {}

  void ProcessRow(const RowInfo& input_rows, const RowInfo& output_rows,
                  size_t xextra, size_t xsize, size_t xpos, size_t ypos,
                  size_t thread_id) const final {
    PROFILER_ZONE("FromLinear");

    const HWY_FULL(float) d;
    const size_t xsize_v = RoundUpTo(xsize, Lanes(d));
    float* JXL_RESTRICT row0 = GetInputRow(input_rows, 0, 0);
    float* JXL_RESTRICT row1 = GetInputRow(input_rows, 1, 0);
    float* JXL_RESTRICT row2 = GetInputRow(input_rows, 2, 0);
    // All calculations are lane-wise, still some might require
    // value-dependent behaviour (e.g. NearestInt). Temporary unpoison last
    // vector tail.
    msan::UnpoisonMemory(row0 + xsize, sizeof(float) * (xsize_v - xsize));
    msan::UnpoisonMemory(row1 + xsize, sizeof(float) * (xsize_v - xsize));
    msan::UnpoisonMemory(row2 + xsize, sizeof(float) * (xsize_v - xsize));
    for (ssize_t x = -xextra; x < (ssize_t)(xsize + xextra); x += Lanes(d)) {
      auto r = Load(d, row0 + x);
      auto g = Load(d, row1 + x);
      auto b = Load(d, row2 + x);
      op_.Transform(d, &r, &g, &b);
      Store(r, d, row0 + x);
      Store(g, d, row1 + x);
      Store(b, d, row2 + x);
    }
    msan::PoisonMemory(row0 + xsize, sizeof(float) * (xsize_v - xsize));
    msan::PoisonMemory(row1 + xsize, sizeof(float) * (xsize_v - xsize));
    msan::PoisonMemory(row2 + xsize, sizeof(float) * (xsize_v - xsize));
  }

  RenderPipelineChannelMode GetChannelMode(size_t c) const final {
    return c < 3 ? RenderPipelineChannelMode::kInPlace
                 : RenderPipelineChannelMode::kIgnored;
  }

  const char* GetName() const override { return "FromLinear"; }

 private:
  Op op_;
};

template <typename Op>
std::unique_ptr<FromLinearStage<Op>> MakeFromLinearStage(Op&& op) {
  return jxl::make_unique<FromLinearStage<Op>>(std::forward<Op>(op));
}

std::unique_ptr<RenderPipelineStage> GetFromLinearStage(
    const OutputEncodingInfo& output_encoding_info) {
  if (output_encoding_info.color_encoding.tf.IsLinear()) {
    return MakeFromLinearStage(MakePerChannelOp(OpLinear()));
  } else if (output_encoding_info.color_encoding.tf.IsSRGB()) {
    return MakeFromLinearStage(MakePerChannelOp(OpRgb()));
  } else if (output_encoding_info.color_encoding.tf.IsPQ()) {
    return MakeFromLinearStage(MakePerChannelOp(OpPq()));
  } else if (output_encoding_info.color_encoding.tf.IsHLG()) {
    return MakeFromLinearStage(OpHlg(output_encoding_info.luminances,
                                     output_encoding_info.intensity_target));
  } else if (output_encoding_info.color_encoding.tf.Is709()) {
    return MakeFromLinearStage(MakePerChannelOp(Op709()));
  } else if (output_encoding_info.color_encoding.tf.IsGamma() ||
             output_encoding_info.color_encoding.tf.IsDCI()) {
    return MakeFromLinearStage(
        MakePerChannelOp(OpGamma{output_encoding_info.inverse_gamma}));
  } else {
    // This is a programming error.
    JXL_ABORT("Invalid target encoding");
  }
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jxl {

HWY_EXPORT(GetFromLinearStage);

std::unique_ptr<RenderPipelineStage> GetFromLinearStage(
    const OutputEncodingInfo& output_encoding_info) {
  return HWY_DYNAMIC_DISPATCH(GetFromLinearStage)(output_encoding_info);
}

}  // namespace jxl
#endif
