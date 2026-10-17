#include "src/net/byte_stream.h"

namespace godot::net {

ByteWriter::ByteWriter() = default;

ByteWriter::~ByteWriter() = default;

void ByteWriter::VarUint(uint64_t value) {
  while (value >= 0x80) {
    U8(static_cast<uint8_t>(value) | 0x80);
    value >>= 7;
  }
  U8(static_cast<uint8_t>(value));
}

PackedByteArray ByteWriter::Finish() const {
  PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(bytes_.size()));
  std::memcpy(bytes.ptrw(), bytes_.data(), bytes_.size());
  return bytes;
}

ByteReader::ByteReader(const PackedByteArray& bytes)
    : data_(bytes.ptr()), size_(static_cast<size_t>(bytes.size())) {}

ByteReader::~ByteReader() = default;

uint64_t ByteReader::VarUint() {
  uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    uint8_t byte = U8();
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      return value;
    }
  }
  // Longer than any value.
  ok_ = false;
  return 0;
}

}  // namespace godot::net
