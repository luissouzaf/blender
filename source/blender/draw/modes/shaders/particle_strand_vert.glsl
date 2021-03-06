
uniform mat4 ModelViewProjectionMatrix;

in vec3 pos;
in float color;

out vec4 finalColor;
#ifdef USE_POINTS
out vec2 radii;
#endif

vec3 weight_to_rgb(float weight)
{
	vec3 r_rgb;
	float blend = ((weight / 2.0) + 0.5);

	if (weight <= 0.25) {    /* blue->cyan */
		r_rgb[0] = 0.0;
		r_rgb[1] = blend * weight * 4.0;
		r_rgb[2] = blend;
	}
	else if (weight <= 0.50) {  /* cyan->green */
		r_rgb[0] = 0.0;
		r_rgb[1] = blend;
		r_rgb[2] = blend * (1.0 - ((weight - 0.25) * 4.0));
	}
	else if (weight <= 0.75) {  /* green->yellow */
		r_rgb[0] = blend * ((weight - 0.50) * 4.0);
		r_rgb[1] = blend;
		r_rgb[2] = 0.0;
	}
	else if (weight <= 1.0) {  /* yellow->red */
		r_rgb[0] = blend;
		r_rgb[1] = blend * (1.0 - ((weight - 0.75) * 4.0));
		r_rgb[2] = 0.0;
	}
	else {
		/* exceptional value, unclamped or nan,
		 * avoid uninitialized memory use */
		r_rgb[0] = 1.0;
		r_rgb[1] = 0.0;
		r_rgb[2] = 1.0;
	}

	return r_rgb;
}

#define DECOMPRESS_RANGE 1.0039

void main()
{
	gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);

#ifdef USE_WEIGHT
	finalColor = vec4(weight_to_rgb(color * DECOMPRESS_RANGE), 1.0);
#else
	finalColor = mix(colorWire, colorEdgeSelect, color);
#endif

#ifdef USE_POINTS
	gl_PointSize = sizeVertex;

	/* calculate concentric radii in pixels */
	float radius = 0.5 * sizeVertex;

	/* start at the outside and progress toward the center */
	radii[0] = radius;
	radii[1] = radius - 1.0;

	/* convert to PointCoord units */
	radii /= sizeVertex;
#endif
}
