extends SceneTree

# Generates the test level, scenes/levels/test_level.tscn: a blockout of a
# spawn room (with jump-test blocks), a corridor, and an arena with cover,
# crates and a raised platform up a flight of stairs. Every piece is a box.
# Each area of the level is one StaticBody3D with one mesh (with lightmap
# UVs, for LightmapGI) and a box collision shape per piece. Deterministic.
# Needs an editor build of Godot (lightmap_unwrap). Run from the project
# root:
#
#   godot --headless --path . --script tools/gen_level.gd [-- <out.tscn>]
#
# Physics layers (named in project.godot):
#   1 world: blocks the player and shots.
#   2 player_clip: blocks the player only. The stairs are a smooth,
#     invisible ramp to the player, so it doesn't snag on the steps...
#   3 shot_only: blocks shots only. ...while the shots hit the steps.

const WORLD := 1
const PLAYER_CLIP := 2
const SHOT_ONLY := 4
const LAYER_BODY_NAMES := {PLAYER_CLIP: "PlayerClip", SHOT_ONLY: "ShotOnly"}

const WALL := 0.4  # wall thickness, meters
const ROOM_HEIGHT := 4.0
const DOOR_WIDTH := 3.0
const DOOR_HEIGHT := 3.0
const RAIL := 0.06  # railing bar thickness
const RAIL_HEIGHT := 1.0
const POST_SPACING := 1.2  # at most, between railing posts
const LIGHTMAP_TEXEL := 0.2  # meters

# The pieces of the area being built: {name, xform, size, mat, layer}. A
# null mat is invisible (collision only).
var _pieces: Array[Dictionary] = []
# BoxShape3D by size, shared by all pieces of that size.
var _shapes := {}


func _init():
	var args := OS.get_cmdline_user_args()
	var out_path: String = args[0] if args.size() > 0 else "res://scenes/levels/test_level.tscn"

	var ground := material("ground", Color(0.45, 0.48, 0.42))
	var wall := material("wall", Color(0.62, 0.6, 0.56))
	var cover := material("cover", Color(0.5, 0.5, 0.52))
	var crate := material("crate", Color(0.55, 0.4, 0.25))
	var platform := material("platform", Color(0.42, 0.44, 0.5))
	var metal := material("metal", Color(0.25, 0.27, 0.3))

	var root := Node3D.new()
	root.name = "TestLevel"

	# Under the whole level, out to the outer walls. Its sides and bottom
	# are never seen (no mesh faces below the ground, see add_box_faces).
	box("Ground", Vector3(-14.4, -1, -40.4), Vector3(14.4, 0, 10.4), ground)
	add_area(root, "Ground")

	# Spawn room, with a door north to the corridor. Jump height is ~1.2 m,
	# so the tallest block is out of reach.
	room(-6, 6, 0, 10, wall, true, false)
	box("Block050", Vector3(-6, 0, 1.5), Vector3(-5, 0.5, 2.5), cover)
	box("Block100", Vector3(-6, 0, 3.5), Vector3(-5, 1.0, 4.5), cover)
	box("Block150", Vector3(-6, 0, 5.5), Vector3(-5, 1.5, 6.5), cover)
	add_area(root, "Spawn")

	# Corridor, door high and roofed, from the spawn room's north wall to
	# the arena's south wall.
	var d := DOOR_WIDTH / 2
	box("WallWest", Vector3(-d - WALL, 0, -11.6), Vector3(-d, DOOR_HEIGHT, -0.4), wall)
	box("WallEast", Vector3(d, 0, -11.6), Vector3(d + WALL, DOOR_HEIGHT, -0.4), wall)
	box("Ceiling", Vector3(-d - WALL, DOOR_HEIGHT, -11.6),
			Vector3(d + WALL, DOOR_HEIGHT + WALL, -0.4), wall)
	add_area(root, "Corridor")

	# Arena, with a door south to the corridor.
	room(-14, 14, -40, -12, wall, false, true)
	box("CoverWest", Vector3(-9, 0, -19.4), Vector3(-5, 1, -19), cover)
	box("CoverEast", Vector3(4, 0, -21.4), Vector3(8, 1, -21), cover)
	box("CoverNorth", Vector3(-6, 0, -35.4), Vector3(-2, 1, -35), cover)
	box("CoverNorthSide", Vector3(-6, 0, -35), Vector3(-5.6, 1, -32), cover)
	# Blocks the view across the arena from the door.
	box("Block", Vector3(-1, 0, -27), Vector3(1, ROOM_HEIGHT, -25), wall)
	box("Pillar", Vector3(9, 0, -25), Vector3(10, ROOM_HEIGHT, -24), wall)
	box("Crate1", Vector3(-11, 0, -31), Vector3(-10, 1, -30), crate)
	box("Crate2", Vector3(-10, 0, -31), Vector3(-9, 1, -30), crate)
	box("Crate3", Vector3(-10.5, 1, -31), Vector3(-9.5, 2, -30), crate)
	box("Crate4", Vector3(3, 0, -35), Vector3(4, 1, -34), crate)

	# Platform in the north-east corner, 2 m up 10 rises of stairs (the
	# platform's edge is the last) on its south side, x 10..13.
	box("Platform", Vector3(6, 0, -40), Vector3(14, 2, -32), platform)
	var rises := 10
	var rise := 2.0 / rises
	var tread := 0.5
	for i in rises - 1:
		var front := -32.0 + tread * (rises - 1 - i)
		box("Step%d" % (i + 1), Vector3(10, 0, front - tread),
				Vector3(13, rise * (i + 1), front), platform, SHOT_ONLY)
	# Along the steps' front edges, from the ground half a tread before the
	# first step to the platform's edge.
	ramp_z("StairsRamp", 10, 13, -32.0 + tread * rises, 0, -32, 2, PLAYER_CLIP)
	# Along the platform's open edges, low enough to jump over.
	var h := RAIL / 2
	railing("RailingWest", [Vector3(6 + h, 2, -40 + h), Vector3(6 + h, 2, -32 - h),
			Vector3(10 - h, 2, -32 - h)], metal)
	railing("RailingEast", [Vector3(13 + h, 2, -32 - h), Vector3(14 - h, 2, -32 - h)], metal)
	add_area(root, "Arena")

	var packed := PackedScene.new()
	var err := packed.pack(root)
	if err == OK:
		DirAccess.make_dir_recursive_absolute(out_path.get_base_dir())
		err = ResourceSaver.save(packed, out_path)
	if err == OK:
		err = strip_unique_ids(out_path)
	root.free()
	if err != OK:
		push_error("gen_level: failed to save %s: %s" % [out_path, error_string(err)])
	quit(0 if err == OK else 1)


