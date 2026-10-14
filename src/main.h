#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class Hud;
class Weapon;

class Main : public Node {
  GDCLASS(Main, Node);

 public:
  Main();
  ~Main() override;

  void _ready() override;

 protected:
  static void _bind_methods();

 private:
  void OnWeaponHit(Object* collider);
  void OnAmmoChanged(int ammo, int reserve, bool reloading);
  void OnTargetDied();

  Weapon* weapon_ = nullptr;
  Hud* hud_ = nullptr;
};

}  // namespace godot

#endif  // SRC_MAIN_H_
