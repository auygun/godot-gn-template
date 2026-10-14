#include "src/register_types.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "src/fps_character.h"
#include "src/hud.h"
#include "src/impact.h"
#include "src/main.h"
#include "src/target.h"
#include "src/touch_controls.h"
#include "src/weapon.h"

void InitializeFpsModule(godot::ModuleInitializationLevel level) {
  if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
    return;
  }
  GDREGISTER_RUNTIME_CLASS(godot::FpsCharacter);
  GDREGISTER_RUNTIME_CLASS(godot::Weapon);
  GDREGISTER_RUNTIME_CLASS(godot::Target);
  GDREGISTER_RUNTIME_CLASS(godot::Impact);
  GDREGISTER_RUNTIME_CLASS(godot::Hud);
  GDREGISTER_RUNTIME_CLASS(godot::Main);
  GDREGISTER_RUNTIME_CLASS(godot::TouchControls);
}

void UninitializeFpsModule(godot::ModuleInitializationLevel level) {}

extern "C" {

// Initialization. The name is the entry_symbol in
// gdextension/extension.gdextension.
GDExtensionBool GDE_EXPORT
extension_init(GDExtensionInterfaceGetProcAddress get_proc_address,
               GDExtensionClassLibraryPtr library,
               GDExtensionInitialization* initialization) {
  godot::GDExtensionBinding::InitObject init_obj(get_proc_address, library,
                                                 initialization);

  init_obj.register_initializer(InitializeFpsModule);
  init_obj.register_terminator(UninitializeFpsModule);
  init_obj.set_minimum_library_initialization_level(
      godot::MODULE_INITIALIZATION_LEVEL_SCENE);

  return init_obj.init();
}

}  // extern "C"
