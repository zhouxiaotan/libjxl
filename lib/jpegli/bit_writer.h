// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef LIB_JPEGLI_BIT_WRITER_H_
#define LIB_JPEGLI_BIT_WRITER_H_

/* clang-format off */
#include <stdio.h>
#include <jpeglib.h>
#include <stdint.h>
#include <string.h>
/* clang-format on */

#include "lib/jpegli/error.h"
#include "lib/jxl/base/compiler_specific.h"

namespace jpegli {

static JXL_INLINE void WriteOutput(j_compress_ptr cinfo, const uint8_t* buf,
                                   size_t bufsize) {
  size_t pos = 0;
  while (pos < bufsize) {
    if (cinfo->dest->free_in_buffer == 0 &&
        !(*cinfo->dest->empty_output_buffer)(cinfo)) {
      JPEGLI_ERROR("Destination suspension is not supported.");
    }
    size_t len = std::min<size_t>(cinfo->dest->free_in_buffer, bufsize - pos);
    memcpy(cinfo->dest->next_output_byte, buf + pos, len);
    pos += len;
    cinfo->dest->free_in_buffer -= len;
    cinfo->dest->next_output_byte += len;
    if (cinfo->dest->free_in_buffer == 0 &&
        !(*cinfo->dest->empty_output_buffer)(cinfo)) {
      JPEGLI_ERROR("Destination suspension is not supported.");
    }
  }
}

static JXL_INLINE void WriteOutput(j_compress_ptr cinfo,
                                   const std::vector<uint8_t>& bytes) {
  WriteOutput(cinfo, bytes.data(), bytes.size());
}

static JXL_INLINE void WriteOutput(j_compress_ptr cinfo,
                                   std::initializer_list<uint8_t> bytes) {
  WriteOutput(cinfo, bytes.begin(), bytes.size());
}

// Handles the packing of bits into output bytes.
struct JpegBitWriter {
  j_compress_ptr cinfo;
  std::vector<uint8_t> buffer;
  uint8_t* data;
  size_t pos;
  uint64_t put_buffer;
  int free_bits;
  bool healthy;
};

// JpegBitWriter: buffer size
const size_t kJpegBitWriterChunkSize = 16384;

// Returns non-zero if and only if x has a zero byte, i.e. one of
// x & 0xff, x & 0xff00, ..., x & 0xff00000000000000 is zero.
static JXL_INLINE uint64_t HasZeroByte(uint64_t x) {
  return (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
}

static JXL_INLINE void JpegBitWriterInit(JpegBitWriter* bw,
                                         j_compress_ptr cinfo) {
  bw->cinfo = cinfo;
  bw->buffer.resize(kJpegBitWriterChunkSize);
  bw->data = bw->buffer.data();
  bw->pos = 0;
  bw->put_buffer = 0;
  bw->free_bits = 64;
  bw->healthy = true;
}

static JXL_INLINE void EmptyBitWriterBuffer(JpegBitWriter* bw) {
  WriteOutput(bw->cinfo, bw->data, bw->pos);
  bw->data = bw->buffer.data();
  bw->pos = 0;
}

static JXL_INLINE void Reserve(JpegBitWriter* bw, size_t n_bytes) {
  if (JXL_UNLIKELY((bw->pos + n_bytes) > kJpegBitWriterChunkSize)) {
    EmptyBitWriterBuffer(bw);
  }
}

/**
 * Writes the given byte to the output, writes an extra zero if byte is 0xFF.
 *
 * This method is "careless" - caller must make sure that there is enough
 * space in the output buffer. Emits up to 2 bytes to buffer.
 */
static JXL_INLINE void EmitByte(JpegBitWriter* bw, int byte) {
  bw->data[bw->pos++] = byte;
  if (byte == 0xFF) bw->data[bw->pos++] = 0;
}

static JXL_INLINE void DischargeBitBuffer(JpegBitWriter* bw) {
  // At this point we are ready to emit the bytes of put_buffer to the output.
  // The JPEG format requires that after every 0xff byte in the entropy
  // coded section, there is a zero byte, therefore we first check if any of
  // the bytes of put_buffer is 0xFF.
  Reserve(bw, 16);
  if (HasZeroByte(~bw->put_buffer)) {
    // We have a 0xFF byte somewhere, examine each byte and append a zero
    // byte if necessary.
    EmitByte(bw, (bw->put_buffer >> 56) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 48) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 40) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 32) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 24) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 16) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 8) & 0xFF);
    EmitByte(bw, (bw->put_buffer >> 0) & 0xFF);
  } else {
    // We don't have any 0xFF bytes, output all 6 bytes without checking.
    bw->data[bw->pos] = (bw->put_buffer >> 56) & 0xFF;
    bw->data[bw->pos + 1] = (bw->put_buffer >> 48) & 0xFF;
    bw->data[bw->pos + 2] = (bw->put_buffer >> 40) & 0xFF;
    bw->data[bw->pos + 3] = (bw->put_buffer >> 32) & 0xFF;
    bw->data[bw->pos + 4] = (bw->put_buffer >> 24) & 0xFF;
    bw->data[bw->pos + 5] = (bw->put_buffer >> 16) & 0xFF;
    bw->data[bw->pos + 6] = (bw->put_buffer >> 8) & 0xFF;
    bw->data[bw->pos + 7] = (bw->put_buffer >> 0) & 0xFF;
    bw->pos += 8;
  }
}

static JXL_INLINE void WriteBits(JpegBitWriter* bw, int nbits, uint64_t bits) {
  // This is an optimization; if everything goes well,
  // then |nbits| is positive; if non-existing Huffman symbol is going to be
  // encoded, its length should be zero; later encoder could check the
  // "health" of JpegBitWriter.
  if (nbits == 0) {
    bw->healthy = false;
    return;
  }
  bw->free_bits -= nbits;
  if (bw->free_bits < 0) {
    bw->put_buffer <<= (bw->free_bits + nbits);
    bw->put_buffer |= (bits >> -bw->free_bits);
    DischargeBitBuffer(bw);
    bw->free_bits += 64;
    bw->put_buffer = nbits;
  }
  bw->put_buffer <<= nbits;
  bw->put_buffer |= bits;
}

static JXL_INLINE void EmitMarker(JpegBitWriter* bw, int marker) {
  Reserve(bw, 2);
  bw->data[bw->pos++] = 0xFF;
  bw->data[bw->pos++] = marker;
}

static JXL_INLINE void JumpToByteBoundary(JpegBitWriter* bw) {
  size_t n_bits = bw->free_bits & 7u;
  if (n_bits > 0) {
    WriteBits(bw, n_bits, (1u << n_bits) - 1);
  }
  Reserve(bw, 16);
  bw->put_buffer <<= bw->free_bits;
  while (bw->free_bits <= 56) {
    int c = (bw->put_buffer >> 56) & 0xFF;
    EmitByte(bw, c);
    bw->put_buffer <<= 8;
    bw->free_bits += 8;
  }
  bw->put_buffer = 0;
  bw->free_bits = 64;
}

static JXL_INLINE void JpegBitWriterFinish(JpegBitWriter* bw) {
  if (bw->pos == 0) return;
  WriteOutput(bw->cinfo, bw->data, bw->pos);
  bw->data = nullptr;
  bw->pos = 0;
}

}  // namespace jpegli
#endif  // LIB_JPEGLI_BIT_WRITER_H_
