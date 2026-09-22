/**
 * Readout — browser demo.
 *
 * `VERTEX`, `CAPTURE` and `READOUT` below are `kVertexShader`, `kCaptureShader`
 * and `kReadoutShader` from `source/Shaders.cpp`, copied across unedited.
 * `demo/tools/check_shaders.py` compares them character for character with the
 * C++ and is called from `tools/verify.sh`, because two copies of a shader is
 * exactly the arrangement that drifts — invisibly, from both sides.
 *
 * Everything below the shaders is a **port**: `controls` is a hand translation
 * of `source/Controls.cpp`, `Ring` is `source/Ring.{h,cpp}` rewritten against
 * WebGL2, and `createRenderer` is the part of `Readout::ProcessOpenGL` that
 * turns controls into uniforms. Nothing checks the port but a reader. When a
 * mapping changes in `Controls.cpp` it has to change here too, and a wrong one
 * shows up on the page as a band in slightly the wrong place, which nobody will
 * notice.
 *
 * ------------------------------------------------------------ what is here
 *
 * The whole model, which is one idea: a CMOS sensor reads its rows one after
 * another, so every pixel has a sample window and the picture is sampled there.
 * That means the ring — the last N frames in one `GL_TEXTURE_2D_ARRAY`, at the
 * plugin's own depth of up to 16 — is here in full, because without it there is
 * no sub-frame window and therefore no skew, no jello and no motion blur.
 * WebGL2 has array textures and `framebufferTextureLayer`, so `Ring` is the
 * same object with different spellings.
 *
 * Skew, jello, flash banding, mains flicker, the four readout directions, Blend
 * vs Hold, Global and Mix all work, on the plugin's own arithmetic.
 *
 * --------------------------------------------------------- what is missing
 *
 * **The audio side, entirely.** The plugin takes Resolume's 64-bin spectrum as
 * a parameter and uses it three ways — Audio Drive scales the shake, an onset
 * kicks a damped impulse into the camera, and Trigger: Onset fires the flash.
 * A browser has no Resolume FFT parameter, and asking a visitor for their
 * microphone to demonstrate a video effect is not a trade worth making. So
 * **Audio Drive and Audio Band are absent from the panel** rather than present
 * and dead. The onset impulse is still in the shader, because the shader is the
 * plugin's own text, but nothing on this page can arm it: `OnsetTau` is handed
 * over as -1 on every frame. `rotest --audio` in the repository is where the
 * audio path is exercised.
 *
 * **Beat and Bar.** Those two Trigger elements read Resolume's transport, which
 * a browser does not have. They are still in the dropdown because the element
 * list is part of what the plugin declares and truncating it would renumber
 * Interval from 4 to 2 — so a shared link would mean something different here
 * than in the host. They never fire: the ported code is the plugin's, and with
 * no transport its bar phase never crosses anything. The hint on the control
 * says so, and so does the disclosure at the foot.
 *
 * **Fire is a toggle, not an event.** FFGL has `FF_TYPE_EVENT` and the kit's
 * parameter model does not, so Fire is declared here as a boolean that the
 * renderer snaps back to Off the moment it has queued the pulse — which is what
 * a host does with an event parameter anyway, and it is why the button blinks.
 *
 * **The clock.** The plugin measures the host's frame period from the host's own
 * deltas, and that port is exact; but this page's clock is a browser's
 * `requestAnimationFrame`, so the ring holds frames at whatever rate your
 * display runs, not at a composition's frame rate. It also captures a frame only
 * when its clock actually advanced, where the plugin captures on every
 * `ProcessOpenGL` — otherwise Pause would flush the ring with sixteen copies of
 * one frame and the skew you paused to look at would disappear.
 *
 * What this page is NOT: it is the plugin's shaders, not the plugin. No
 * Resolume, no FFGL, no C++ — and GLSL ES 3.00 rather than desktop GL 4.1 core,
 * which the kit's `port()` handles. Nothing here is measured; `tools/verify.sh`
 * in the repository is the reason to believe the maths.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens once in the capture pass instead, and
	//the readout pass works on a layer we allocated, where the picture really
	//does fill the texture.
	uv = vUV;
}
`;

const CAPTURE = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;     //the part of the input texture that is really picture
uniform vec2 HalfTexel; //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and the shake sends
	//a fetch to the very edge of the frame on purpose -- a dark fringe there
	//would read as the camera's own vignette.
	//
	//At every interior pixel centre this is an exact copy: the clamp does not
	//move the point and bilinear at a texel centre returns the texel. The
	//harness's --global check relies on that being bit-exact.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	fragColor = texture( InputTexture, picture * MaxUV );
}
`;

const READOUT = `#version 410 core

uniform sampler2DArray Ring;
uniform int Slots;          //layers in the ring
uniform int Head;           //the layer holding the newest frame
uniform int Filled;         //how many layers hold a frame at all
uniform vec2 PictureSize;   //the ring's size, in pixels

uniform float FrameSeconds;   //the host's frame period
uniform float ReadoutFrames;  //readout time, in host frames
uniform float ExposureFrames; //exposure, in host frames
uniform int Direction;        //0 top-down, 1 bottom-up, 2 left-right, 3 right-left
uniform int Hold;             //0 blend between frames, 1 hold the older one

//The camera. Band-limited noise as a handful of sinusoids; each has its
//phase at the frame's timestamp, and amplitudes for x, y (in picture
//heights) and rotation (radians). Plus one damped impulse from the last
//audio onset.
uniform int ShakeActive;
uniform float ShakeHz[ 4 ];
uniform vec3 ShakePhase[ 4 ];
uniform vec3 ShakeAmp[ 4 ];
uniform float OnsetTau;     //seconds before now the onset landed; negative for none
uniform float OnsetAmp;     //picture heights
uniform float OnsetHz;
uniform float OnsetDecay;   //seconds

//Flash pulses still in flight. x: seconds before now the pulse BEGAN,
//y: its length in seconds, z: level.
uniform int FlashCount;
uniform vec4 FlashPulse[ 4 ];
uniform vec3 FlashRGB[ 4 ];

//Mains light: 1 - Depth * cos( Phase - Omega * tau ), with Omega at twice
//the mains frequency because a lamp's power goes as the square of the
//voltage.
uniform float MainsOmega;
uniform float MainsPhase;
uniform float MainsDepth;

uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

const float kTau = 6.283185307179586;

//Weight of frame k over the window [a0, a1], in frames of age.
//
//Blend treats the frames as samples of a continuous scene and interpolates
//linearly between neighbours: frame k's basis is a hat of width 2 about k,
//and the weight is the hat's integral over the window. Hold keeps the older
//frame until the newer one exists: frame k's basis is a box over (k-1, k].
//Both bases sum to one everywhere, so the weights need no normalising
//except where the window runs past the frames the ring holds.
float hatIntegral( float x )
{
	if( x <= -1.0 )
		return 0.0;
	if( x < 0.0 )
	{
		float u = x + 1.0;
		return 0.5 * u * u;
	}
	if( x < 1.0 )
	{
		float u = 1.0 - x;
		return 1.0 - 0.5 * u * u;
	}
	return 1.0;
}

float boxIntegral( float x )
{
	return clamp( x + 1.0, 0.0, 1.0 );
}

float slotWeight( float k, float a0, float a1 )
{
	float span = a1 - a0;
	if( span < 1e-6 )
	{
		//An instantaneous sample: the basis itself.
		float x = a0 - k;
		if( Hold == 1 )
			return ( x > -1.0 && x <= 0.0 ) ? 1.0 : 0.0;
		return max( 0.0, 1.0 - abs( x ) );
	}
	if( Hold == 1 )
		return ( boxIntegral( a1 - k ) - boxIntegral( a0 - k ) ) / span;
	return ( hatIntegral( a1 - k ) - hatIntegral( a0 - k ) ) / span;
}

void main()
{
	//-----------------------------------------------------------------
	// Which row, and when it was read.
	//
	// Rows are discrete: every pixel on a sensor row shares its sample
	// time, so the row index is floored rather than taken from uv. Row 0 is
	// the first row read, whichever edge that is.
	//-----------------------------------------------------------------
	float rows, row;
	if( Direction == 0 )
	{
		rows = PictureSize.y;
		row  = floor( ( 1.0 - uv.y ) * rows );
	}
	else if( Direction == 1 )
	{
		rows = PictureSize.y;
		row  = floor( uv.y * rows );
	}
	else if( Direction == 2 )
	{
		rows = PictureSize.x;
		row  = floor( uv.x * rows );
	}
	else
	{
		rows = PictureSize.x;
		row  = floor( ( 1.0 - uv.x ) * rows );
	}
	row = clamp( row, 0.0, rows - 1.0 );

	//Frames before the timestamp this row was read, and the exposure window
	//behind it. The last row is read ReadoutFrames / rows before the
	//timestamp, the first row ReadoutFrames before it.
	float tau = ReadoutFrames * ( 1.0 - row / rows );
	float a0  = tau;
	float a1  = tau + ExposureFrames;

	float tauR = tau * FrameSeconds;            //seconds
	float E    = ExposureFrames * FrameSeconds; //seconds

	//-----------------------------------------------------------------
	// The camera, at the middle of the window.
	//
	// Once per row, not integrated: a real sensor would also blur the shake
	// across the exposure, and this is the honest limit of the model. What
	// it does get exactly right is the part that makes jello: each row sees
	// the camera at a different moment.
	//-----------------------------------------------------------------
	vec2 sampleUV = uv;
	if( ShakeActive == 1 )
	{
		float tauC = tauR + 0.5 * E;

		vec2 d    = vec2( 0.0 );
		float rot = 0.0;
		for( int i = 0; i < 4; ++i )
		{
			float arg = kTau * ShakeHz[ i ] * tauC;
			d.x += ShakeAmp[ i ].x * sin( ShakePhase[ i ].x - arg );
			d.y += ShakeAmp[ i ].y * sin( ShakePhase[ i ].y - arg );
			rot += ShakeAmp[ i ].z * sin( ShakePhase[ i ].z - arg );
		}

		if( OnsetTau >= 0.0 && tauC < OnsetTau )
		{
			float since = OnsetTau - tauC;
			float w     = OnsetAmp * exp( -since / OnsetDecay ) * sin( kTau * OnsetHz * since );
			//Mostly vertical: a thump through the floor pushes the camera down.
			d += vec2( 0.3 * w, w );
		}

		//Centred, in picture heights on both axes, so a rotation is a
		//rotation on a 16:9 frame rather than a shear.
		float aspect = PictureSize.x / PictureSize.y;
		vec2 p       = vec2( ( uv.x - 0.5 ) * aspect, uv.y - 0.5 );
		float c      = cos( rot );
		float s      = sin( rot );
		vec2 r       = vec2( p.x * c - p.y * s, p.x * s + p.y * c ) + d;
		sampleUV     = vec2( r.x / aspect + 0.5, r.y + 0.5 );
	}

	//Outside the picture there is nothing to show. A camera that shakes
	//shows its own frame edge moving, not the edge row smeared across the
	//gap -- and this is where CLAMP_TO_EDGE would be the wrong answer rather
	//than a safe one.
	vec4 colour = vec4( 0.0 );
	if( sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0 )
	{
		//-------------------------------------------------------------
		// The scene, integrated across the ring over the window.
		//-------------------------------------------------------------
		vec4 sum    = vec4( 0.0 );
		float total = 0.0;
		for( int k = 0; k < Slots; ++k )
		{
			float w = slotWeight( float( k ), a0, a1 );
			if( w <= 0.0 )
				continue;

			//Ages the ring has not yet filled read the oldest frame there is,
			//so the first frames after load show a picture rather than black.
			int age   = min( k, Filled - 1 );
			int layer = ( Head - age + Slots ) % Slots;
			sum += w * texture( Ring, vec3( sampleUV, float( layer ) ) );
			total += w;
		}
		colour = total > 0.0 ? sum / total : vec4( 0.0 );
	}

	//-----------------------------------------------------------------
	// The flash. A pulse lights a row by the fraction of it the row's
	// window caught, normalised so a window that catches the whole pulse
	// -- or a pulse that covers the whole window -- reads as the full
	// level. The edges of the band are ramps min( length, exposure ) long.
	//-----------------------------------------------------------------
	vec3 flash = vec3( 0.0 );
	for( int j = 0; j < FlashCount; ++j )
	{
		float begin  = FlashPulse[ j ].x;
		float length = FlashPulse[ j ].y;
		float level  = FlashPulse[ j ].z;

		float frac;
		if( E < 1e-7 )
		{
			frac = ( tauR >= begin - length && tauR <= begin ) ? 1.0 : 0.0;
		}
		else
		{
			float lo = max( begin - length, tauR );
			float hi = min( begin, tauR + E );
			frac     = clamp( max( 0.0, hi - lo ) / min( length, E ), 0.0, 1.0 );
		}
		flash += FlashRGB[ j ] * ( level * frac );
	}

	//-----------------------------------------------------------------
	// The mains light, averaged over the window. Closed form: the integral
	// of a cosine is a sine, and dividing by the window length gives the
	// mean, which is what a photosite measures.
	//-----------------------------------------------------------------
	float light = 1.0;
	if( MainsDepth > 0.0 )
	{
		if( E < 1e-7 )
			light = 1.0 - MainsDepth * cos( MainsPhase - MainsOmega * tauR );
		else
			light = 1.0 - MainsDepth
			        * ( sin( MainsPhase - MainsOmega * tauR ) - sin( MainsPhase - MainsOmega * ( tauR + E ) ) )
			        / ( MainsOmega * E );
	}

	//Premultiplied throughout: the light scales the colour and the flash is
	//scaled by coverage, so a transparent pixel is lit by neither.
	vec4 result = vec4( colour.rgb * light + flash * colour.a, colour.a );

	vec4 source = texture( Ring, vec3( uv, float( Head ) ) );
	fragColor   = mix( source, result, MixAmount );
}
`;

// The readout pass reads a `sampler2DArray`, which GLSL ES 3.00 gives no
// default precision at all — unlike `sampler2D`, which has one in the fragment
// stage. Without a declaration the shader fails to compile with
// `'sampler2DArray' : No precision specified` on some drivers and is accepted
// by others, which is the worst kind of difference to leave to chance.
//
// This page used to splice `precision highp sampler2DArray;` in after the
// version directive itself. The kit's `port()` now declares it, along with the
// 3D and integer array samplers, so READOUT goes to the compiler as it is
// written — and the fix is in the one place the other demos read from rather
// than in this file alone.

//---------------------------------------------------------------------------
// controls — a port of source/Controls.cpp.
//
// Host parameters are 0..1; these are what they mean. Every ranged parameter
// the plugin declares is a plain FF_TYPE_STANDARD float in 0..1 even where it
// stands for milliseconds, because SetParamInfo clamps a standard default into
// 0..1 before SetParamRange can widen it. The conversions live in one file over
// there for the same reason they live in one object here: so there is only ever
// one answer to what a slider position means.
//
// Where a mapping is geometric it is because the interesting range is at one
// end: the difference between a 2 ms and a 4 ms flash is a different band, the
// difference between 18 ms and 20 ms is not.
//---------------------------------------------------------------------------

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);
const unlerp = (from, to, value) => clamp01((value - from) / (to - from));

/** Equal slider movements are equal *ratios*. */
const geometric = (from, to, t) => from * (to / from) ** clamp01(t);
const ungeometric = (from, to, value) => clamp01(Math.log(value / from) / Math.log(to / from));

