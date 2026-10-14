extends SceneTree

# Generates the gunshot, sounds/weapon_fire.wav (0.5s, mono, 16-bit,
# 44.1kHz): a noise crack, a sine thump, and a low rumble. Godot has no
# synthesizer, so every sample is computed here. Deterministic (fixed
# seed). Run from the project root, then re-import:
#
#   godot --headless --path . --script tools/gen_weapon_fire.gd [-- <out.wav>]
#   godot --headless --recovery-mode --path . --import

const RATE := 44100
const LENGTH := 0.5  # seconds
const PEAK := 0.9  # of full scale

# Crack: white noise through a low-pass whose cutoff falls from
# CRACK_CUTOFF_START to CRACK_CUTOFF_END, with a fast decay plus a softer
# tail.
const CRACK_CUTOFF_START := 9000.0  # Hz
const CRACK_CUTOFF_END := 1200.0  # Hz
const CRACK_CUTOFF_TAU := 0.025  # s
const CRACK_TAU := 0.03  # s
const CRACK_TAIL := 0.25  # tail level, relative
const CRACK_TAIL_TAU := 0.1  # s

# Thump: a sine sweeping from THUMP_FREQ_END + THUMP_FREQ_SWEEP down to
# THUMP_FREQ_END.
const THUMP_LEVEL := 0.85
const THUMP_FREQ_END := 50.0  # Hz
const THUMP_FREQ_SWEEP := 75.0  # Hz
const THUMP_FREQ_TAU := 0.06  # s
const THUMP_ATTACK := 0.012  # s
const THUMP_TAU := 0.09  # s

# Rumble: low-passed noise under the thump's decay.
const RUMBLE_LEVEL := 0.18
const RUMBLE_CUTOFF := 300.0  # Hz
const RUMBLE_TAU := 0.13  # s

# Fade-out over the last few milliseconds, so the sample ends at zero.
const FADE := 0.01  # s


# One-pole low-pass coefficient for cutoff fc.
static func lowpass_coeff(fc: float) -> float:
	return 1.0 - exp(-TAU * fc / RATE)


func _init():
	var args := OS.get_cmdline_user_args()
	var out_path: String = args[0] if args.size() > 0 else "res://sounds/weapon_fire.wav"

	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	var count := int(RATE * LENGTH)
	var samples := PackedFloat32Array()
	samples.resize(count)

	var crack_lp1 := 0.0
	var crack_lp2 := 0.0
	var rumble_lp1 := 0.0
	var rumble_lp2 := 0.0
	var phase := 0.0
	var b := lowpass_coeff(RUMBLE_CUTOFF)
	for i in count:
		var t := float(i) / RATE

		# Two one-pole stages: a steeper slope than one.
		var cutoff := CRACK_CUTOFF_END + (CRACK_CUTOFF_START - CRACK_CUTOFF_END) * exp(-t / CRACK_CUTOFF_TAU)
		var a := lowpass_coeff(cutoff)
		crack_lp1 += a * (rng.randf_range(-1.0, 1.0) - crack_lp1)
		crack_lp2 += a * (crack_lp1 - crack_lp2)
		var crack_env := exp(-t / CRACK_TAU) + CRACK_TAIL * exp(-t / CRACK_TAIL_TAU)
		# Low-passing loses level as the cutoff falls; make up for some of it.
		var crack := crack_lp2 * crack_env * sqrt(CRACK_CUTOFF_START / cutoff)

		var freq := THUMP_FREQ_END + THUMP_FREQ_SWEEP * exp(-t / THUMP_FREQ_TAU)
		phase += TAU * freq / RATE
		var thump_env := (1.0 - exp(-t / THUMP_ATTACK)) * exp(-t / THUMP_TAU)
		var thump := THUMP_LEVEL * sin(phase) * thump_env

		rumble_lp1 += b * (rng.randf_range(-1.0, 1.0) - rumble_lp1)
		rumble_lp2 += b * (rumble_lp1 - rumble_lp2)
		var rumble := RUMBLE_LEVEL * rumble_lp2 * exp(-t / RUMBLE_TAU) / sqrt(b)

		# Soft clipping rounds off the loud first milliseconds.
		var s := tanh(2.5 * (crack + thump + rumble))
		samples[i] = s * clampf((LENGTH - t) / FADE, 0.0, 1.0)

	var peak := 0.0
	for s in samples:
		peak = maxf(peak, absf(s))
	var data := PackedByteArray()
	data.resize(count * 2)
	for i in count:
		data.encode_s16(i * 2, int(round(samples[i] / peak * PEAK * 32767.0)))

	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = RATE
	wav.stereo = false
	wav.data = data
	var err := wav.save_to_wav(out_path)
	if err != OK:
		push_error("Failed to write %s: %s" % [out_path, error_string(err)])
		quit(1)
		return
	quit(0)
