#ifndef SRC_REGISTER_TYPES_H_
#define SRC_REGISTER_TYPES_H_

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

void InitializeFpsModule(godot::ModuleInitializationLevel level);
void UninitializeFpsModule(godot::ModuleInitializationLevel level);

#endif  // SRC_REGISTER_TYPES_H_