const kPi = 3.14159265358979323846;

/**
 * Linear RGB. Warm and Cool are a tungsten and a daylight LED against a neutral
 * flash; the saturated ones are gels, because a coloured strobe on a rolling
 * shutter is the whole reason anybody asks for a flash colour.
 */
const FLASH_COLOURS = [
  [1.0, 1.0, 1.0], // White
  [1.0, 0.8, 0.55], // Warm
  [0.75, 0.88, 1.0], // Cool
  [1.0, 0.55, 0.1], // Amber
  [1.0, 0.08, 0.08], // Red
  [0.08, 1.0, 0.15], // Green
  [0.1, 0.25, 1.0], // Blue
  [1.0, 0.1, 0.9], // Magenta
];

const FLASH_COLOUR_NAMES = ['White', 'Warm', 'Cool', 'Amber', 'Red', 'Green', 'Blue', 'Magenta'];

const controls = {
  /** 1 to 60 ms, geometrically. One host frame at 60 fps is 16.7 ms. */
  readoutSeconds: (v) => geometric(0.001, 0.06, v),
  readoutParam: (s) => ungeometric(0.001, 0.06, s),

  /** 0 to 40 ms, linear. Zero is an instantaneous sample and a legitimate one. */
  exposureSeconds: (v) => lerp(0.0, 0.04, v),
  exposureParam: (s) => unlerp(0.0, 0.04, s),

  /**
   * 0 to 0.06 of the picture height, geometric from a floor and floored to
   * exactly zero at the bottom: a geometric mapping cannot reach zero, and a
   * camera that never quite stops shaking is a control to be fought.
   */
  shakeAmplitude: (v) => (v <= 0 ? 0 : geometric(0.0005, 0.06, v)),

  /** 2 to 80 Hz, geometrically. Where the jello lives. */
  shakeFrequencyHz: (v) => geometric(2.0, 80.0, v),
  shakeFrequencyParam: (hz) => ungeometric(2.0, 80.0, hz),

  /** 0 to 2 degrees, in radians, floored to zero. */
  shakeRotationRadians: (v) => (v <= 0 ? 0 : (geometric(0.02, 2.0, v) * kPi) / 180.0),

  /** 0.1 to 4 s, geometrically. */
  flashIntervalSeconds: (v) => geometric(0.1, 4.0, v),

  /** 0.1 to 20 ms, geometrically. A xenon strobe is a few hundred microseconds. */
  flashLengthSeconds: (v) => geometric(0.0001, 0.02, v),
  flashLengthParam: (s) => ungeometric(0.0001, 0.02, s),

  /** 0 to 1 of the readout, linear. Where in the frame the pulse lands. */
  flashPhase: (v) => clamp01(v),

  /** 0 to 2, linear. */
  flashLevel: (v) => lerp(0.0, 2.0, v),

  /** 0 to 1, linear. Modulation depth about a mean of one. */
  flickerDepth: (v) => clamp01(v),

  /** 0 for Off, else 50 or 60. */
  mainsHz: (option) => (option === 1 ? 50.0 : option === 2 ? 60.0 : 0.0),

  flashColour: (option) => FLASH_COLOURS[Math.min(FLASH_COLOURS.length - 1, Math.max(0, Math.round(option)))],

  // AudioDrive() is deliberately not ported: there is no audio here. See the
  // header.
};

