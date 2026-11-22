// A CRT picture tube for the console's frame.
//
// Not an overlay: the frame is redrawn the way a tube would show it. Each of
// the picture's scanlines is a beam whose thickness grows with its brightness,
// so bright lines swell and dark ones leave a gap; the light then passes a
// shadow mask of red, green and blue phosphor stripes laid out in the
// window's own pixels; bright areas spill a soft glow into their surroundings
// (halation in the glass); and the glass is slightly curved, with rounded
// corners. Everything is worked out in linear light and converted back at the
// end, which is what keeps it from looking like dark stripes painted on top.
shader_type canvas_item;

uniform sampler2D frame : filter_linear, repeat_disable; // the console's picture
uniform vec2 frame_size = vec2(320.0, 224.0);            // its size in pixels
uniform vec2 game_size = vec2(320.0, 224.0);             // the picture in the game's own pixels (398 wide in the wide view)
uniform float scanlines = 224.0;      // beams down the picture (the game's own lines)
uniform float curvature = 0.045;      // 0 is a flat tube
uniform float beam_min = 0.42;        // a dark beam's thickness, as a share of the line pitch
uniform float beam_max = 0.80;        // a full-brightness beam's
uniform float mask_strength = 0.30;   // 0: no phosphor mask
uniform float halation = 0.16;        // glow spilled by bright areas
uniform float corner = 0.035;         // corner radius, as a share of the height

vec3 to_linear(vec3 c) { return pow(c, vec3(2.4)); }
vec3 to_screen(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)); }

// The picture at a point, sharp across (the game's pixels keep their edges)
// and smooth down (the beam does the vertical shaping).
vec3 picture(vec2 uv) {
	vec2 texel = uv * frame_size;
	vec2 base = floor(texel - 0.5) + 0.5;
	vec2 f = texel - base;
	// A narrow smooth step between neighbouring pixels: crisp, but no shimmer when the tube's curve resamples it.
	vec2 sharp = clamp((f - 0.5) / max(fwidth(texel), vec2(0.0001)) + 0.5, 0.0, 1.0);
	return to_linear(texture(frame, (base + sharp) / frame_size).rgb);
}

void fragment() {
	// The curved glass: push the picture out towards the corners.
	vec2 centred = UV * 2.0 - 1.0;
	vec2 bent = centred * (1.0 + curvature * dot(centred.yx * centred.yx, vec2(1.0)) * vec2(1.0, 1.15));
	bent /= 1.0 + curvature; // so the middle of each side still reaches the edge
	vec2 uv = bent * 0.5 + 0.5;

	// Two beams contribute to any point: the scanline above and the one below.
	float line = uv.y * scanlines - 0.5;
	float upper = floor(line);
	float within = line - upper;
	vec3 a = picture(vec2(uv.x, (upper + 0.5) / scanlines));
	vec3 b = picture(vec2(uv.x, (upper + 1.5) / scanlines));
	// Each colour's beam is as thick as it is bright.
	vec3 wide_a = mix(vec3(beam_min), vec3(beam_max), sqrt(a));
	vec3 wide_b = mix(vec3(beam_min), vec3(beam_max), sqrt(b));
	vec3 da = vec3(within) / wide_a, db = vec3(1.0 - within) / wide_b;
	vec3 lit = a * exp(-da * da * 2.0) + b * exp(-db * db * 2.0);
	// A thin beam is a brighter beam: the same energy in less height.
	lit *= 1.0 / mix(beam_max + 0.25, 1.0, 0.35);
	// When a scanline is thinner than about two window pixels the beam cannot be drawn: fade to the plain picture.
	float room = 1.0 / max(fwidth(line), 0.0001); // window pixels per scanline
	float beams = smoothstep(1.6, 3.0, room);
	vec3 plain = picture(uv);
	vec3 colour = mix(plain, lit, beams);

	// Halation: a wide, soft copy of the picture added faintly.
	vec3 glow = vec3(0.0);
	for (int i = 0; i < 12; i++) {
		float angle = float(i) * 0.5236;
		float radius = (i % 2 == 0) ? 3.0 : 7.0;
		glow += to_linear(texture(frame, uv + vec2(cos(angle), sin(angle)) * radius / game_size).rgb);
	}
	colour += glow / 12.0 * halation;

	// The shadow mask, in the window's own pixels: stripes of red, green and blue phosphor.
	float stripe = mod(floor(FRAGCOORD.x), 3.0);
	vec3 mask = vec3(1.0 - mask_strength);
	if (stripe < 0.5) mask.r = 1.0;
	else if (stripe < 1.5) mask.g = 1.0;
	else mask.b = 1.0;
	// Every sixth row of the mask is the gap between phosphor slots, offset on alternate triads.
	float slot = mod(floor(FRAGCOORD.y) + (mod(floor(FRAGCOORD.x / 3.0), 2.0) < 0.5 ? 0.0 : 3.0), 6.0);
	if (slot < 0.5) mask *= 1.0 - mask_strength * 0.5;
	// Make up the light the mask takes away, so the picture is not simply darker.
	colour *= mask / (1.0 - mask_strength * 0.72);

	// The edge of the tube: rounded corners, and a little less light towards them.
	vec2 edge = abs(bent);
	vec2 past = max(edge - (1.0 - corner * vec2(game_size.y / game_size.x, 1.0) * 2.0), 0.0) / (corner * vec2(game_size.y / game_size.x, 1.0) * 2.0);
	float inside = 1.0 - smoothstep(0.96, 1.0, length(past));
	inside *= step(edge.x, 1.0) * step(edge.y, 1.0);
	colour *= inside * (1.0 - 0.18 * dot(centred, centred) * 0.5);

	COLOR = vec4(to_screen(colour), 1.0);
}