# Saving gives every node a random unique_id. They're optional (the
# hand-written scenes have none), so drop them to keep the output the same
# between runs.
func strip_unique_ids(path: String) -> Error:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		return FileAccess.get_open_error()
	var re := RegEx.create_from_string(" unique_id=\\d+")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if not file:
		return FileAccess.get_open_error()
	file.store_string(re.sub(text, "", true))
	return OK


func material(id: String, color: Color) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = color
	# Fixed ids in the .tscn, so the output doesn't change between runs.
	mat.resource_scene_unique_id = "StandardMaterial3D_" + id
	return mat


# An axis-aligned box between two opposite corners.
func box(name: String, from: Vector3, to: Vector3, mat: Material, layer := WORLD) -> void:
	_pieces.append({
		name = name,
		xform = Transform3D(Basis(), (from + to) * 0.5),
		size = (to - from).abs(),
		mat = mat,
		layer = layer,
	})


# An invisible ramp WALL thick going north (-Z), its top surface rising
# from (z0, y0) to (z1, y1), x0..x1 wide.
func ramp_z(name: String, x0: float, x1: float, z0: float, y0: float, z1: float, y1: float,
		layer: int) -> void:
	var run := z0 - z1
	var rise := y1 - y0
	var basis := Basis(Vector3.RIGHT, atan2(rise, run))
	var top := Vector3(x0 + x1, y0 + y1, z0 + z1) * 0.5
	_pieces.append({
		name = name,
		xform = Transform3D(basis, top - basis.y * (WALL / 2)),
		size = Vector3(x1 - x0, WALL, sqrt(run * run + rise * rise)),
		mat = null,
		layer = layer,
	})


# Four walls around the interior x0..x1, z0..z1 (z0 is north), outside it,
# with optional doors in the north and south walls.
func room(x0: float, x1: float, z0: float, z1: float, mat: Material, north_door: bool,
		south_door: bool) -> void:
	wall_x("WallNorth", x0 - WALL, x1 + WALL, z0 - WALL, z0, mat, north_door)
	wall_x("WallSouth", x0 - WALL, x1 + WALL, z1, z1 + WALL, mat, south_door)
	box("WallWest", Vector3(x0 - WALL, 0, z0), Vector3(x0, ROOM_HEIGHT, z1), mat)
	box("WallEast", Vector3(x1, 0, z0), Vector3(x1 + WALL, ROOM_HEIGHT, z1), mat)


# A wall along X, x0..x1 long and z0..z1 thick, with a door at x = 0 if
# door is set.
func wall_x(name: String, x0: float, x1: float, z0: float, z1: float, mat: Material,
		door: bool) -> void:
	if not door:
		box(name, Vector3(x0, 0, z0), Vector3(x1, ROOM_HEIGHT, z1), mat)
		return
	var d := DOOR_WIDTH / 2
	box(name + "Left", Vector3(x0, 0, z0), Vector3(-d, ROOM_HEIGHT, z1), mat)
	box(name + "Right", Vector3(d, 0, z0), Vector3(x1, ROOM_HEIGHT, z1), mat)
	box(name + "Lintel", Vector3(-d, DOOR_HEIGHT, z0), Vector3(d, ROOM_HEIGHT, z1), mat)


