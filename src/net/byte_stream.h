#ifndef SRC_NET_BYTE_STREAM_H_
#define SRC_NET_BYTE_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot::net {

// Little-endian (every supported platform is), unaligned.
class ByteWriter {
 public:
  ByteWriter();
  ~ByteWriter();

  void U8(uint8_t value) { bytes_.push_back(value); }
  void U16(uint16_t value) { Raw(&value, sizeof(value)); }
  void I16(int16_t value) { Raw(&value, sizeof(value)); }
  void U32(uint32_t value) { Raw(&value, sizeof(value)); }
  void Float(float value) { Raw(&value, sizeof(value)); }
  // 7 bits a byte, low first: small values take one.
  void VarUint(uint64_t value);

  PackedByteArray Finish() const;

 private:
  void Raw(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
  }

  std::vector<uint8_t> bytes_;
};

// Reads past the end return 0 and fail the reader.
class ByteReader {
 public:
  explicit ByteReader(const PackedByteArray& bytes);
  ~ByteReader();

  uint8_t U8() { return Read<uint8_t>(); }
  uint16_t U16() { return Read<uint16_t>(); }
  int16_t I16() { return Read<int16_t>(); }
  uint32_t U32() { return Read<uint32_t>(); }
  float Float() { return Read<float>(); }
  uint64_t VarUint();

  // Whether every read so far was in bounds (and well formed). Done() also
  // checks that nothing is left over.
  bool ok() const { return ok_; }
  bool Done() const { return ok_ && pos_ == size_; }
  void Fail() { ok_ = false; }

 private:
  template <typename T>
  T Read() {
    T value{};
    if (!ok_ || sizeof(T) > size_ - pos_) {
      ok_ = false;
      return value;
    }
    std::memcpy(&value, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return value;
  }

  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace godot::net

#endif  // SRC_NET_BYTE_STREAM_H_