//---------------------------------------------------------------------------
// Readout.cpp's file-scope constants, and the ones the shake needs.
//---------------------------------------------------------------------------

const kTau = 6.283185307179586;

/** Seconds of clock a single frame is allowed to advance by. */
const kMaxFrameDelta = 0.25;

/** The most frames the ring will hold — Ring.h's kMaxSlots. */
const kMaxSlots = 16;

const kShakeComponents = 4;
const kMaxFlashes = 4;

/**
 * The band-limited noise: four sinusoids around the base frequency, with
 * incommensurable ratios so the sum never repeats within a take, and weights
 * falling off away from the centre. Normalised so the peak displacement is the
 * amplitude the control names.
 */
const kShakeRatio = [1.0, 1.47, 0.63, 2.31];
const kShakeWeight = [1.0, 0.45, 0.35, 0.18];
const kShakeWeightSum = 1.98;

/**
 * Fixed phase offsets per component and axis, so x, y and rotation are not in
 * step. Golden-angle spaced.
 */
const kPhaseOffset = [
  [0.0, 2.399963, 4.799926],
  [0.916298, 3.316261, 5.716224],
  [1.832596, 4.232559, 0.349337],
  [2.748894, 5.148857, 1.265635],
];

/**
 * An angle reduced into [0, 2pi) before it is handed to a float uniform.
 *
 * In the plugin this is done in `double` because Resolume's clock has been seen
 * at 499,217 seconds, where a float resolves to 0.03 s — three whole readouts.
 * JavaScript numbers are doubles throughout, so the reduction is the same
 * arithmetic; the page's own clock starts at zero and never gets near that, but
 * the port is kept because the *reduction* is what the shader's contract
 * assumes.
 */
