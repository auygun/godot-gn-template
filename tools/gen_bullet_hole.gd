extends SceneTree

# Generates the bullet hole decal's PBR set, textures/bullet_hole_*.png
# (albedo, normal, ORM). Deterministic (fixed seeds). Run from the project
# root, then re-import:
#
#   godot --headless --path . --script tools/gen_bullet_hole.gd [-- <out_dir>]
#   godot --headless --recovery-mode --path . --import
const N := 256
const NORMAL_STRENGTH := 14.0

func _init():
	var args := OS.get_cmdline_user_args()
	var out_dir: String = args[0] if args.size() > 0 else "res://textures"
	var edge := FastNoiseLite.new()
	edge.seed = 11
	edge.frequency = 0.35
	edge.fractal_octaves = 3
	var grain := FastNoiseLite.new()
	grain.seed = 12
	grain.frequency = 0.03
	grain.fractal_octaves = 4
	var fine := FastNoiseLite.new()
	fine.seed = 13
	fine.frequency = 0.35
	var chips := FastNoiseLite.new()
	chips.seed = 14
	chips.noise_type = FastNoiseLite.TYPE_CELLULAR
	chips.frequency = 0.09
	chips.cellular_return_type = FastNoiseLite.RETURN_CELL_VALUE

	var rng := RandomNumberGenerator.new()
	rng.seed = 5
	var cracks := []
	for i in 5:
		cracks.append({
			"angle": TAU * (i + rng.randf_range(-0.35, 0.35)) / 5.0,
			"length": rng.randf_range(0.45, 0.7),
			"width": rng.randf_range(0.006, 0.011),
		})

	var height := PackedFloat32Array()
	height.resize(N * N)
	var albedo := Image.create(N, N, false, Image.FORMAT_RGBA8)
	var orm := Image.create(N, N, false, Image.FORMAT_RGB8)

	for y in N:
		for x in N:
			var u := (x + 0.5) / N * 2.0 - 1.0
			var v := (y + 0.5) / N * 2.0 - 1.0
			var r := sqrt(u * u + v * v)
			var th := atan2(v, u)
			var cx := cos(th)
			var cy := sin(th)
			var e1 := edge.get_noise_2d(cx * 2.0, cy * 2.0)
			var e2 := edge.get_noise_2d(cx * 3.0 + 10.0, cy * 3.0 + 10.0)
			var e3 := edge.get_noise_2d(cx * 1.5 - 20.0, cy * 1.5 - 20.0)
			var g := grain.get_noise_2d(x, y)
			var f := fine.get_noise_2d(x, y)
			var rh := 0.11 * (1.0 + 0.15 * e1)
			var rc := 0.30 * (1.0 + 0.25 * e2)

			# Radial cracks running out from the crater.
			var crack := 0.0
			for c in cracks:
				var wiggle: float = c.angle + 0.14 * edge.get_noise_2d(r * 9.0, c.angle * 10.0)
				var d := absf(wrapf(th - wiggle, -PI, PI)) * r
				var w: float = c.width * (1.0 - smoothstep(rc, c.length, r))
				if w > 0.0 and r > rc * 0.8:
					crack = maxf(crack, 1.0 - smoothstep(w * 0.3, w, d))

			# Small chips scattered around the crater.
			var chip := 0.0
			var cell := chips.get_noise_2d(x, y)
			if cell > 0.82 and r < 0.62 and r > rc:
				chip = (1.0 - smoothstep(0.45, 0.62, r))

			var h := 0.0
			var col := 0.0
			var a := 0.0
			var ao := 1.0
			var rough := 0.9
			if r < rh:
				h = -1.0 + 0.02 * f
				col = 0.012
				a = 1.0
				ao = 0.2
				rough = 1.0
			elif r < rc:
				var t := (r - rh) / (rc - rh)
				h = lerpf(-0.75, 0.0, pow(t, 0.6)) + 0.08 * g + 0.015 * f
				col = lerpf(0.05, 0.36, t) + 0.10 * g + 0.04 * f
				a = 1.0
				ao = lerpf(0.4, 1.0, t)
				rough = 0.85 + 0.1 * g
			else:
				# Soot, fading out with a ragged edge.
				var soot_r := rc + 0.22 + 0.1 * e3
				var soot := 1.0 - smoothstep(rc, soot_r, r)
				h = 0.015 * g * soot
				col = 0.07 + 0.04 * g
				a = soot * (0.75 + 0.25 * g)
				rough = 0.95
			# The raised, broken lip.
			h += 0.22 * exp(-pow((r - rc) / 0.045, 2.0)) * (0.7 + 0.3 * g)
			if crack > 0.0:
				h -= 0.15 * crack
				col = lerpf(col, 0.04, crack)
				a = maxf(a, crack * 0.9)
				ao = minf(ao, 1.0 - 0.5 * crack)
			if chip > 0.0:
				h -= 0.15 * chip
				col = lerpf(col, 0.38, chip)
				a = maxf(a, chip * 0.85)
			a *= 1.0 - smoothstep(0.82, 0.96, r)

			height[y * N + x] = h
			col = clampf(col, 0.0, 1.0)
			albedo.set_pixel(x, y, Color(col, col * 0.97, col * 0.93, clampf(a, 0.0, 1.0)))
			orm.set_pixel(x, y, Color(clampf(ao, 0.0, 1.0), clampf(rough, 0.0, 1.0), 0.0))

	var normal := Image.create(N, N, false, Image.FORMAT_RGB8)
	for y in N:
		for x in N:
			var dx := (height[y * N + mini(x + 1, N - 1)] - height[y * N + maxi(x - 1, 0)]) * 0.5
			var dy := (height[mini(y + 1, N - 1) * N + x] - height[maxi(y - 1, 0) * N + x]) * 0.5
			# OpenGL convention: green is +V up, image rows go down.
			var n := Vector3(-dx * NORMAL_STRENGTH, dy * NORMAL_STRENGTH, 1.0).normalized()
			normal.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))

	albedo.save_png(out_dir + "/bullet_hole_albedo.png")
	normal.save_png(out_dir + "/bullet_hole_normal.png")
	orm.save_png(out_dir + "/bullet_hole_orm.png")
	quit(0)
