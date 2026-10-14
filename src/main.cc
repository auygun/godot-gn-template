#include "src/main.h"

#include <godot_cpp/core/class_db.hpp>

#include "src/fps_character.h"
#include "src/hud.h"
#include "src/target.h"
#include "src/weapon.h"

namespace godot {

Main::Main() = default;

Main::~Main() = default;

void Main::_ready() {
  FpsCharacter* player =
      Object::cast_to<FpsCharacter>(get_node_or_null("Player"));
  hud_ = Object::cast_to<Hud>(get_node_or_null("Hud"));
  weapon_ = player ? Object::cast_to<Weapon>(player->get_node_or_null("Weapon"))
                   : nullptr;

  if (weapon_) {
    weapon_->connect("hit", callable_mp(this, &Main::OnWeaponHit));
    weapon_->connect("ammo_changed", callable_mp(this, &Main::OnAmmoChanged));
  }

  Node* targets = get_node_or_null("Targets");
  if (targets) {
    for (int i = 0; i < targets->get_child_count(); i++) {
      Target* target = Object::cast_to<Target>(targets->get_child(i));
      if (target) {
        target->connect("died", callable_mp(this, &Main::OnTargetDied));
      }
    }
  }

  if (hud_ && weapon_) {
    hud_->UpdateAmmo(weapon_->ammo(), weapon_->reserve_ammo(), false);
  }
}

// static
void Main::_bind_methods() {}

void Main::OnWeaponHit(Object* collider) {
  if (hud_) {
    hud_->ShowHitmarker();
  }
}

void Main::OnAmmoChanged(int ammo, int reserve, bool reloading) {
  if (hud_) {
    hud_->UpdateAmmo(ammo, reserve, reloading);
  }
}

void Main::OnTargetDied() {
  if (hud_) {
    hud_->AddScore(100);
  }
}

}  // namespace godot
