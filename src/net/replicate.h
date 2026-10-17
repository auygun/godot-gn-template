#ifndef SRC_NET_REPLICATE_H_
#define SRC_NET_REPLICATE_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot::net {

class ByteReader;
class ByteWriter;

// Replicated structs declare their fields once, in a member template that
// the framework runs with its own visitors:
//
//   struct TargetState {
//     Vector3 position;
//     bool dead = false;
//
//     template <class V>
//     void Replicate(V& v) {
//       v(position, net::Position(), net::kAll | net::kBlend);
//       v(dead, net::Exact(), net::kAll | net::kTeleport);
//     }
//   };
//
// From that, the framework encodes and decodes the struct (as a delta
// against an earlier value), quantizes it, compares predictions and blends
// snapshots. The struct must be trivially copyable, at most kMaxStateSize
// bytes, and its fields bool, uint8_t, uint16_t, uint32_t, int32_t, float,
// double, Vector2 or Vector3 (nested structs call their own Replicate).
// Entity states, commands and RPC payloads are all declared this way.
//
// Changing a declaration changes the Schema's hash, which peers compare
// when they connect: no protocol version to bump.

// Who receives a field, and what a client does with it.
enum FieldFlags : uint32_t {
  // The entity's owner (the client that commands it) gets the field
  // exact, whatever its codec: it predicts from it.
  kOwner = 1 << 0,
  // Everyone else gets it through its codec.
  kOthers = 1 << 1,
  kAll = kOwner | kOthers,
  // Prediction compares it with the server's (within the codec's
  // tolerance): a difference is a misprediction.
  kPredicted = 1 << 2,
  // Only the server changes it (e.g. health, a score): when the owner's
  // differs from the server's, it takes the server's state (like a
  // misprediction, but not counted as one).
  kServer = 1 << 5,
  // Interpolated between snapshots (float, Vector2, Vector3; angles the
  // short way round). The rest comes from the older snapshot.
  kBlend = 1 << 3,
  // When it changes between two snapshots, nothing of the entity is
  // blended (e.g. a respawn count: a teleport mustn't slide).
  kTeleport = 1 << 4,
};

enum class FieldType : uint8_t {
  kBool,
  kU8,
  kU16,
  kU32,
  kI32,
  kFloat,
  kDouble,
  kVector2,
  kVector3,
};

enum class CodecKind : uint8_t {
  kExact,
  kPosition,
  kAngle,
  kPitch,
  kUnit,
  kLowByte,
};

// How a field is sent to those who get it quantized (kOthers, and every
// field of commands and RPC payloads). tolerance: how far a prediction may
// be off (kPredicted floats and vectors; distance for vectors).
struct Codec {
  CodecKind kind = CodecKind::kExact;
  float param = 0.0f;
  float tolerance = 0.0f;
};

// As is. Integers take as few bytes as their value needs; a bool takes
// none (its change bit flips it).
struct Exact : Codec {
  constexpr explicit Exact(float tolerance = 1e-4f)
      : Codec{CodecKind::kExact, 0.0f, tolerance} {}
  template <class T>
  static constexpr bool kAccepts = true;
};

// A Vector3 in 16-bit steps: scale steps a unit (the default, 2 mm steps
// within +-65 m).
struct Position : Codec {
  constexpr explicit Position(float scale = 500.0f)
      : Codec{CodecKind::kPosition, scale, 1e-3f} {}
  template <class T>
  static constexpr bool kAccepts = std::is_same_v<T, Vector3>;
};

// An angle in radians, any number of turns, in 16 bits; decoded within
// -pi..pi.
struct Angle : Codec {
  constexpr Angle() : Codec{CodecKind::kAngle, 0.0f, 1e-4f} {}
  template <class T>
  static constexpr bool kAccepts = std::is_same_v<T, float>;
};

// An angle within +-pi/2 (a pitch), in 16 bits.
struct Pitch : Codec {
  constexpr Pitch() : Codec{CodecKind::kPitch, 0.0f, 1e-4f} {}
  template <class T>
  static constexpr bool kAccepts = std::is_same_v<T, float>;
};

// Within +-1 (each component of a Vector2), in 8 bits.
struct Unit : Codec {
  constexpr Unit() : Codec{CodecKind::kUnit, 0.0f, 0.0f} {}
  template <class T>
  static constexpr bool kAccepts =
      std::is_same_v<T, float> || std::is_same_v<T, Vector2>;
};

// An integer's low byte, e.g. a counter whose changes are what matter.
struct LowByte : Codec {
  constexpr LowByte() : Codec{CodecKind::kLowByte, 0.0f, 0.0f} {}
  template <class T>
  static constexpr bool kAccepts =
      std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> ||
      std::is_same_v<T, int32_t>;
};

// Which fields an encoding carries, and how.
enum class View : uint8_t {
  // The entity's owner: its kOwner fields, exact.
  kOwner,
  // Everyone else: the kOthers fields, through their codecs.
  kOthers,
  // Every field, through its codec (commands and RPC payloads).
  kAll,
};

struct FieldInfo {
  uint32_t offset = 0;
  FieldType type = FieldType::kBool;
  Codec codec;
  uint32_t flags = 0;
};

// What the framework knows about a replicated struct.
struct Layout {
  // Identifies the C++ type (TypeKey).
  const void* key = nullptr;
  size_t size = 0;
  std::vector<FieldInfo> fields;
  // The fields each View carries (indices into fields), in order: a
  // delta's change mask has a bit for each.
  std::vector<uint8_t> view_fields[3];
  // A default-constructed value.
  std::vector<uint8_t> defaults;
  // Of the fields' types, codecs and flags (not their offsets, which
  // differ between platforms).
  uint32_t hash = 0;
};