function reducedAngle(omega, t) {
  const a = (omega * t) % kTau;
  return a < 0 ? a + kTau : a;
}

//---------------------------------------------------------------------------
// Ring — a port of source/Ring.{h,cpp}.
//
// The last N input frames, as the layers of one array texture. The readout pass
// needs every recent frame at once: a single row's exposure window can straddle
// two or three of them. Sixteen separate textures would be sixteen sampler
// bindings, and GLSL indexes a sampler array only with a dynamically uniform
// expression, which a per-pixel frame age is not. One array texture is a single
// sampler that any integer can index.
//
// The kit's PassBuffer is 2D only, which is why this is here rather than there:
// nothing else in the fleet needs an array texture.
//---------------------------------------------------------------------------

class Ring {
  constructor(gl) {
    this.gl = gl;
    this.texture = null;
    this.fbo = null;
    this.width = 0;
    this.height = 0;
    this.slots = 0;
    this.reshaped = false;
    this.savedFBO = null;
  }

  /**
   * Allocate at this size and depth, reusing the existing texture if it already
   * matches. A reshape empties the ring: the layer indexing is modulo the depth,
   * so a different depth does not shuffle the contents, it files every one of
   * them under the wrong age.
   */
  ensure(width, height, slots) {
    const gl = this.gl;
    this.reshaped = false;

    width = Math.max(1, Math.floor(width));
    height = Math.max(1, Math.floor(height));
    slots = Math.min(kMaxSlots, Math.max(2, slots));

    if (this.texture && this.width === width && this.height === height && this.slots === slots) {
      return true;
    }

    this.destroy();

    this.texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.texture);
    gl.texImage3D(gl.TEXTURE_2D_ARRAY, 0, gl.RGBA8, width, height, slots, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);

    // Bilinear, because every fetch the readout pass makes is between texels:
    // the shake moves the sample point by a fraction of a pixel, and a sensor
    // row lands between two frames of the ring far more often than on one.
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);

    this.fbo = gl.createFramebuffer();

    this.width = width;
    this.height = height;
    this.slots = slots;

    // Clear every layer. A buffer whose contents are undefined is not "a bit of
    // noise on the first frame" — a slot is shown for as many frames as the
    // window is long.
    const previous = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    let complete = true;
    for (let layer = 0; layer < slots; layer += 1) {
      gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, this.texture, 0, layer);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
        complete = false;
        break;
      }
      gl.clear(gl.COLOR_BUFFER_BIT);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, previous);

    if (!complete) {
      this.destroy();
      return false;
    }

    this.reshaped = true;
    return true;
  }

  /** Bind the FBO to draw into the slot the newest frame belongs in. */
  beginCapture(layer) {
    const gl = this.gl;
    this.savedFBO = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.framebufferTextureLayer(
      gl.FRAMEBUFFER,
      gl.COLOR_ATTACHMENT0,
      this.texture,
      0,
      Math.min(Math.max(layer, 0), Math.max(0, this.slots - 1)),
    );
    gl.viewport(0, 0, this.width, this.height);
  }

  /**
   * The framebuffer is put back; the viewport deliberately is not, because
   * nothing here knows what the caller's was.
   */
  endCapture() {
    this.gl.bindFramebuffer(this.gl.FRAMEBUFFER, this.savedFBO);
  }

  destroy() {
    const gl = this.gl;
    if (this.fbo) gl.deleteFramebuffer(this.fbo);
    if (this.texture) gl.deleteTexture(this.texture);
    this.fbo = null;
    this.texture = null;
    this.width = 0;
    this.height = 0;
    this.slots = 0;
  }
}

