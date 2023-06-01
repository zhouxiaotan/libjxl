// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jpegli/bitstream.h"

#include <cmath>

#include "lib/jpegli/bit_writer.h"
#include "lib/jpegli/entropy_coding.h"
#include "lib/jpegli/error.h"
#include "lib/jpegli/memory_manager.h"
#include "lib/jxl/base/bits.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jpegli/bitstream.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jpegli/dct-inl.h"
#include "lib/jpegli/entropy_coding-inl.h"

HWY_BEFORE_NAMESPACE();
namespace jpegli {
namespace HWY_NAMESPACE {

void WriteBlock(int32_t* JXL_RESTRICT block, int32_t* JXL_RESTRICT symbols,
                int32_t* JXL_RESTRICT nonzero_idx, HuffmanCodeTable* dc_huff,
                HuffmanCodeTable* ac_huff, JpegBitWriter* bw) {
  ZigZagShuffle(block);
  int num_nonzeros = CompactBlock(block, nonzero_idx);
  ComputeSymbols(num_nonzeros, nonzero_idx, block, symbols);
  int symbol = symbols[0];
  WriteBits(bw, dc_huff->depth[symbol], dc_huff->code[symbol] | block[0]);
  for (int i = 1; i < num_nonzeros; ++i) {
    symbol = symbols[i];
    while (symbol > 255) {
      WriteBits(bw, ac_huff->depth[0xf0], ac_huff->code[0xf0]);
      symbol -= 256;
    }
    WriteBits(bw, ac_huff->depth[symbol], ac_huff->code[symbol] | block[i]);
  }
  if (nonzero_idx[num_nonzeros - 1] < 1008) {
    WriteBits(bw, ac_huff->depth[0], ac_huff->code[0]);
  }
}

void EncodeiMCURow(j_compress_ptr cinfo, bool streaming) {
  jpeg_comp_master* m = cinfo->master;
  JpegBitWriter* bw = &m->bw;
  int xsize_mcus = DivCeil(cinfo->image_width, 8 * cinfo->max_h_samp_factor);
  int ysize_mcus = DivCeil(cinfo->image_height, 8 * cinfo->max_v_samp_factor);
  int mcu_y = m->next_iMCU_row;
  int32_t* block = m->block_tmp;
  int32_t* symbols = m->block_tmp + DCTSIZE2;
  int32_t* nonzero_idx = m->block_tmp + 3 * DCTSIZE2;
  coeff_t* JXL_RESTRICT last_dc_coeff = m->last_dc_coeff;
  bool output_tokens = streaming && cinfo->optimize_coding;
  bool output_bits = streaming && !cinfo->optimize_coding;
  bool save_coefficients = !streaming;
  JBLOCKARRAY ba[kMaxComponents];
  if (save_coefficients) {
    for (int c = 0; c < cinfo->num_components; ++c) {
      jpeg_component_info* comp = &cinfo->comp_info[c];
      int by0 = mcu_y * comp->v_samp_factor;
      int block_rows_left = comp->height_in_blocks - by0;
      int max_block_rows = std::min(comp->v_samp_factor, block_rows_left);
      ba[c] = (*cinfo->mem->access_virt_barray)(
          reinterpret_cast<j_common_ptr>(cinfo), m->coeff_buffers[c], by0,
          max_block_rows, true);
    }
  }
  if (output_tokens) {
    TokenArray* ta = &m->token_arrays[m->cur_token_array];
    int max_tokens_per_mcu_row = MaxNumTokensPerMCURow(cinfo);
    if (ta->num_tokens + max_tokens_per_mcu_row > m->num_tokens) {
      if (ta->tokens) {
        m->total_num_tokens += ta->num_tokens;
        ++m->cur_token_array;
        ta = &m->token_arrays[m->cur_token_array];
      }
      m->num_tokens =
          EstimateNumTokens(cinfo, mcu_y, ysize_mcus, m->total_num_tokens,
                            max_tokens_per_mcu_row);
      ta->tokens = Allocate<Token>(cinfo, m->num_tokens, JPOOL_IMAGE);
      m->next_token = ta->tokens;
    }
  }
  const float* imcu_start[kMaxComponents];
  for (int c = 0; c < cinfo->num_components; ++c) {
    jpeg_component_info* comp = &cinfo->comp_info[c];
    imcu_start[c] = m->raw_data[c]->Row(mcu_y * comp->v_samp_factor * DCTSIZE);
  }
  const float* qf = nullptr;
  if (m->use_adaptive_quantization) {
    qf = m->quant_field.Row(0);
  }
  const size_t qf_stride = m->quant_field.stride();
  for (int mcu_x = 0; mcu_x < xsize_mcus; ++mcu_x) {
    for (int c = 0; c < cinfo->num_components; ++c) {
      jpeg_component_info* comp = &cinfo->comp_info[c];
      HuffmanCodeTable* dc_huff = &m->huff_tables[comp->dc_tbl_no];
      HuffmanCodeTable* ac_huff = &m->huff_tables[comp->ac_tbl_no + 4];
      float* JXL_RESTRICT qmc = m->quant_mul[c];
      const size_t stride = m->raw_data[c]->stride();
      const int h_factor = m->h_factor[c];
      const float* zero_bias_offset = m->zero_bias_offset[c];
      const float* zero_bias_mul = m->zero_bias_mul[c];
      float aq_strength = 0.0f;
      for (int iy = 0; iy < comp->v_samp_factor; ++iy) {
        for (int ix = 0; ix < comp->h_samp_factor; ++ix) {
          size_t by = mcu_y * comp->v_samp_factor + iy;
          size_t bx = mcu_x * comp->h_samp_factor + ix;
          if (bx >= comp->width_in_blocks || by >= comp->height_in_blocks) {
            if (output_tokens) {
              *m->next_token++ = Token(c, 0, 0);
              *m->next_token++ = Token(c + 4, 0, 0);
            } else if (output_bits) {
              WriteBits(bw, dc_huff->depth[0], dc_huff->code[0]);
              WriteBits(bw, ac_huff->depth[0], ac_huff->code[0]);
            }
            continue;
          }
          if (m->use_adaptive_quantization) {
            aq_strength = qf[iy * qf_stride + bx * h_factor];
          }
          const float* pixels = imcu_start[c] + (iy * stride + bx) * DCTSIZE;
          ComputeCoefficientBlock(pixels, stride, qmc, last_dc_coeff[c],
                                  aq_strength, zero_bias_offset, zero_bias_mul,
                                  m->dct_buffer, block);
          if (save_coefficients) {
            JCOEF* cblock = &ba[c][iy][bx][0];
            for (int k = 0; k < DCTSIZE2; ++k) {
              cblock[k] = block[kJPEGNaturalOrder[k]];
            }
          }
          block[0] -= last_dc_coeff[c];
          last_dc_coeff[c] += block[0];
          if (output_tokens) {
            ComputeTokensForBlock<int32_t, false>(block, 0, c, c + 4,
                                                  &m->next_token);
          } else if (output_bits) {
            WriteBlock(block, symbols, nonzero_idx, dc_huff, ac_huff, bw);
          }
        }
      }
    }
  }
  if (output_tokens) {
    TokenArray* ta = &m->token_arrays[m->cur_token_array];
    ta->num_tokens = m->next_token - ta->tokens;
    ScanTokenInfo* sti = &m->scan_token_info[0];
    sti->num_tokens = m->total_num_tokens + ta->num_tokens;
    sti->restarts[0] = sti->num_tokens;
  }
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jpegli
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jpegli {
namespace {

bool BuildHuffmanCodeTable(const JPEGHuffmanCode& huff, HuffmanCodeTable* table,
                           bool pre_shifted = false) {
  int huff_code[kJpegHuffmanAlphabetSize];
  // +1 for a sentinel element.
  uint32_t huff_size[kJpegHuffmanAlphabetSize + 1];
  int p = 0;
  for (size_t l = 1; l <= kJpegHuffmanMaxBitLength; ++l) {
    int i = huff.counts[l];
    if (p + i > kJpegHuffmanAlphabetSize + 1) {
      return false;
    }
    while (i--) huff_size[p++] = l;
  }

  if (p == 0) {
    return true;
  }

  // Reuse sentinel element.
  int last_p = p - 1;
  huff_size[last_p] = 0;

  int code = 0;
  uint32_t si = huff_size[0];
  p = 0;
  while (huff_size[p]) {
    while ((huff_size[p]) == si) {
      huff_code[p++] = code;
      code++;
    }
    code <<= 1;
    si++;
  }
  for (p = 0; p < last_p; p++) {
    int i = huff.values[p];
    table->depth[i] = huff_size[p];
    table->code[i] = huff_code[p];
    if (pre_shifted) {
      int nbits = i & 0xf;
      table->depth[i] += nbits;
      table->code[i] <<= nbits;
    }
  }
  return true;
}

}  // namespace

void WriteOutput(j_compress_ptr cinfo, const uint8_t* buf, size_t bufsize) {
  size_t pos = 0;
  while (pos < bufsize) {
    if (cinfo->dest->free_in_buffer == 0 &&
        !(*cinfo->dest->empty_output_buffer)(cinfo)) {
      JPEGLI_ERROR("Destination suspension is not supported in markers.");
    }
    size_t len = std::min<size_t>(cinfo->dest->free_in_buffer, bufsize - pos);
    memcpy(cinfo->dest->next_output_byte, buf + pos, len);
    pos += len;
    cinfo->dest->free_in_buffer -= len;
    cinfo->dest->next_output_byte += len;
  }
}

void WriteOutput(j_compress_ptr cinfo, const std::vector<uint8_t>& bytes) {
  WriteOutput(cinfo, bytes.data(), bytes.size());
}

void WriteOutput(j_compress_ptr cinfo, std::initializer_list<uint8_t> bytes) {
  WriteOutput(cinfo, bytes.begin(), bytes.size());
}

void EncodeAPP0(j_compress_ptr cinfo) {
  WriteOutput(cinfo,
              {0xff, 0xe0, 0, 16, 'J', 'F', 'I', 'F', '\0',
               cinfo->JFIF_major_version, cinfo->JFIF_minor_version,
               cinfo->density_unit, static_cast<uint8_t>(cinfo->X_density >> 8),
               static_cast<uint8_t>(cinfo->X_density & 0xff),
               static_cast<uint8_t>(cinfo->Y_density >> 8),
               static_cast<uint8_t>(cinfo->Y_density & 0xff), 0, 0});
}

void EncodeAPP14(j_compress_ptr cinfo) {
  uint8_t color_transform = cinfo->jpeg_color_space == JCS_YCbCr  ? 1
                            : cinfo->jpeg_color_space == JCS_YCCK ? 2
                                                                  : 0;
  WriteOutput(cinfo, {0xff, 0xee, 0, 14, 'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0,
                      0, 0, color_transform});
}

void EncodeSOF(j_compress_ptr cinfo, bool is_baseline) {
  if (cinfo->data_precision != kJpegPrecision) {
    is_baseline = false;
    JPEGLI_ERROR("Unsupported data precision %d", cinfo->data_precision);
  }
  const uint8_t marker = cinfo->progressive_mode ? 0xc2
                         : is_baseline           ? 0xc0
                                                 : 0xc1;
  const size_t n_comps = cinfo->num_components;
  const size_t marker_len = 8 + 3 * n_comps;
  std::vector<uint8_t> data(marker_len + 2);
  size_t pos = 0;
  data[pos++] = 0xFF;
  data[pos++] = marker;
  data[pos++] = marker_len >> 8u;
  data[pos++] = marker_len & 0xFFu;
  data[pos++] = kJpegPrecision;
  data[pos++] = cinfo->image_height >> 8u;
  data[pos++] = cinfo->image_height & 0xFFu;
  data[pos++] = cinfo->image_width >> 8u;
  data[pos++] = cinfo->image_width & 0xFFu;
  data[pos++] = n_comps;
  for (size_t i = 0; i < n_comps; ++i) {
    jpeg_component_info* comp = &cinfo->comp_info[i];
    data[pos++] = comp->component_id;
    data[pos++] = ((comp->h_samp_factor << 4u) | (comp->v_samp_factor));
    const uint32_t quant_idx = comp->quant_tbl_no;
    if (cinfo->quant_tbl_ptrs[quant_idx] == nullptr) {
      JPEGLI_ERROR("Invalid component quant table index %u.", quant_idx);
    }
    data[pos++] = quant_idx;
  }
  WriteOutput(cinfo, data);
}

void EncodeSOS(j_compress_ptr cinfo, int scan_index) {
  const jpeg_scan_info* scan_info = &cinfo->scan_info[scan_index];
  const ScanCodingInfo& sci = cinfo->master->scan_coding_info[scan_index];
  const size_t marker_len = 6 + 2 * scan_info->comps_in_scan;
  std::vector<uint8_t> data(marker_len + 2);
  size_t pos = 0;
  data[pos++] = 0xFF;
  data[pos++] = 0xDA;
  data[pos++] = marker_len >> 8u;
  data[pos++] = marker_len & 0xFFu;
  data[pos++] = scan_info->comps_in_scan;
  for (int i = 0; i < scan_info->comps_in_scan; ++i) {
    int comp_idx = scan_info->component_index[i];
    data[pos++] = cinfo->comp_info[comp_idx].component_id;
    data[pos++] = (sci.dc_tbl_idx[i] << 4u) + (sci.ac_tbl_idx[i] - 4);
  }
  data[pos++] = scan_info->Ss;
  data[pos++] = scan_info->Se;
  data[pos++] = ((scan_info->Ah << 4u) | (scan_info->Al));
  WriteOutput(cinfo, data);
}

void EncodeDHT(j_compress_ptr cinfo, const JPEGHuffmanCode* huffman_codes,
               size_t num_huffman_codes, bool pre_shifted) {
  if (num_huffman_codes == 0) {
    return;
  }

  size_t marker_len = 2;
  for (size_t i = 0; i < num_huffman_codes; ++i) {
    const JPEGHuffmanCode& huff = huffman_codes[i];
    if (huff.sent_table) continue;
    marker_len += kJpegHuffmanMaxBitLength;
    for (size_t j = 0; j <= kJpegHuffmanMaxBitLength; ++j) {
      marker_len += huff.counts[j];
    }
  }
  std::vector<uint8_t> data(marker_len + 2);
  size_t pos = 0;
  data[pos++] = 0xFF;
  data[pos++] = 0xC4;
  data[pos++] = marker_len >> 8u;
  data[pos++] = marker_len & 0xFFu;
  for (size_t i = 0; i < num_huffman_codes; ++i) {
    const JPEGHuffmanCode& huff = huffman_codes[i];
    size_t index = huff.slot_id;
    HuffmanCodeTable* huff_table;
    if (index & 0x10) {
      huff_table = &cinfo->master->huff_tables[index - 12];
    } else {
      huff_table = &cinfo->master->huff_tables[index];
    }
    // TODO(eustas): cache
    // TODO(eustas): set up non-existing symbols
    if (!BuildHuffmanCodeTable(huff, huff_table, pre_shifted)) {
      JPEGLI_ERROR("Failed to build Huffman code table.");
    }
    if (huff.sent_table) continue;
    size_t total_count = 0;
    size_t max_length = 0;
    for (size_t i = 0; i <= kJpegHuffmanMaxBitLength; ++i) {
      if (huff.counts[i] != 0) {
        max_length = i;
      }
      total_count += huff.counts[i];
    }
    --total_count;
    data[pos++] = huff.slot_id;
    for (size_t i = 1; i <= kJpegHuffmanMaxBitLength; ++i) {
      data[pos++] = (i == max_length ? huff.counts[i] - 1 : huff.counts[i]);
    }
    for (size_t i = 0; i < total_count; ++i) {
      data[pos++] = huff.values[i];
    }
  }
  if (marker_len > 2) {
    WriteOutput(cinfo, data);
  }
}

void EncodeDQT(j_compress_ptr cinfo, bool write_all_tables, bool* is_baseline) {
  uint8_t data[4 + NUM_QUANT_TBLS * (1 + 2 * DCTSIZE2)];  // 520 bytes
  size_t pos = 0;
  data[pos++] = 0xFF;
  data[pos++] = 0xDB;
  pos += 2;  // Length will be filled in later.

  int send_table[NUM_QUANT_TBLS] = {};
  if (write_all_tables) {
    for (int i = 0; i < NUM_QUANT_TBLS; ++i) {
      if (cinfo->quant_tbl_ptrs[i]) send_table[i] = 1;
    }
  } else {
    for (int c = 0; c < cinfo->num_components; ++c) {
      send_table[cinfo->comp_info[c].quant_tbl_no] = 1;
    }
  }

  for (int i = 0; i < NUM_QUANT_TBLS; ++i) {
    if (!send_table[i]) continue;
    JQUANT_TBL* quant_table = cinfo->quant_tbl_ptrs[i];
    if (quant_table == nullptr) {
      JPEGLI_ERROR("Missing quant table %d", i);
    }
    int precision = 0;
    for (size_t k = 0; k < DCTSIZE2; ++k) {
      if (quant_table->quantval[k] > 255) {
        precision = 1;
        *is_baseline = false;
      }
    }
    if (quant_table->sent_table) {
      continue;
    }
    data[pos++] = (precision << 4) + i;
    for (size_t j = 0; j < DCTSIZE2; ++j) {
      int val_idx = kJPEGNaturalOrder[j];
      int val = quant_table->quantval[val_idx];
      if (val == 0) {
        JPEGLI_ERROR("Invalid quantval 0.");
      }
      if (precision) {
        data[pos++] = val >> 8;
      }
      data[pos++] = val & 0xFFu;
    }
    quant_table->sent_table = TRUE;
  }
  if (pos > 4) {
    data[2] = (pos - 2) >> 8u;
    data[3] = (pos - 2) & 0xFFu;
    WriteOutput(cinfo, data, pos);
  }
}

void EncodeDRI(j_compress_ptr cinfo) {
  WriteOutput(cinfo, {0xFF, 0xDD, 0, 4,
                      static_cast<uint8_t>(cinfo->restart_interval >> 8),
                      static_cast<uint8_t>(cinfo->restart_interval & 0xFF)});
}

static JXL_INLINE void EmitMarker(JpegBitWriter* bw, int marker) {
  bw->data[bw->pos++] = 0xFF;
  bw->data[bw->pos++] = marker;
}

static JXL_INLINE void WriteSymbol(int symbol, const HuffmanCodeTable* table,
                                   JpegBitWriter* bw) {
  WriteBits(bw, table->depth[symbol], table->code[symbol]);
}

void WriteTokens(j_compress_ptr cinfo, int scan_index, JpegBitWriter* bw) {
  jpeg_comp_master* m = cinfo->master;
  HuffmanCodeTable* huff_tables = &m->huff_tables[0];
  int next_restart_marker = 0;
  const ScanTokenInfo& sti = m->scan_token_info[scan_index];
  size_t num_token_arrays = m->cur_token_array + 1;
  size_t total_tokens = 0;
  size_t restart_idx = 0;
  size_t next_restart = sti.restarts[restart_idx];
  uint8_t* context_map = m->context_map;
  for (size_t i = 0; i < num_token_arrays; ++i) {
    Token* tokens = m->token_arrays[i].tokens;
    size_t num_tokens = m->token_arrays[i].num_tokens;
    if (sti.token_offset < total_tokens + num_tokens &&
        total_tokens < sti.token_offset + sti.num_tokens) {
      size_t start_ix =
          total_tokens < sti.token_offset ? sti.token_offset - total_tokens : 0;
      size_t end_ix = std::min(sti.token_offset + sti.num_tokens - total_tokens,
                               num_tokens);
      size_t cycle_len = bw->len / 8;
      size_t next_cycle = cycle_len;
      for (size_t i = start_ix; i < end_ix; ++i) {
        if (total_tokens + i == next_restart) {
          JumpToByteBoundary(bw);
          EmitMarker(bw, 0xD0 + next_restart_marker);
          next_restart_marker += 1;
          next_restart_marker &= 0x7;
          next_restart = sti.restarts[++restart_idx];
        }
        Token t = tokens[i];
        int nbits = t.symbol & 0xf;
        WriteSymbol(t.symbol, &huff_tables[context_map[t.histo_idx]], bw);
        if (nbits > 0) {
          WriteBits(bw, nbits, t.bits);
        } else {
          nbits = t.symbol >> 4;
          if (nbits > 0 && nbits < 15) {
            WriteBits(bw, nbits, t.bits);
          }
        }
        if (--next_cycle == 0) {
          if (!EmptyBitWriterBuffer(bw)) {
            JPEGLI_ERROR(
                "Output suspension is not supported in "
                "finish_compress");
          }
          next_cycle = cycle_len;
        }
      }
    }
    total_tokens += num_tokens;
  }
}

void WriteACRefinementTokens(j_compress_ptr cinfo, int scan_index,
                             JpegBitWriter* bw) {
  jpeg_comp_master* m = cinfo->master;
  const ScanCodingInfo& sci = m->scan_coding_info[scan_index];
  const ScanTokenInfo& sti = m->scan_token_info[scan_index];
  HuffmanCodeTable* ac_huff = &m->huff_tables[sci.ac_tbl_idx[0]];
  size_t cycle_len = bw->len / 64;
  size_t next_cycle = cycle_len;
  size_t refbit_idx = 0;
  size_t eobrun_idx = 0;
  size_t restart_idx = 0;
  size_t next_restart = sti.restarts[restart_idx];
  int next_restart_marker = 0;
  for (size_t i = 0; i < sti.num_tokens; ++i) {
    if (i == next_restart) {
      JumpToByteBoundary(bw);
      EmitMarker(bw, 0xD0 + next_restart_marker);
      next_restart_marker += 1;
      next_restart_marker &= 0x7;
      next_restart = sti.restarts[++restart_idx];
    }
    RefToken token = sti.tokens[i];
    WriteSymbol(token.symbol & 253, ac_huff, bw);
    int r = token.symbol >> 4;
    int nbits = token.symbol & 0xf;
    if (nbits == 0) {
      if (r < 15) {
        if (r > 0) {
          uint16_t eobrun = sti.eobruns[eobrun_idx++];
          WriteBits(bw, r, eobrun);
        }
      }
    } else {
      WriteBits(bw, 1, (nbits >> 1) & 1);
    }
    for (int j = 0; j < token.refbits; ++j) {
      WriteBits(bw, 1, sti.refbits[refbit_idx++]);
    }
    if (--next_cycle == 0) {
      if (!EmptyBitWriterBuffer(bw)) {
        JPEGLI_ERROR("Output suspension is not supported in finish_compress");
      }
      next_cycle = cycle_len;
    }
  }
}

void WriteDCRefinementBits(j_compress_ptr cinfo, int scan_index,
                           JpegBitWriter* bw) {
  jpeg_comp_master* m = cinfo->master;
  const ScanTokenInfo& sti = m->scan_token_info[scan_index];
  size_t restart_idx = 0;
  size_t next_restart = sti.restarts[restart_idx];
  int next_restart_marker = 0;
  size_t cycle_len = bw->len * 4;
  size_t next_cycle = cycle_len;
  size_t refbit_idx = 0;
  for (size_t i = 0; i < sti.num_tokens; ++i) {
    if (i == next_restart) {
      JumpToByteBoundary(bw);
      EmitMarker(bw, 0xD0 + next_restart_marker);
      next_restart_marker += 1;
      next_restart_marker &= 0x7;
      next_restart = sti.restarts[++restart_idx];
    }
    WriteBits(bw, 1, sti.refbits[refbit_idx++]);
    if (--next_cycle == 0) {
      if (!EmptyBitWriterBuffer(bw)) {
        JPEGLI_ERROR(
            "Output suspension is not supported in "
            "finish_compress");
      }
      next_cycle = cycle_len;
    }
  }
}

void WriteScanData(j_compress_ptr cinfo, int scan_index) {
  const jpeg_scan_info* scan_info = &cinfo->scan_info[scan_index];
  JpegBitWriter* bw = &cinfo->master->bw;
  if (scan_info->Ah == 0) {
    WriteTokens(cinfo, scan_index, bw);
  } else if (scan_info->Ss > 0) {
    WriteACRefinementTokens(cinfo, scan_index, bw);
  } else {
    WriteDCRefinementBits(cinfo, scan_index, bw);
  }
  if (!bw->healthy) {
    JPEGLI_ERROR("Unknown Huffman coded symbol found in scan %d", scan_index);
  }
  JumpToByteBoundary(bw);
  if (!EmptyBitWriterBuffer(bw)) {
    JPEGLI_ERROR("Output suspension is not supported in finish_compress");
  }
}

HWY_EXPORT(EncodeiMCURow);
void EncodeiMCURow(j_compress_ptr cinfo, bool streaming) {
  HWY_DYNAMIC_DISPATCH(EncodeiMCURow)(cinfo, streaming);
}

}  // namespace jpegli
#endif  // HWY_ONCE