inline constexpr size_t kMaxStateSize = 256;

template <class T>
inline constexpr char kTypeKeyTag = 0;
// A unique address per type, without RTTI.
template <class T>
const void* TypeKey() {
  return &kTypeKeyTag<T>;
}

template <class T>
constexpr FieldType FieldTypeOf() {
  if constexpr (std::is_same_v<T, bool>) {
    return FieldType::kBool;
  } else if constexpr (std::is_same_v<T, uint8_t>) {
    return FieldType::kU8;
  } else if constexpr (std::is_same_v<T, uint16_t>) {
    return FieldType::kU16;
  } else if constexpr (std::is_same_v<T, uint32_t>) {
    return FieldType::kU32;
  } else if constexpr (std::is_same_v<T, int32_t>) {
    return FieldType::kI32;
  } else if constexpr (std::is_same_v<T, float>) {
    return FieldType::kFloat;
  } else if constexpr (std::is_same_v<T, double>) {
    return FieldType::kDouble;
  } else if constexpr (std::is_same_v<T, Vector2>) {
    return FieldType::kVector2;
  } else {
    static_assert(std::is_same_v<T, Vector3>, "not a replicable field type");
    return FieldType::kVector3;
  }
}

// The visitor that builds a Layout.
class LayoutBuilder {
 public:
  LayoutBuilder(Layout* layout, const void* base)
      : layout_(layout), base_(static_cast<const uint8_t*>(base)) {}

  template <class T, class C>
  void operator()(T& field, const C& codec, uint32_t flags = kAll) {
    static_assert(std::is_base_of_v<Codec, C>, "not a codec");
    static_assert(C::template kAccepts<T>, "the codec can't encode this type");
    constexpr bool kBlendable =
        std::is_same_v<T, float> || std::is_same_v<T, double> ||
        std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3>;
    CRASH_COND_MSG((flags & kBlend) && !kBlendable,
                   "kBlend on a field that can't be blended");
    FieldInfo info;
    info.offset =
        static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(&field) - base_);
    info.type = FieldTypeOf<T>();
    info.codec = codec;
    info.flags = flags;
    layout_->fields.push_back(info);
  }

 private:
  Layout* layout_;
  const uint8_t* base_;
};

// Fills in view_fields and the hash, and checks the limits.
void FinishLayout(Layout* layout);

template <class T>
const Layout& LayoutOf() {
  static_assert(std::is_trivially_copyable_v<T>,
                "replicated structs must be trivially copyable");
  static_assert(sizeof(T) <= kMaxStateSize, "raise kMaxStateSize");
  static const Layout layout = [] {
    Layout result;
    T value{};
    LayoutBuilder builder(&result, &value);
    value.Replicate(builder);
    result.key = TypeKey<T>();
    result.size = sizeof(T);
    result.defaults.resize(sizeof(T));
    std::memcpy(result.defaults.data(), &value, sizeof(T));
    FinishLayout(&result);
    return result;
  }();
  return layout;
}

// A value of a replicated struct, of any type: what the framework stores
// and passes around (entity states, commands, RPC payloads).
class Box {
 public:
  Box() = default;
  explicit Box(const Layout& layout) { Reset(layout); }

  template <class T>
  static Box Of(const T& value) {
    Box box(LayoutOf<T>());
    std::memcpy(box.data_, &value, sizeof(T));
    return box;
  }

  // A default value of the layout's type.
  void Reset(const Layout& layout);

  // The value, or null if it is of another type.
  template <class T>
  T* Get() {
    return layout_ && layout_->key == TypeKey<T>()
               ? std::launder(reinterpret_cast<T*>(data_))
               : nullptr;
  }
  template <class T>
  const T* Get() const {
    return layout_ && layout_->key == TypeKey<T>()
               ? std::launder(reinterpret_cast<const T*>(data_))
               : nullptr;
  }
  template <class T>
  T& As() {
    T* value = Get<T>();
    CRASH_COND_MSG(!value, "Box of another type");
    return *value;
  }
  template <class T>
  const T& As() const {
    const T* value = Get<T>();
    CRASH_COND_MSG(!value, "Box of another type");
    return *value;
  }

  bool empty() const { return !layout_; }
  const Layout* layout() const { return layout_; }
  uint8_t* data() { return data_; }
  const uint8_t* data() const { return data_; }

 private:
  const Layout* layout_ = nullptr;
  alignas(16) uint8_t data_[kMaxStateSize] = {};
};

// The fields of the view that differ from base's (as the receiver would
// see them), as a change mask and their values. base is of the same type.
void WriteDelta(const Box& value,
                const Box& base,
                View view,
                ByteWriter* writer);
// The other end: *value holds the base, and gets the changes. False if
// the bytes don't decode.
bool ReadDelta(View view, ByteReader* reader, Box* value);
// The view's fields as the receiver will decode them.
void Quantize(View view, Box* value);
// Whether the receiver of the view would see the same in both.
bool SameView(const Box& a, const Box& b, View view);
// Whether every kPredicted field matches, within its tolerance.
bool PredictionMatches(const Box& predicted, const Box& actual);
// Whether every kServer field is the same.
bool SameServerFields(const Box& a, const Box& b);
// Whether every kTeleport field is the same.
bool SameTeleportFields(const Box& a, const Box& b);
// from, moved a fraction t of the way to to (the kBlend fields), unless a
// kTeleport field changed; from and to of the same type.
void Blend(const Box& from, const Box& to, float t, Box* out);

}  // namespace godot::net

#endif  // SRC_NET_REPLICATE_H_