//---------------------------------------------------------------------------
// The renderer — a port of Readout::ProcessOpenGL.
//---------------------------------------------------------------------------

function createRenderer(gl, quad) {
  const captureShader = new Program(gl, VERTEX, CAPTURE, 'capture');
  const readoutShader = new Program(gl, VERTEX, READOUT, 'readout');
  const ring = new Ring(gl);

  // --- the ring ----------------------------------------------------------
  let head = 0; // the layer holding the newest frame
  let filled = 0; // how many layers hold a frame at all

  // --- the clock ---------------------------------------------------------
  //
  // `render()` is handed a clock and no frame period, so the period is measured
  // from the clock's own deltas exactly as the plugin measures it from the
  // host's. It is what turns milliseconds of readout into frames of ring, so it
  // has to be real: assume 60 and every skew is wrong at 50.
  let lastNow = -1;
  let frameSeconds = 1 / 60;

  // --- the flash ---------------------------------------------------------
  /** @type {{centre: number, length: number, level: number, rgb: number[]}[]} */
  let flashes = [];
  let nextInterval = -1;

  // Scratch, reused every frame so a 60 Hz page does not allocate six arrays a
  // frame for the garbage collector to find later.
  const shakeHz = new Float32Array(kShakeComponents);
  const shakePhase = new Float32Array(kShakeComponents * 3);
  const shakeAmp = new Float32Array(kShakeComponents * 3);
  const flashPulse = new Float32Array(kMaxFlashes * 4);
  const flashRGB = new Float32Array(kMaxFlashes * 3);

  /**
   * `Readout::fire` — queue a pulse centred on the phase point of THIS frame's
   * readout. The first row was read `readout` ago and the last row now, so phase
   * p is `readout * ( 1 - p )` seconds before now.
   */
  function fire(params, now, readoutSeconds) {
    flashes.push({
      centre: now - readoutSeconds * (1.0 - controls.flashPhase(params.get('phase'))),
      length: controls.flashLengthSeconds(params.get('length')),
      level: controls.flashLevel(params.get('level')),
      rgb: controls.flashColour(params.option('colour')),
    });
    while (flashes.length > kMaxFlashes) flashes.shift();
  }

  return {
    render({ input, params, width, height, time }) {
      //---------------------------------------------------------------
      // The clock, and the frame period measured from it.
      //---------------------------------------------------------------
      const now = time;

      // A jump backwards is Restart, or a scrub in a host. The ring's contents
      // are then frames from the future, so it is emptied rather than read.
      if (lastNow >= 0 && now < lastNow) {
        head = 0;
        filled = 0;
        flashes = [];
        nextInterval = -1;
      }

      let dt = 0;
      if (lastNow >= 0 && now > lastNow) {
        dt = Math.min(now - lastNow, kMaxFrameDelta);
        // Smoothed, because a single long frame must not turn a 20 ms readout
        // into half a frame for one frame. A steady clock converges exactly.
        const delta = Math.min(Math.max(dt, 1 / 240), 1 / 10);
        frameSeconds += (delta - frameSeconds) * 0.15;
      }
      lastNow = now;

      //---------------------------------------------------------------
      // What the controls say.
      //---------------------------------------------------------------
      const global = params.get('global') > 0.5;
      const readoutSeconds = global ? 0.0 : controls.readoutSeconds(params.get('readout'));
      const exposure = controls.exposureSeconds(params.get('exposure'));
      const direction = params.option('direction');
      const hold = params.option('interpolation') === 1 ? 1 : 0;
      const trigger = params.option('trigger');

      //---------------------------------------------------------------
      // The flash schedule.
      //
      // Interval is a counter from the first frame it was switched on, not
      // arithmetic on the clock: `floor( now / interval )` lands a hair under
      // the integer on exactly the frames that should fire.
      //
      // Beat, Bar and Onset are the three the plugin drives from things this
      // page does not have — Resolume's transport and Resolume's spectrum — so
      // they simply never come round. See the header.
      //---------------------------------------------------------------
      let firePending = false;

      // Fire is FF_TYPE_EVENT in the plugin and a boolean here, so the renderer
      // does what a host does with an event: take the press and release it.
      if (params.get('fire') > 0.5) {
        firePending = true;
        params.set('fire', 0);
      }

      if (trigger === 4) {
        if (nextInterval < 0 || now + 1e-6 >= nextInterval) {
          firePending = true;
          nextInterval = now + controls.flashIntervalSeconds(params.get('interval'));
        }
      } else {
        nextInterval = -1;
      }

      if (firePending) fire(params, now, readoutSeconds);

      // Drop every pulse that every row's window has passed.
      flashes = flashes.filter((f) => {
        const begin = now - f.centre + 0.5 * f.length;
        return begin - f.length <= readoutSeconds + exposure + 0.002;
      });

      //---------------------------------------------------------------
      // The ring. Deep enough for the oldest row's window, plus a frame each
      // side for the interpolation, rounded up to a multiple of four so that
      // dragging Readout Time reallocates — and empties — the ring at four
      // boundaries rather than at every millisecond.
      //---------------------------------------------------------------
      const windowFrames = (readoutSeconds + exposure) / frameSeconds;
      let slots = Math.ceil(windowFrames) + 2;
      slots = Math.min(kMaxSlots, Math.max(4, Math.floor((slots + 3) / 4) * 4));

      if (!ring.ensure(width, height, slots)) {
        throw new Error(
          `The frame ring could not be allocated: ${slots} × ${width}×${height}. `
            + 'Try a shorter Readout Time or Exposure, or a smaller composition.',
        );
      }
      if (ring.reshaped) {
        head = 0;
        filled = 0;
      }
      slots = ring.slots;

      //---------------------------------------------------------------
      // 1. Capture. The head advances and the input goes into it.
      //
      // **The one place this deviates from the plugin.** Readout.cpp captures
      // on every ProcessOpenGL, because a host calls it once per frame and its
      // clock moves. Here, Pause and a parameter drag both redraw without the
      // clock moving, and capturing then would fill the ring with sixteen
      // copies of one frame — so the skew you paused to look at would fade out
      // over the next few redraws. When the clock is running this is the same
      // thing; it is listed in the disclosure either way.
      //---------------------------------------------------------------
      if (dt > 0 || filled === 0) {
        if (filled > 0) head = (head + 1) % slots;
        filled = Math.min(filled + 1, slots);

        ring.beginCapture(head);
        captureShader.use();
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, input.texture);
        captureShader.setSampler('InputTexture', 0);
        // The kit hands us a texture that is all picture, where Resolume hands
        // the plugin a possibly larger one with the picture in a corner.
        captureShader.set('MaxUV', 1, 1);
        captureShader.set('HalfTexel', 0.5 / width, 0.5 / height);
        gl.disable(gl.BLEND);
        quad.draw();
        ring.endCapture();
      }

      //---------------------------------------------------------------
      // 2. Readout, straight to the canvas.
      //---------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      readoutShader.use();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, ring.texture);

      readoutShader.setSampler('Ring', 0);
      readoutShader.setInt('Slots', slots);
      readoutShader.setInt('Head', head);
      readoutShader.setInt('Filled', filled);
      readoutShader.set('PictureSize', width, height);

      readoutShader.set('FrameSeconds', frameSeconds);
      readoutShader.set('ReadoutFrames', readoutSeconds / frameSeconds);
      readoutShader.set('ExposureFrames', exposure / frameSeconds);
      readoutShader.setInt('Direction', direction);
      readoutShader.setInt('Hold', hold);

      //--- the camera --------------------------------------------------
      shakeHz.fill(0);
      shakePhase.fill(0);
      shakeAmp.fill(0);

      const base = controls.shakeFrequencyHz(params.get('frequency'));
      // `audioGain` is 1 + drive * level in the plugin, and there is no level
      // here, so it is 1 — written out rather than folded away so the two
      // expressions still read as the same one.
      const audioGain = 1.0;
      const translate = controls.shakeAmplitude(params.get('amount')) * audioGain;
      const rotate = controls.shakeRotationRadians(params.get('rotation')) * audioGain;

      for (let i = 0; i < kShakeComponents; i += 1) {
        const f = base * kShakeRatio[i];
        const w = kShakeWeight[i] / kShakeWeightSum;
        shakeHz[i] = f;
        for (let axis = 0; axis < 3; axis += 1) {
          shakePhase[i * 3 + axis] = reducedAngle(kTau * f, now + kPhaseOffset[i][axis] / (kTau * f));
        }
        shakeAmp[i * 3 + 0] = translate * w * 0.8;
        shakeAmp[i * 3 + 1] = translate * w;
        shakeAmp[i * 3 + 2] = rotate * w;
      }
      const active = translate > 0 || rotate > 0;

      readoutShader.setArray('ShakeHz', shakeHz, 1);
      readoutShader.setArray('ShakePhase', shakePhase, 3);
      readoutShader.setArray('ShakeAmp', shakeAmp, 3);
      readoutShader.setInt('ShakeActive', active ? 1 : 0);

      // The onset impulse. Ported as far as the uniforms and no further: it is
      // armed by an audio transient, and there is no audio. -1 is the plugin's
      // own "no onset".
      readoutShader.set('OnsetTau', -1);
      readoutShader.set('OnsetAmp', 0);
      readoutShader.set('OnsetHz', base);
      readoutShader.set('OnsetDecay', 0.25);

      //--- the flash ---------------------------------------------------
      flashPulse.fill(0);
      flashRGB.fill(0);
      const count = Math.min(flashes.length, kMaxFlashes);
      for (let j = 0; j < count; j += 1) {
        const f = flashes[j];
        flashPulse[j * 4 + 0] = now - f.centre + 0.5 * f.length;
        flashPulse[j * 4 + 1] = f.length;
        flashPulse[j * 4 + 2] = f.level;
        flashRGB[j * 3 + 0] = f.rgb[0];
        flashRGB[j * 3 + 1] = f.rgb[1];
        flashRGB[j * 3 + 2] = f.rgb[2];
      }
      readoutShader.setInt('FlashCount', count);
      readoutShader.setArray('FlashPulse', flashPulse, 4);
      readoutShader.setArray('FlashRGB', flashRGB, 3);

      //--- the light ---------------------------------------------------
      const mainsHz = controls.mainsHz(params.option('mains'));
      const omega = kTau * 2.0 * mainsHz;
      readoutShader.set('MainsOmega', omega);
      readoutShader.set('MainsPhase', mainsHz > 0 ? reducedAngle(omega, now) : 0);
      readoutShader.set('MainsDepth', mainsHz > 0 ? controls.flickerDepth(params.get('depth')) : 0);

      readoutShader.set('MixAmount', params.get('mix'));
      quad.draw();

      gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
    },
  };
}

