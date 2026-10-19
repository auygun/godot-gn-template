#include "src/net/replicate.h"

#include <algorithm>
#include <cmath>

#include <godot_cpp/core/math.hpp>
#include <godot_cpp/core/math_defs.hpp>

#include "src/net/byte_stream.h"

namespace godot::net {

namespace {

// Masks are written as a varint: at most 64 fields per view.
constexpr size_t kMaxViewFields = 64;

constexpr float kYawScale = 65536.0f / static_cast<float>(Math::TAU);
constexpr float kPitchScale = 16384.0f / static_cast<float>(Math::PI / 2.0);
constexpr float kUnitScale = 127.0f;

int16_t QuantizeLinear(float value, float scale, long limit) {
  if (!std::isfinite(value)) {
    return 0;
  }
  return static_cast<int16_t>(
      std::clamp(std::lround(value * scale), -limit, limit));
}

uint16_t QuantizeAngle(float value) {
  if (!std::isfinite(value)) {
    return 0;
  }
  // Wrapped: any number of turns.
  return static_cast<uint16_t>(
      static_cast<int32_t>(std::lround(value * kYawScale)) & 0xffff);
}

float AngleFrom(uint16_t value) {
  // -pi..pi.
  return static_cast<float>(static_cast<int16_t>(value)) / kYawScale;
}

template <class T>
T Load(const uint8_t* data, const FieldInfo& field) {
  T value;
  std::memcpy(&value, data + field.offset, sizeof(T));
  return value;
}

template <class T>
void Store(uint8_t* data, const FieldInfo& field, const T& value) {
  std::memcpy(data + field.offset, &value, sizeof(T));
}

size_t FieldSize(FieldType type) {
  switch (type) {
    case FieldType::kBool:
    case FieldType::kU8:
      return 1;
    case FieldType::kU16:
      return 2;
    case FieldType::kU32:
    case FieldType::kI32:
    case FieldType::kFloat:
      return 4;
    case FieldType::kDouble:
    case FieldType::kVector2:
      return 8;
    case FieldType::kVector3:
      return 12;
    case FieldType::kQuaternion:
      return 16;
  }
  return 0;
}

// Whether the view gets the field through its codec, or exact.
bool Coded(View view) {
  return view != View::kOwner;
}

float QuantizeFloat(float value, const Codec& codec) {
  switch (codec.kind) {
    case CodecKind::kAngle:
      return AngleFrom(QuantizeAngle(value));
    case CodecKind::kPitch:
      return QuantizeLinear(value, kPitchScale, 16384) / kPitchScale;
    case CodecKind::kUnit:
      return QuantizeLinear(value, kUnitScale, 127) / kUnitScale;
    default:
      return value;
  }
}

// Smallest three: the largest component's index in the top 2 bits of the
// first word, the others in 15 bits each (scaled from +-1/sqrt(2)), with
// the sign chosen so the largest is positive (q and -q are the same
// rotation).
constexpr float kRotationScale = 16383.0f / static_cast<float>(Math::SQRT12);

void EncodeRotation(const Quaternion& rotation, uint16_t out[3]) {
  Quaternion q = rotation;
  float length = q.length();
  if (!std::isfinite(length) || length < 1e-6f) {
    q = Quaternion();
  } else {
    q = q / length;
  }
  float c[4] = {q.x, q.y, q.z, q.w};
  int largest = 0;
  for (int i = 1; i < 4; i++) {
    if (std::abs(c[i]) > std::abs(c[largest])) {
      largest = i;
    }
  }
  float sign = c[largest] < 0.0f ? -1.0f : 1.0f;
  int k = 0;
  for (int i = 0; i < 4; i++) {
    if (i == largest) {
      continue;
    }
    long v =
        std::clamp(std::lround(c[i] * sign * kRotationScale), -16383L, 16383L);
    // 15 bits, offset to unsigned.
    out[k++] = static_cast<uint16_t>(v + 16383);
  }
  out[0] |= static_cast<uint16_t>((largest & 1) << 15);
  out[1] |= static_cast<uint16_t>((largest >> 1) << 15);
}

Quaternion DecodeRotation(const uint16_t in[3]) {
  int largest = (in[0] >> 15) | ((in[1] >> 15) << 1);
  float c[4];
  float sum = 0.0f;
  int k = 0;
  for (int i = 0; i < 4; i++) {
    if (i == largest) {
      continue;
    }
    float v =
        (static_cast<float>(in[k++] & 0x7fff) - 16383.0f) / kRotationScale;
    c[i] = v;
    sum += v * v;
  }
  c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
  return Quaternion(c[0], c[1], c[2], c[3]).normalized();
}

// The field as the view receives it.
void QuantizeField(uint8_t* data, const FieldInfo& field) {
  const Codec& codec = field.codec;
  switch (field.type) {
    case FieldType::kFloat:
      Store(data, field, QuantizeFloat(Load<float>(data, field), codec));
      break;
    case FieldType::kVector2: {
      Vector2 v = Load<Vector2>(data, field);
      Store(data, field,
            Vector2(QuantizeFloat(v.x, codec), QuantizeFloat(v.y, codec)));
      break;
    }
    case FieldType::kVector3: {
      if (codec.kind != CodecKind::kPosition) {
        break;
      }
      Vector3 v = Load<Vector3>(data, field);
      float s = codec.param;
      Store(data, field,
            Vector3(QuantizeLinear(v.x, s, 32767) / s,
                    QuantizeLinear(v.y, s, 32767) / s,
                    QuantizeLinear(v.z, s, 32767) / s));
      break;
    }
    case FieldType::kQuaternion:
      if (codec.kind == CodecKind::kRotation) {
        uint16_t words[3];
        EncodeRotation(Load<Quaternion>(data, field), words);
        Store(data, field, DecodeRotation(words));
      }
      break;
    case FieldType::kU16:
      if (codec.kind == CodecKind::kLowByte) {
        Store(data, field,
              static_cast<uint16_t>(Load<uint16_t>(data, field) & 0xff));
      }
      break;
    case FieldType::kU32:
      if (codec.kind == CodecKind::kLowByte) {
        Store(data, field, Load<uint32_t>(data, field) & 0xffu);
      }
      break;
    case FieldType::kI32:
      if (codec.kind == CodecKind::kLowByte) {
        Store(data, field, Load<int32_t>(data, field) & 0xff);
      }
      break;
    default:
      break;
  }
}

void WriteFloat(float value, const Codec& codec, ByteWriter* writer) {
  switch (codec.kind) {
    case CodecKind::kAngle:
      writer->U16(QuantizeAngle(value));
      break;
    case CodecKind::kPitch:
      writer->I16(QuantizeLinear(value, kPitchScale, 16384));
      break;
    case CodecKind::kUnit:
      writer->U8(static_cast<uint8_t>(
          static_cast<int8_t>(QuantizeLinear(value, kUnitScale, 127))));
      break;
    default:
      writer->Float(value);
      break;
  }
}

float ReadFloat(const Codec& codec, ByteReader* reader) {
  switch (codec.kind) {
    case CodecKind::kAngle:
      return AngleFrom(reader->U16());
    case CodecKind::kPitch:
      return reader->I16() / kPitchScale;
    case CodecKind::kUnit:
      return static_cast<int8_t>(reader->U8()) / kUnitScale;
    default:
      return reader->Float();
  }
}

// A changed field's value (a bool has none: the change flips it).
void WriteField(const uint8_t* data,
                const FieldInfo& field,
                bool coded,
                ByteWriter* writer) {
  Codec exact;
  const Codec& codec = coded ? field.codec : exact;
  switch (field.type) {
    case FieldType::kBool:
      break;
    case FieldType::kU8:
      writer->U8(Load<uint8_t>(data, field));
      break;
    case FieldType::kU16:
    case FieldType::kU32:
    case FieldType::kI32: {
      uint32_t value = field.type == FieldType::kU16
                           ? Load<uint16_t>(data, field)
                       : field.type == FieldType::kU32
                           ? Load<uint32_t>(data, field)
                           : static_cast<uint32_t>(Load<int32_t>(data, field));
      if (codec.kind == CodecKind::kLowByte) {
        writer->U8(static_cast<uint8_t>(value));
      } else {
        writer->VarUint(value);
      }
      break;
    }
    case FieldType::kFloat:
      WriteFloat(Load<float>(data, field), codec, writer);
      break;
    case FieldType::kDouble: {
      double value = Load<double>(data, field);
      uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      writer->U32(static_cast<uint32_t>(bits));
      writer->U32(static_cast<uint32_t>(bits >> 32));
      break;
    }
    case FieldType::kVector2: {
      Vector2 v = Load<Vector2>(data, field);
      WriteFloat(v.x, codec, writer);
      WriteFloat(v.y, codec, writer);
      break;
    }
    case FieldType::kVector3: {
      Vector3 v = Load<Vector3>(data, field);
      if (codec.kind == CodecKind::kPosition) {
        float s = codec.param;
        writer->I16(QuantizeLinear(v.x, s, 32767));
        writer->I16(QuantizeLinear(v.y, s, 32767));
        writer->I16(QuantizeLinear(v.z, s, 32767));
      } else {
        writer->Float(v.x);
        writer->Float(v.y);
        writer->Float(v.z);
      }
      break;
    }
    case FieldType::kQuaternion: {
      Quaternion q = Load<Quaternion>(data, field);
      if (codec.kind == CodecKind::kRotation) {
        uint16_t words[3];
        EncodeRotation(q, words);
        for (uint16_t word : words) {
          writer->U16(word);
        }
      } else {
        writer->Float(q.x);
        writer->Float(q.y);
        writer->Float(q.z);
        writer->Float(q.w);
      }
      break;
    }
  }
}

void ReadField(const FieldInfo& field,
               bool coded,
               ByteReader* reader,
               uint8_t* data) {
  Codec exact;
  const Codec& codec = coded ? field.codec : exact;
  switch (field.type) {
    case FieldType::kBool:
      Store(data, field, !Load<bool>(data, field));
      break;
    case FieldType::kU8:
      Store(data, field, reader->U8());
      break;
    case FieldType::kU16:
    case FieldType::kU32:
    case FieldType::kI32: {
      uint64_t value =
          codec.kind == CodecKind::kLowByte ? reader->U8() : reader->VarUint();
      if (field.type == FieldType::kU16) {
        if (value > 0xffff) {
          reader->Fail();
        }
        Store(data, field, static_cast<uint16_t>(value));
      } else {
        if (value > 0xffffffffu) {
          reader->Fail();
        }
        if (field.type == FieldType::kU32) {
          Store(data, field, static_cast<uint32_t>(value));
        } else {
          Store(data, field,
                static_cast<int32_t>(static_cast<uint32_t>(value)));
        }
      }
      break;
    }
    case FieldType::kFloat:
      Store(data, field, ReadFloat(codec, reader));
      break;
    case FieldType::kDouble: {
      uint64_t bits = reader->U32();
      bits |= static_cast<uint64_t>(reader->U32()) << 32;
      double value;
      std::memcpy(&value, &bits, sizeof(value));
      Store(data, field, value);
      break;
    }
    case FieldType::kVector2: {
      float x = ReadFloat(codec, reader);
      float y = ReadFloat(codec, reader);
      Store(data, field, Vector2(x, y));
      break;
    }
    case FieldType::kVector3: {
      Vector3 v;
      if (codec.kind == CodecKind::kPosition) {
        float s = codec.param;
        v.x = reader->I16() / s;
        v.y = reader->I16() / s;
        v.z = reader->I16() / s;
      } else {
        v.x = reader->Float();
        v.y = reader->Float();
        v.z = reader->Float();
      }
      Store(data, field, v);
      break;
    }
    case FieldType::kQuaternion: {
      Quaternion q;
      if (codec.kind == CodecKind::kRotation) {
        uint16_t words[3];
        for (uint16_t& word : words) {
          word = reader->U16();
        }
        q = DecodeRotation(words);
      } else {
        q.x = reader->Float();
        q.y = reader->Float();
        q.z = reader->Float();
        q.w = reader->Float();
      }
      Store(data, field, q);
      break;
    }
  }
}

// Whether the view would decode the field the same from both. Floats
// compare as numbers (so -0 equals 0).
bool SameField(const uint8_t* a,
               const uint8_t* b,
               const FieldInfo& field,
               bool coded) {
  if (!coded) {
    switch (field.type) {
      case FieldType::kFloat:
        return Load<float>(a, field) == Load<float>(b, field);
      case FieldType::kDouble:
        return Load<double>(a, field) == Load<double>(b, field);
      case FieldType::kVector2:
        return Load<Vector2>(a, field) == Load<Vector2>(b, field);
      case FieldType::kVector3:
        return Load<Vector3>(a, field) == Load<Vector3>(b, field);
      case FieldType::kQuaternion:
        return Load<Quaternion>(a, field) == Load<Quaternion>(b, field);
      case FieldType::kBool:
        return Load<bool>(a, field) == Load<bool>(b, field);
      default:
        return std::memcmp(a + field.offset, b + field.offset,
                           FieldSize(field.type)) == 0;
    }
  }
  uint8_t qa[kMaxStateSize];
  uint8_t qb[kMaxStateSize];
  size_t end = field.offset + FieldSize(field.type);
  std::memcpy(qa, a, end);
  std::memcpy(qb, b, end);
  QuantizeField(qa, field);
  QuantizeField(qb, field);
  return SameField(qa, qb, field, false);
}

uint32_t Mix(uint32_t hash, uint32_t value) {
  // FNV-1a over the value's bytes.
  for (int i = 0; i < 4; i++) {
    hash ^= (value >> (i * 8)) & 0xff;
    hash *= 16777619u;
  }
  return hash;
}

bool ViewCarries(View view, uint32_t flags) {
  switch (view) {
    case View::kOwner:
      return flags & kOwner;
    case View::kOthers:
      return flags & kOthers;
    case View::kAll:
      return true;
  }
  return false;
}

bool SameFlaggedFields(const Box& a, const Box& b, uint32_t flag) {
  const Layout& layout = *a.layout();
  DEV_ASSERT(b.layout() == &layout);
  for (const FieldInfo& field : layout.fields) {
    if ((field.flags & flag) && !SameField(a.data(), b.data(), field, false)) {
      return false;
    }
  }
  return true;
}

template <class T>
void DrawField(const Box& prev,
               const Box& cur,
               float fraction,
               float dt,
               const FieldInfo& field,
               Box* out) {
  auto draw = reinterpret_cast<DrawFn<T>>(field.draw);
  Store(out->data(), field,
        draw(Load<T>(prev.data(), field), Load<T>(cur.data(), field), fraction,
             dt));
}

// Each eased field, by type.
template <class F>
void ForEachEased(const Layout& layout, F&& f) {
  for (const FieldInfo& field : layout.fields) {
    if (field.ease) {
      f(field);
    }
  }
}

}  // namespace

void FinishLayout(Layout* layout) {
  uint32_t hash = 2166136261u;
  hash = Mix(hash, static_cast<uint32_t>(layout->fields.size()));
  for (size_t i = 0; i < layout->fields.size(); i++) {
    const FieldInfo& field = layout->fields[i];
    hash = Mix(hash, static_cast<uint32_t>(field.type));
    hash = Mix(hash, static_cast<uint32_t>(field.codec.kind));
    uint32_t param;
    std::memcpy(&param, &field.codec.param, sizeof(param));
    hash = Mix(hash, param);
    hash = Mix(hash, field.flags);
    // The owner compares (and takes) these from what it receives.
    CRASH_COND_MSG(
        (field.flags & (kPredicted | kServer)) && !(field.flags & kOwner),
        "kPredicted and kServer fields need kOwner");
    for (View view : {View::kOwner, View::kOthers, View::kAll}) {
      if (ViewCarries(view, field.flags)) {
        layout->view_fields[static_cast<int>(view)].push_back(
            static_cast<uint8_t>(i));
      }
    }
  }
  for (const auto& fields : layout->view_fields) {
    CRASH_COND_MSG(fields.size() > kMaxViewFields,
                   "a replicated struct has too many fields");
  }
  layout->hash = hash;
}

void Box::Reset(const Layout& layout) {
  layout_ = &layout;
  std::memset(data_, 0, sizeof(data_));
  std::memcpy(data_, layout.defaults.data(), layout.size);
}

void WriteDelta(const Box& value,
                const Box& base,
                View view,
                ByteWriter* writer) {
  const Layout& layout = *value.layout();
  DEV_ASSERT(base.layout() == &layout);
  bool coded = Coded(view);
  const std::vector<uint8_t>& fields =
      layout.view_fields[static_cast<int>(view)];
  uint64_t mask = 0;
  for (size_t i = 0; i < fields.size(); i++) {
    if (!SameField(value.data(), base.data(), layout.fields[fields[i]],
                   coded)) {
      mask |= uint64_t{1} << i;
    }
  }
  writer->VarUint(mask);
  for (size_t i = 0; i < fields.size(); i++) {
    if (mask & (uint64_t{1} << i)) {
      WriteField(value.data(), layout.fields[fields[i]], coded, writer);
    }
  }
}

bool ReadDelta(View view, ByteReader* reader, Box* value) {
  const Layout& layout = *value->layout();
  bool coded = Coded(view);
  const std::vector<uint8_t>& fields =
      layout.view_fields[static_cast<int>(view)];
  uint64_t mask = reader->VarUint();
  if (fields.size() < kMaxViewFields && (mask >> fields.size()) != 0) {
    return false;
  }
  for (size_t i = 0; i < fields.size() && reader->ok(); i++) {
    if (mask & (uint64_t{1} << i)) {
      ReadField(layout.fields[fields[i]], coded, reader, value->data());
    }
  }
  return reader->ok();
}

void Quantize(View view, Box* value) {
  if (!Coded(view)) {
    return;
  }
  const Layout& layout = *value->layout();
  for (uint8_t index : layout.view_fields[static_cast<int>(view)]) {
    QuantizeField(value->data(), layout.fields[index]);
  }
}

bool SameView(const Box& a, const Box& b, View view) {
  const Layout& layout = *a.layout();
  if (b.layout() != &layout) {
    return false;
  }
  bool coded = Coded(view);
  for (uint8_t index : layout.view_fields[static_cast<int>(view)]) {
    if (!SameField(a.data(), b.data(), layout.fields[index], coded)) {
      return false;
    }
  }
  return true;
}

bool PredictionMatches(const Box& predicted, const Box& actual) {
  const Layout& layout = *predicted.layout();
  DEV_ASSERT(actual.layout() == &layout);
  const uint8_t* p = predicted.data();
  const uint8_t* a = actual.data();
  for (const FieldInfo& field : layout.fields) {
    if (!(field.flags & kPredicted)) {
      continue;
    }
    float tolerance = field.codec.tolerance;
    bool same;
    switch (field.type) {
      case FieldType::kFloat:
        same =
            std::abs(Load<float>(p, field) - Load<float>(a, field)) < tolerance;
        break;
      case FieldType::kDouble:
        same = std::abs(Load<double>(p, field) - Load<double>(a, field)) <
               tolerance;
        break;
      case FieldType::kVector2:
        same = Load<Vector2>(p, field).distance_squared_to(
                   Load<Vector2>(a, field)) < tolerance * tolerance;
        break;
      case FieldType::kVector3:
        same = Load<Vector3>(p, field).distance_squared_to(
                   Load<Vector3>(a, field)) < tolerance * tolerance;
        break;
      case FieldType::kQuaternion: {
        // q and -q are the same rotation.
        float dot = Load<Quaternion>(p, field).dot(Load<Quaternion>(a, field));
        same = 1.0f - std::abs(dot) < tolerance * tolerance;
        break;
      }
      default:
        same = SameField(p, a, field, false);
        break;
    }
    if (!same) {
      return false;
    }
  }
  return true;
}

bool SameServerFields(const Box& a, const Box& b) {
  return SameFlaggedFields(a, b, kServer);
}

bool SameTeleportFields(const Box& a, const Box& b) {
  return SameFlaggedFields(a, b, kTeleport);
}

void Blend(const Box& from, const Box& to, float t, Box* out) {
  *out = from;
  const Layout& layout = *from.layout();
  if (to.layout() != &layout) {
    return;
  }
  if (!SameTeleportFields(from, to)) {
    return;
  }
  uint8_t* o = out->data();
  for (const FieldInfo& field : layout.fields) {
    if (!(field.flags & kBlend)) {
      continue;
    }
    bool angle = field.codec.kind == CodecKind::kAngle;
    switch (field.type) {
      case FieldType::kFloat: {
        float a = Load<float>(from.data(), field);
        float b = Load<float>(to.data(), field);
        Store(o, field,
              angle ? Math::lerp_angle(a, b, t) : Math::lerp(a, b, t));
        break;
      }
      case FieldType::kDouble:
        Store(
            o, field,
            Math::lerp(Load<double>(from.data(), field),
                       Load<double>(to.data(), field), static_cast<double>(t)));
        break;
      case FieldType::kVector2:
        Store(o, field,
              Load<Vector2>(from.data(), field)
                  .lerp(Load<Vector2>(to.data(), field), t));
        break;
      case FieldType::kVector3:
        Store(o, field,
              Load<Vector3>(from.data(), field)
                  .lerp(Load<Vector3>(to.data(), field), t));
        break;
      case FieldType::kQuaternion: {
        Quaternion a = Load<Quaternion>(from.data(), field);
        Quaternion b = Load<Quaternion>(to.data(), field);
        if (a.is_normalized() && b.is_normalized()) {
          Store(o, field, a.slerp(b, t));
        }
        break;
      }
      default:
        break;
    }
  }
}

void DrawFields(const Box& prev,
                const Box& cur,
                float fraction,
                float dt,
                Box* out) {
  *out = cur;
  const Layout& layout = *cur.layout();
  if (prev.layout() != &layout) {
    return;
  }
  for (const FieldInfo& field : layout.fields) {
    if (!field.draw) {
      continue;
    }
    switch (field.type) {
      case FieldType::kFloat:
        DrawField<float>(prev, cur, fraction, dt, field, out);
        break;
      case FieldType::kDouble:
        DrawField<double>(prev, cur, fraction, dt, field, out);
        break;
      case FieldType::kVector2:
        DrawField<Vector2>(prev, cur, fraction, dt, field, out);
        break;
      case FieldType::kVector3:
        DrawField<Vector3>(prev, cur, fraction, dt, field, out);
        break;
      case FieldType::kQuaternion:
        DrawField<Quaternion>(prev, cur, fraction, dt, field, out);
        break;
      default:
        break;
    }
  }
}

void SubtractEased(const Box& a, const Box& b, Box* out) {
  const Layout& layout = *a.layout();
  DEV_ASSERT(b.layout() == &layout);
  out->Reset(layout);
  std::memset(out->data(), 0, layout.size);
  uint8_t* o = out->data();
  ForEachEased(layout, [&](const FieldInfo& field) {
    switch (field.type) {
      case FieldType::kFloat: {
        float x = Load<float>(a.data(), field);
        float y = Load<float>(b.data(), field);
        Store(o, field,
              field.codec.kind == CodecKind::kAngle
                  ? Math::angle_difference(y, x)
                  : x - y);
        break;
      }
      case FieldType::kDouble:
        Store(o, field,
              Load<double>(a.data(), field) - Load<double>(b.data(), field));
        break;
      case FieldType::kVector2:
        Store(o, field,
              Load<Vector2>(a.data(), field) - Load<Vector2>(b.data(), field));
        break;
      case FieldType::kVector3:
        Store(o, field,
              Load<Vector3>(a.data(), field) - Load<Vector3>(b.data(), field));
        break;
      default:
        break;
    }
  });
}

void AddEased(const Box& diff, float age, Box* out) {
  const Layout& layout = *diff.layout();
  DEV_ASSERT(out->layout() == &layout);
  uint8_t* o = out->data();
  ForEachEased(layout, [&](const FieldInfo& field) {
    float share = field.ease(age);
    switch (field.type) {
      case FieldType::kFloat:
        Store(o, field,
              Load<float>(o, field) + Load<float>(diff.data(), field) * share);
        break;
      case FieldType::kDouble:
        Store(o, field,
              Load<double>(o, field) + Load<double>(diff.data(), field) *
                                           static_cast<double>(share));
        break;
      case FieldType::kVector2:
        Store(o, field,
              Load<Vector2>(o, field) +
                  Load<Vector2>(diff.data(), field) * share);
        break;
      case FieldType::kVector3:
        Store(o, field,
              Load<Vector3>(o, field) +
                  Load<Vector3>(diff.data(), field) * share);
        break;
      default:
        break;
    }
  });
}

bool StillEasing(const Layout& layout, float age) {
  bool easing = false;
  ForEachEased(layout, [&](const FieldInfo& field) {
    easing = easing || field.ease(age) > 1e-3f;
  });
  return easing;
}

bool ExceedsSnap(const Box& offset) {
  bool exceeds = false;
  ForEachEased(*offset.layout(), [&](const FieldInfo& field) {
    if (field.snap <= 0.0f) {
      return;
    }
    double size = 0.0;
    switch (field.type) {
      case FieldType::kFloat:
        size = std::abs(Load<float>(offset.data(), field));
        break;
      case FieldType::kDouble:
        size = std::abs(Load<double>(offset.data(), field));
        break;
      case FieldType::kVector2:
        size = Load<Vector2>(offset.data(), field).length();
        break;
      case FieldType::kVector3:
        size = Load<Vector3>(offset.data(), field).length();
        break;
      default:
        break;
    }
    exceeds = exceeds || size > field.snap;
  });
  return exceeds;
}

}  // namespace godot::net
