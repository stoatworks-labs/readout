#include "Shaders.h"

namespace readout::shaders
{

const char* const kVertexShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 1: capture, into the ring.
//---------------------------------------------------------------------------
const char* const kCaptureShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 2: readout, straight to the host.
//
// One idea. A CMOS sensor reads its rows one after another: row r of H is
// read `Readout * ( 1 - r / H )` before the frame's timestamp, and was
// exposed for `Exposure` before that. Every pixel gets a sample window, and
// the scene is integrated over it. Everything the plugin does is that window
// applied to something:
//
//   - the ring, which holds the scene at discrete times, so the window is
//     integrated across it with sub-frame weights (skew, blur, jello);
//   - the camera pose, evaluated at the window's centre (jello);
//   - the flash pulse, by how much of it the window overlaps (banding);
//   - the mains light, by its mean over the window (flicker bands).
//
// Time inside this shader is `tau`: seconds (or frames) BEFORE the frame's
// timestamp, never an absolute time. Resolume's clock has been seen at
// 499,217 seconds, and a float that size resolves to 0.03 s -- three whole
// readouts. Every absolute phase is reduced on the CPU in double and handed
// over as an angle.
//---------------------------------------------------------------------------
const char* const kReadoutShader = R"(#version 410 core

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
)";

} // namespace readout::shaders