# A railing along a path of floor points (each step along X or Z): posts
# at the points and at most POST_SPACING apart, under a top and a middle
# rail.
func railing(name: String, points: Array[Vector3], mat: Material) -> void:
	var h := RAIL / 2
	var posts: Array[Vector3] = [points[0]]
	var rails := 0
	for i in range(1, points.size()):
		var a := points[i - 1]
		var b := points[i]
		var n := ceili(a.distance_to(b) / POST_SPACING)
		for s in range(1, n + 1):
			posts.append(a.lerp(b, float(s) / n))
		for y: float in [RAIL_HEIGHT / 2, RAIL_HEIGHT]:
			rails += 1
			box("%sRail%d" % [name, rails], a + Vector3(-h, y - RAIL, -h),
					b + Vector3(h, y, h), mat)
	for i in posts.size():
		box("%sPost%d" % [name, i + 1], posts[i] - Vector3(h, 0, h),
				posts[i] + Vector3(h, RAIL_HEIGHT, h), mat)


# Makes the pieces so far into the area name under root: a StaticBody3D on
# the world layer with the mesh and the collision shapes, with a child body
# for each other layer used. Starts the next area.
func add_area(root: Node3D, name: String) -> void:
	var body := StaticBody3D.new()
	body.name = name
	add_owned(root, root, body)

	var surfaces := {}  # Material -> SurfaceTool
	for p in _pieces:
		if p.mat:
			add_box_faces(surfaces, p)
	if not surfaces.is_empty():
		var mesh := ArrayMesh.new()
		mesh.resource_scene_unique_id = "ArrayMesh_" + name
		for st: SurfaceTool in surfaces.values():
			st.index()
			st.generate_tangents()
			st.commit(mesh)
		mesh.lightmap_unwrap(Transform3D.IDENTITY, LIGHTMAP_TEXEL)
		var instance := MeshInstance3D.new()
		instance.name = "Mesh"
		instance.mesh = mesh
		add_owned(root, body, instance)

	var bodies := {WORLD: body}
	for p in _pieces:
		if not bodies.has(p.layer):
			var layer_body := StaticBody3D.new()
			layer_body.name = LAYER_BODY_NAMES[p.layer]
			layer_body.collision_layer = p.layer
			add_owned(root, body, layer_body)
			bodies[p.layer] = layer_body
		var shape := CollisionShape3D.new()
		shape.name = p.name
		shape.transform = p.xform
		shape.shape = box_shape(p.size)
		add_owned(root, bodies[p.layer], shape)
	_pieces.clear()


func add_owned(root: Node, parent: Node, node: Node) -> void:
	parent.add_child(node)
	node.owner = root


func box_shape(size: Vector3) -> BoxShape3D:
	size = size.snappedf(0.001)
	if not _shapes.has(size):
		var shape := BoxShape3D.new()
		shape.size = size
		shape.resource_scene_unique_id = "BoxShape3D_%d" % (_shapes.size() + 1)
		_shapes[size] = shape
	return _shapes[size]


# Adds the faces of the piece p to the SurfaceTool for its material, except
# faces at or below the ground (other than tops), which are never seen. UVs
# are world space, 1 per meter, projected along the face's axis.
func add_box_faces(surfaces: Dictionary, p: Dictionary) -> void:
	var st: SurfaceTool = surfaces.get(p.mat)
	if not st:
		st = SurfaceTool.new()
		st.begin(Mesh.PRIMITIVE_TRIANGLES)
		st.set_material(p.mat)
		surfaces[p.mat] = st
	var xform: Transform3D = p.xform
	var half: Vector3 = p.size * 0.5
	for axis in 3:
		for sign: float in [1.0, -1.0]:
			# u x v = n, so the corners below go counterclockwise seen from
			# the front.
			var n := Vector3.ZERO
			n[axis] = sign
			var u := Vector3.ZERO
			var v := Vector3.ZERO
			var j := (axis + 1) % 3
			var k := (axis + 2) % 3
			if sign < 0:
				var t := j
				j = k
				k = t
			u[j] = half[j]
			v[k] = half[k]
			var c := n * half[axis]
			var corners: Array[Vector3] = []
			var below := true
			for q in [c - u - v, c + u - v, c + u + v, c - u + v]:
				var world: Vector3 = xform * q
				corners.append(world)
				below = below and world.y <= 0.001
			var normal := (xform.basis * n).normalized()
			if below and normal.y < 0.999:
				continue
			# Godot's front faces are clockwise.
			for i in [0, 2, 1, 0, 3, 2]:
				st.set_normal(normal)
				st.set_uv(face_uv(corners[i], normal))
				st.add_vertex(corners[i])


func face_uv(pos: Vector3, normal: Vector3) -> Vector2:
	var a := normal.abs()
	if a.y >= a.x and a.y >= a.z:
		return Vector2(pos.x, pos.z)
	if a.x >= a.z:
		return Vector2(pos.z, -pos.y)
	return Vector2(pos.x, -pos.y)