//---------------------------------------------------------------------------
// The controls, read out of the plugin's own constructor. Same names, same
// groups, same order, same defaults, same dropdown elements.
//
// Audio Drive, Audio Band and the 64-bin Audio buffer are absent, for the
// reason at the top of this file, and so is the About block, which is a text
// line and four buttons that open a browser.
//
// The defaults are computed through the inverse mappings, the way the plugin's
// constructor computes them (`controls::ReadoutParam( 0.020f )`), rather than
// typed as decimals — so what the README says the default is, is the default.
//---------------------------------------------------------------------------

const ms = (seconds) => (seconds >= 0.01 ? `${(seconds * 1000).toFixed(1)} ms` : `${(seconds * 1000).toFixed(2)} ms`);

mountDemo({
  name: 'Readout',
  pluginId: 'RO01',
  tagline:
    'A CMOS rolling shutter. A sensor does not expose a frame, it exposes and reads its rows one after another — so every pixel has a sample window and the picture is sampled there. Nothing else. Skew, jello, flash banding and mains flicker all fall out of that one idea rather than being drawn.',
  repo: 'https://github.com/stoatworks-labs/readout',

  showBackdrop: true,

  params: [
    { id: 'readout', name: 'Readout Time', type: 'standard', default: controls.readoutParam(0.02), group: 'Sensor',
      display: (v) => ms(controls.readoutSeconds(v)),
      hint: 'How long the sensor takes to read every row. One frame at 60 fps is 16.7 ms, which is where the classic skew lives; 60 ms is three and a half frames and comes apart into ribbons.' },
    { id: 'exposure', name: 'Exposure', type: 'standard', default: controls.exposureParam(0.004), group: 'Sensor',
      display: (v) => ms(controls.exposureSeconds(v)),
      hint: 'How long each row integrates before it is read. Zero is an instantaneous sample — no motion blur — and is a legitimate setting.' },
    { id: 'direction', name: 'Direction', type: 'option', default: 0, group: 'Sensor',
      elements: ['Top Down', 'Bottom Up', 'Left Right', 'Right Left'],
      hint: 'Which edge is read first. Phones are usually top-down; a sensor read sideways leans a moving object horizontally instead, which is what a rotated phone does.' },
    { id: 'interpolation', name: 'Interpolation', type: 'option', default: 0, group: 'Sensor',
      elements: ['Blend', 'Hold'],
      hint: 'A real sensor samples a continuous scene; this one has the frames it was given. Blend interpolates between them, Hold keeps the older one until the newer exists. Blend is smoother and its sub-frame detail is the interpolation’s invention; Hold is honest and steps.' },
    { id: 'global', name: 'Global', type: 'boolean', default: 0, group: 'Sensor',
      hint: 'A global shutter: readout time zero, every row sampled at once. With nothing else switched on it returns the picture untouched — the null. `rotest --global` measures that as 0/255 of deviation.' },

    { id: 'amount', name: 'Amount', type: 'standard', default: 0, group: 'Shake',
      display: (v) => (v <= 0 ? 'still' : `${(controls.shakeAmplitude(v) * 100).toFixed(2)}% of height`),
      hint: 'Camera shake. Every row sees the camera in a different place, so a straight edge comes out as a wave. This is the jello.' },
    { id: 'frequency', name: 'Frequency', type: 'standard', default: controls.shakeFrequencyParam(8), group: 'Shake',
      display: (v) => `${controls.shakeFrequencyHz(v).toFixed(1)} Hz`,
      hint: 'A wobble slower than the readout leans the frame; one faster ripples it. One full cycle every 1/f of readout — `rotest --jello` measures the period.' },
    { id: 'rotation', name: 'Rotation', type: 'standard', default: 0, group: 'Shake',
      display: (v) => (v <= 0 ? 'none' : `${((controls.shakeRotationRadians(v) * 180) / kPi).toFixed(2)}°`),
      hint: 'How much of the shake is a twist rather than a shift. Centred, and in picture heights on both axes, so it is a rotation on a 16:9 frame rather than a shear.' },

    { id: 'fire', name: 'Fire', type: 'boolean', default: 0, group: 'Flash',
      hint: 'Fire one pulse now. The plugin declares this as FF_TYPE_EVENT and the host draws it as a button; the kit has no event type, so it is a toggle the renderer releases itself — which is why it blinks. One pulse lasts a frame or two, exactly as a real strobe does: for something you can look at, set Trigger to Interval.' },
    { id: 'trigger', name: 'Trigger', type: 'option', default: 0, group: 'Flash',
      elements: ['Off', 'Beat', 'Bar', 'Onset', 'Interval'],
      hint: 'Only Off and Interval do anything here. Beat and Bar follow Resolume’s transport and Onset follows a transient in the routed audio, and a browser has neither — they are still in the list because renumbering Interval would make a shared link mean something else in the host.' },
    { id: 'interval', name: 'Interval', type: 'standard', default: 0.624, group: 'Flash',
      display: (v) => `${controls.flashIntervalSeconds(v).toFixed(2)} s`,
      hint: 'How often Trigger: Interval fires.' },
    { id: 'length', name: 'Length', type: 'standard', default: controls.flashLengthParam(0.002), group: 'Flash',
      display: (v) => ms(controls.flashLengthSeconds(v)),
      hint: 'How long the pulse burns. A xenon strobe is a few hundred microseconds; a flash cue off an LED wall can be a whole field. The band is (Exposure + Length) / Readout of the frame high — `rotest --band` measures it.' },
    { id: 'phase', name: 'Phase', type: 'standard', default: 0.3, group: 'Flash',
      display: (v) => `${(controls.flashPhase(v) * 100).toFixed(0)}% down`,
      hint: 'Where in the readout the pulse lands, and therefore where the band sits in the frame.' },
    { id: 'level', name: 'Level', type: 'standard', default: 0.5, group: 'Flash',
      display: (v) => `×${controls.flashLevel(v).toFixed(2)}`,
      hint: 'How much light the pulse adds to a row whose exposure catches all of it. The default, 0.5, is unity after mapping.' },
    { id: 'colour', name: 'Colour', type: 'option', default: 0, group: 'Flash',
      elements: FLASH_COLOUR_NAMES,
      hint: 'Linear RGB. Warm and Cool are a tungsten and a daylight LED against a neutral flash; the saturated ones are gels, because a coloured strobe on a rolling shutter is the whole reason anybody asks.' },

    { id: 'mains', name: 'Mains', type: 'option', default: 0, group: 'Light',
      elements: ['Off', '50 Hz', '60 Hz'],
      hint: 'A mains-driven lamp is brightest twice per cycle, so at 50 Hz it pulses at 100 Hz. Beat that against the readout and you get rolling dark bands T_light / Readout of the frame apart — the reason phone video of a stage looks striped.' },
    { id: 'depth', name: 'Depth', type: 'standard', default: 0.5, group: 'Light',
      display: (v) => `${(controls.flickerDepth(v) * 100).toFixed(0)}%`,
      hint: 'Modulation depth of the light, about a mean of one. An LED on a cheap driver is nearly 100%; a big tungsten source barely flickers at all.' },

    { id: 'mix', name: 'Mix', type: 'standard', default: 1, group: 'Output',
      display: (v) => `${(v * 100).toFixed(0)}%`,
      hint: 'Against the newest frame of the ring — the picture as the host handed it over.' },
  ],

  // The geometry card first: it has straight lines for the jello to bend and a
  // rotating spoke, which is the propeller photograph that needs no code at all.
  // Then the moving clips, because a rolling shutter has nothing to say about a
  // still one — on Colour bars, with the shake off, this plugin is a no-op and
  // that is worth seeing too.
  sources: ['grid', 'spot', 'scene', 'detail', 'bars', 'ramp', 'alpha'],

  presets: {
    'Phone Camera': { readout: controls.readoutParam(0.016), exposure: controls.exposureParam(0.004) },
    'Slow Sensor': { readout: 1, exposure: controls.exposureParam(0.008) },
    'Jello': { readout: controls.readoutParam(0.045), amount: 0.72, frequency: controls.shakeFrequencyParam(9) },
    'Handheld': { readout: controls.readoutParam(0.022), amount: 0.45, frequency: controls.shakeFrequencyParam(5), rotation: 0.5 },
    'Whip Pan': { readout: 1, exposure: controls.exposureParam(0.02), amount: 0.55, frequency: controls.shakeFrequencyParam(3) },
    // Controls.h declares no inverse for the flash interval — the harness never
    // needed to ask for an exact one — so these two are slider positions with
    // the seconds they map to written down: 0.36 is 0.38 s, 0.7 is 1.3 s.
    'Strobe': { readout: controls.readoutParam(0.05), trigger: 4, interval: 0.36, length: controls.flashLengthParam(0.004), level: 0.75, colour: 3 },
    'Camera Flash': { readout: controls.readoutParam(0.04), trigger: 4, interval: 0.7, length: controls.flashLengthParam(0.0012), level: 1, colour: 0 },
    'Stage Flicker 50 Hz': { readout: controls.readoutParam(0.05), exposure: controls.exposureParam(0.002), mains: 1, depth: 0.9 },
    'Stage Flicker 60 Hz': { readout: controls.readoutParam(0.05), exposure: controls.exposureParam(0.002), mains: 2, depth: 0.9 },
    'Sideways Sensor': { readout: controls.readoutParam(0.05), direction: 2 },
    'Hold, not Blend': { readout: 1, interpolation: 1 },
    'Global Shutter': { global: 1 },
  },

  blurb:
    "It is Readout's own GLSL — the capture pass and the readout pass — ported from the repository to WebGL2 and running on generated clips in this page, with the frame ring and the control arithmetic ported alongside them. Same parameters, same maths, no install.",

  differences: [
    'The audio side is not here at all. The plugin takes Resolume’s 64-bin spectrum as a parameter and uses it three ways: Audio Drive scales the shake, a transient kicks a damped impulse into the camera, and Trigger: Onset fires the flash. A browser has no equivalent, and asking for your microphone to demonstrate a video effect is not a trade worth making — so Audio Drive and Audio Band are absent from the panel rather than present and dead, and the onset impulse in the shader is never armed.',
    'Trigger: Beat and Trigger: Bar never fire. They read Resolume’s transport, and there is no transport here. They stay in the dropdown because the element list is the plugin’s and dropping two entries would renumber Interval, so a link copied from this page would mean something different in the host.',
    'Fire is a toggle here, not an event. FFGL has FF_TYPE_EVENT and the kit’s parameter model does not, so the renderer releases the button itself once it has queued the pulse. A single pulse lasts a frame or two — a real strobe does too — so Trigger: Interval is the one to use for something you can look at.',
    'The frame ring is the plugin’s, at its full depth of up to 16 frames, but it is filled at your display’s refresh rate rather than at a composition’s frame rate — and only when this page’s clock is actually running. Pausing freezes the ring instead of flushing it with copies of one frame, which the plugin would do under a host that kept calling it.',
    'The two shaders are the plugin’s own text and `demo/tools/check_shaders.py` proves it, but the control arithmetic, the frame ring and the flash schedule are a hand port of `source/Controls.cpp`, `source/Ring.cpp` and `Readout::ProcessOpenGL`. Nothing checks that port but a reader.',
    'Nothing on this page is measured. The plugin’s claims are numbers: a bar at 24 px/frame leans 24.000 px, a flash bands 215.0 rows of 720 against an expected 216.0, 50 and 60 Hz bands sit exactly 120.00 and 100.00 rows apart, and Global returns the input with 0/255 of deviation — with a negative control so the check can fail. That is `tools/verify.sh` in the repository, not this page.',
  ],

  createRenderer,
});
