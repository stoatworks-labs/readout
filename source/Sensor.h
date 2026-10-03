#pragma once

/**
	The sensor model with no GL in it: what the readout pass is handed, and
	-- for the OpenFX build, which has no GPU -- what it does with it.

	Two builds read this file. The FFGL plugin fills a `Uniforms` every frame
	from its controls, its clock, its flash list and its audio, and pushes it
	to `kReadoutShader` field for field. The OpenFX plugin fills the same
	struct from the same helpers, with no audio and no memory, and runs
	`readoutPixel` -- the readout shader's `main`, line for line in C++ --
	over the host's buffer instead.

	So the split is: everything that decides a uniform lives HERE once, and
	both builds call it; the per-pixel arithmetic lives twice, in
	`kReadoutShader` and in `readoutPixel` below, and every mirrored stage is
	marked `//= mirrored`. **Edit one, edit the other.** `rotest --mirror`
	renders both and compares them pixel for pixel, so a drift shows up as a
	number rather than as a look that is subtly different in Resolve.

	Nothing here may include the FFGL SDK or a GL header: the OpenFX build
	links it on Linux, where there is no GL loader at all.
*/

#include <algorithm>
#include <cmath>

namespace readout
{
/// The most frames the ring will hold. Sixteen full pictures is a quarter of
/// a gigabyte at 4K, and it covers a 60 ms readout plus a 40 ms exposure at
/// any frame rate up to about 140 fps; past that the oldest rows lose the
/// part of their window that falls beyond the sixteenth frame.
///
/// In the OpenFX build it bounds the temporal window instead: one output
/// frame fetches at most this many source frames, `t` back to `t - 15`.
constexpr int kMaxSlots = 16;

namespace sensor
{
constexpr int kShakeComponents = 4;

/// Pulses kept in flight. A pulse is dropped once every row's window has
/// passed it; four covers a strobe at the shortest interval.
constexpr int kMaxFlashes = 4;

constexpr double kTau = 6.283185307179586;

/// An angle reduced into [0, 2pi) in double, before it is handed to a
/// float. `fmod( omega * t, 2pi )` at t = 500,000 s is exact to about 1e-10
/// in double and meaningless in float.
float reducedAngle( double omega, double t );

/// How deep the ring is for a window: deep enough for the oldest row's
/// window, plus a frame each side for the interpolation, rounded up to a
/// multiple of four so that dragging Readout Time reallocates -- and empties
/// -- the FFGL ring at four boundaries rather than at every millisecond. The
/// OpenFX build uses the same number, so the two weigh the same frames.
int ringSlots( float readoutSeconds, float exposureSeconds, double frameSeconds );

/**
	Everything `kReadoutShader` is handed, by the names it uses. Arrays are
	laid out the way `glUniform3fv` and `glUniform4fv` read them.
*/
struct Uniforms
{
	int slots  = 4;///< Slots: layers in the ring
	int filled = 1;///< Filled: how many of them hold a frame at all
	int width  = 1;///< PictureSize.x, in pixels
	int height = 1;///< PictureSize.y

	float frameSeconds   = 1.0f / 60.0f;
	float readoutFrames  = 0.0f;
	float exposureFrames = 0.0f;
	int direction        = 0;
	int hold             = 0;

	int shakeActive                          = 0;
	float shakeHz[ kShakeComponents ]        = {};
	float shakePhase[ kShakeComponents * 3 ] = {};
	float shakeAmp[ kShakeComponents * 3 ]   = {};
	float onsetTau                           = -1.0f;
	float onsetAmp                           = 0.0f;
	float onsetHz                            = 0.0f;
	float onsetDecay                         = 0.25f;

	int flashCount                      = 0;
	float flashPulse[ kMaxFlashes * 4 ] = {};
	float flashRGB[ kMaxFlashes * 3 ]   = {};

	float mainsOmega = 0.0f;
	float mainsPhase = 0.0f;
	float mainsDepth = 0.0f;

	float mixAmount = 1.0f;
};

/// The camera: band-limited noise as four sinusoids around `baseHz`, each
/// with its phase at `now`, amplitudes for x and y in picture heights and
/// rotation in radians. `ShakeActive` is set only when something moves, so a
/// still camera takes the shader's exact, untransformed path.
void setShake( Uniforms& u, float baseHz, float translate, float rotate, double now );

/// The harness's single sinusoid in x, phase zero at time zero.
void setShakeForTest( Uniforms& u, float hz, float amplitude, double now );

/// A flash pulse, kept by its CENTRE in host seconds. The shader is handed
/// its BEGINNING relative to now instead; see `setFlashes`.
struct Pulse
{
	double centre = 0.0;
	float length  = 0.0f;
	float level   = 0.0f;
	float rgb[ 3 ] = { 1.0f, 1.0f, 1.0f };
};

/// A pulse fired on the frame whose timestamp is `now`: centred on the
/// Phase point of THAT frame's readout. The first row was read `readout`
/// before now and the last row at now, so phase p is `readout * ( 1 - p )`
/// seconds back. The other four arguments are the controls' 0..1 positions.
Pulse firePulse( double now, float readoutSeconds, float phase, float length, float level, float colour );

/// True once every row's window has passed the pulse, with 2 ms to spare.
bool pulseSpent( const Pulse& pulse, double now, float readoutSeconds, float exposureSeconds );

/// FlashCount, FlashPulse and FlashRGB, from the pulses in flight, oldest
/// first. At most `kMaxFlashes` are taken.
void setFlashes( Uniforms& u, const Pulse* pulses, int count, double now );

/// MainsOmega, MainsPhase and MainsDepth. Twice the mains frequency, because a
/// lamp's power goes as the square of the voltage. Off is a depth of zero.
void setMains( Uniforms& u, float mainsHz, float depth, double now );

//---------------------------------------------------------------------------
// A host with no memory: the OpenFX build.
//---------------------------------------------------------------------------

/// Every control the OpenFX build has, at the 0..1 positions -- and option
/// indices -- both builds' inspectors show. The FFGL-only ones (the audio
/// group, Beat, Bar, Onset and the Fire button) are not here.
struct Settings
{
	float readout       = 0.0f;
	float exposure      = 0.0f;
	float direction     = 0.0f;
	float interpolation = 0.0f;
	float global        = 0.0f;

	float amount    = 0.0f;
	float frequency = 0.0f;
	float rotation  = 0.0f;

	float interval = 0.0f;
	float length   = 0.0f;
	float phase    = 0.0f;
	float level    = 0.0f;
	float colour   = 0.0f;

	float mains = 0.0f;
	float depth = 0.0f;

	float mix = 1.0f;
};

/// Readout Time in seconds, after Global: zero for a global shutter.
float readoutSecondsOf( const Settings& s );

/// The flash schedule a timeline can have. The FFGL build's Fire button is
/// an event at wall-clock time and its Interval counts from whichever frame
/// it was switched on at; neither exists for a host that renders frames in
/// any order. So the schedule is anchored to the timeline instead: `Once`
/// fires on the first frame at or after `start`, `Interval` fires on the first
/// frame at or after every `start + n * interval`. Each pulse is then exactly
/// what the FFGL build fires on that frame.
enum class Schedule
{
	Off = 0,
	Once,
	Interval
};

/// The pulses such a schedule has fired by `now` and still has in flight,
/// oldest first. A pulse fired on a later frame is not one, even where it
/// would overlap an early row's window -- the FFGL build fires on the frame,
/// and this does what it does. Returns the count.
int scheduledPulses( const Settings& s, Schedule schedule, double startSeconds, double fps, double now,
                     Pulse out[ kMaxFlashes ] );

/// The newest frame the readout of `s` can weigh, as an age in frames. The
/// window is the ring's -- `ringSlots` frames -- and within it only ages up to
/// the oldest row's `tau + exposure` plus one interpolation neighbour carry
/// any weight. This is what the OpenFX build fetches: `t - 0` to `t - this`.
int oldestAge( const Settings& s, double fps );

/// Every uniform for one frame, from the controls, for a host with no audio,
/// no transport and an exact frame period. `filled` is how many frames of
/// history the host could give, the current one included.
Uniforms uniformsAt( const Settings& s, double now, double fps, int width, int height, int filled,
                     const Pulse* pulses, int pulseCount );

//---------------------------------------------------------------------------
// The readout shader's main, in C++.
//---------------------------------------------------------------------------

//= mirrored from kReadoutShader: hatIntegral, boxIntegral and slotWeight.
inline float hatIntegral( float x )
{
	if( x <= -1.0f )
		return 0.0f;
	if( x < 0.0f )
	{
		const float u = x + 1.0f;
		return 0.5f * u * u;
	}
	if( x < 1.0f )
	{
		const float u = 1.0f - x;
		return 1.0f - 0.5f * u * u;
	}
	return 1.0f;
}

inline float boxIntegral( float x )
{
	return std::clamp( x + 1.0f, 0.0f, 1.0f );
}

inline float slotWeight( float k, float a0, float a1, int hold )
{
	const float span = a1 - a0;
	if( span < 1e-6f )
	{
		const float x = a0 - k;
		if( hold == 1 )
			return ( x > -1.0f && x <= 0.0f ) ? 1.0f : 0.0f;
		return std::max( 0.0f, 1.0f - std::fabs( x ) );
	}
	if( hold == 1 )
		return ( boxIntegral( a1 - k ) - boxIntegral( a0 - k ) ) / span;
	return ( hatIntegral( a1 - k ) - hatIntegral( a0 - k ) ) / span;
}

/// GL_LINEAR with CLAMP_TO_EDGE: what `texture( Ring, ... )` returns between
/// texel centres. `texel( x, y, rgba )` reads one texel, row 0 at the bottom.
template< class Texel >
inline void linear( const Texel& texel, int w, int h, float u, float v, float out[ 4 ] )
{
	const float fx = u * static_cast< float >( w ) - 0.5f;
	const float fy = v * static_cast< float >( h ) - 0.5f;
	const float x0 = std::floor( fx );
	const float y0 = std::floor( fy );
	const float ax = fx - x0;
	const float ay = fy - y0;

	const int ix0 = std::clamp( static_cast< int >( x0 ), 0, w - 1 );
	const int iy0 = std::clamp( static_cast< int >( y0 ), 0, h - 1 );
	const int ix1 = std::clamp( static_cast< int >( x0 ) + 1, 0, w - 1 );
	const int iy1 = std::clamp( static_cast< int >( y0 ) + 1, 0, h - 1 );

	float p00[ 4 ], p10[ 4 ], p01[ 4 ], p11[ 4 ];
	texel( ix0, iy0, p00 );
	texel( ix1, iy0, p10 );
	texel( ix0, iy1, p01 );
	texel( ix1, iy1, p11 );
	for( int c = 0; c < 4; ++c )
	{
		const float bottom = p00[ c ] + ( p10[ c ] - p00[ c ] ) * ax;
		const float top    = p01[ c ] + ( p11[ c ] - p01[ c ] ) * ax;
		out[ c ]           = bottom + ( top - bottom ) * ay;
	}
}

/// When one pixel's row was read, in the shader's own terms: `tau` frames
/// before the timestamp, the window [a0, a1] in frames, and `tauR` and `E` in
/// seconds. `x` and `y` count from the bottom left, as GL and OpenFX do.
struct RowTime
{
	float tau  = 0.0f;
	float a0   = 0.0f;
	float a1   = 0.0f;
	float tauR = 0.0f;
	float E    = 0.0f;
};

/// What the vertex shader interpolates to at this pixel's centre.
inline void pixelUV( const Uniforms& u, int x, int y, float& uvx, float& uvy )
{
	uvx = ( static_cast< float >( x ) + 0.5f ) / static_cast< float >( u.width );
	uvy = ( static_cast< float >( y ) + 0.5f ) / static_cast< float >( u.height );
}

inline RowTime rowTime( const Uniforms& u, int x, int y )
{
	float uvx, uvy;
	pixelUV( u, x, y, uvx, uvy );

	//= mirrored from kReadoutShader: which row, and when it was read.
	float rows, row;
	if( u.direction == 0 )
	{
		rows = static_cast< float >( u.height );
		row  = std::floor( ( 1.0f - uvy ) * rows );
	}
	else if( u.direction == 1 )
	{
		rows = static_cast< float >( u.height );
		row  = std::floor( uvy * rows );
	}
	else if( u.direction == 2 )
	{
		rows = static_cast< float >( u.width );
		row  = std::floor( uvx * rows );
	}
	else
	{
		rows = static_cast< float >( u.width );
		row  = std::floor( ( 1.0f - uvx ) * rows );
	}
	row = std::clamp( row, 0.0f, rows - 1.0f );

	RowTime t;
	t.tau  = u.readoutFrames * ( 1.0f - row / rows );
	t.a0   = t.tau;
	t.a1   = t.tau + u.exposureFrames;
	t.tauR = t.tau * u.frameSeconds;
	t.E    = u.exposureFrames * u.frameSeconds;
	return t;
}

/// Where the camera puts this pixel's sample, in 0..1 picture space: the
/// shader's `sampleUV`. Returns false -- and the pixel centre -- when the
/// camera is still, which is the shader's exact, untransformed path.
inline bool cameraSample( const Uniforms& u, int x, int y, const RowTime& t, float& sampleU, float& sampleV )
{
	float uvx, uvy;
	pixelUV( u, x, y, uvx, uvy );
	sampleU = uvx;
	sampleV = uvy;
	if( u.shakeActive != 1 )
		return false;

	//= mirrored from kReadoutShader: the camera, at the middle of the window.
	const float kTauF = 6.283185307179586f;
	const float tauC  = t.tauR + 0.5f * t.E;

	float dx = 0.0f, dy = 0.0f, rot = 0.0f;
	for( int i = 0; i < kShakeComponents; ++i )
	{
		const float arg = kTauF * u.shakeHz[ i ] * tauC;
		dx += u.shakeAmp[ i * 3 + 0 ] * std::sin( u.shakePhase[ i * 3 + 0 ] - arg );
		dy += u.shakeAmp[ i * 3 + 1 ] * std::sin( u.shakePhase[ i * 3 + 1 ] - arg );
		rot += u.shakeAmp[ i * 3 + 2 ] * std::sin( u.shakePhase[ i * 3 + 2 ] - arg );
	}

	if( u.onsetTau >= 0.0f && tauC < u.onsetTau )
	{
		const float since = u.onsetTau - tauC;
		const float w     = u.onsetAmp * std::exp( -since / u.onsetDecay ) * std::sin( kTauF * u.onsetHz * since );
		dx += 0.3f * w;
		dy += w;
	}

	const float aspect = static_cast< float >( u.width ) / static_cast< float >( u.height );
	const float px     = ( uvx - 0.5f ) * aspect;
	const float py     = uvy - 0.5f;
	const float c      = std::cos( rot );
	const float s      = std::sin( rot );
	const float rx     = px * c - py * s + dx;
	const float ry     = px * s + py * c + dy;
	sampleU            = rx / aspect + 0.5f;
	sampleV            = ry + 0.5f;
	return true;
}

/**
	One output pixel: `kReadoutShader`'s main, in C++.

	`x` and `y` are the pixel in the picture, row 0 at the BOTTOM, the way GL
	and OpenFX both count. `frames` is the ring: `frames.texel( age, x, y,
	rgba )` reads one premultiplied texel of the frame `age` frames old, and
	`age` is always below `u.filled`.

	`out` is premultiplied and unclamped: the GPU's clamp is the RGBA8
	framebuffer it writes to, and a float host has no such thing.
*/
template< class Frames >
inline void readoutPixel( const Uniforms& u, int x, int y, const Frames& frames, float out[ 4 ] )
{
	const RowTime t  = rowTime( u, x, y );
	const float a0   = t.a0;
	const float a1   = t.a1;
	const float tauR = t.tauR;
	const float E    = t.E;

	float sampleU, sampleV;
	const bool moved = cameraSample( u, x, y, t, sampleU, sampleV );

	//= mirrored from kReadoutShader: the scene, integrated across the ring
	//over the window. Outside the picture there is nothing to show.
	float colour[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if( sampleU >= 0.0f && sampleU <= 1.0f && sampleV >= 0.0f && sampleV <= 1.0f )
	{
		float sum[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float total    = 0.0f;
		for( int k = 0; k < u.slots; ++k )
		{
			const float w = slotWeight( static_cast< float >( k ), a0, a1, u.hold );
			if( w <= 0.0f )
				continue;

			const int age = std::min( k, u.filled - 1 );
			float texel[ 4 ];
			//A still camera samples at the pixel centre, where GL_LINEAR
			//returns the texel itself; reading it directly is that, exactly.
			if( moved )
				linear( [ & ]( int tx, int ty, float rgba[ 4 ] ) { frames.texel( age, tx, ty, rgba ); },
				        u.width, u.height, sampleU, sampleV, texel );
			else
				frames.texel( age, x, y, texel );

			for( int c = 0; c < 4; ++c )
				sum[ c ] += w * texel[ c ];
			total += w;
		}
		if( total > 0.0f )
			for( int c = 0; c < 4; ++c )
				colour[ c ] = sum[ c ] / total;
	}

	//= mirrored from kReadoutShader: the flash.
	float flash[ 3 ] = { 0.0f, 0.0f, 0.0f };
	for( int j = 0; j < u.flashCount; ++j )
	{
		const float begin  = u.flashPulse[ j * 4 + 0 ];
		const float length = u.flashPulse[ j * 4 + 1 ];
		const float level  = u.flashPulse[ j * 4 + 2 ];

		float frac;
		if( E < 1e-7f )
		{
			frac = ( tauR >= begin - length && tauR <= begin ) ? 1.0f : 0.0f;
		}
		else
		{
			const float lo = std::max( begin - length, tauR );
			const float hi = std::min( begin, tauR + E );
			frac           = std::clamp( std::max( 0.0f, hi - lo ) / std::min( length, E ), 0.0f, 1.0f );
		}
		for( int c = 0; c < 3; ++c )
			flash[ c ] += u.flashRGB[ j * 3 + c ] * ( level * frac );
	}

	//= mirrored from kReadoutShader: the mains light, averaged over the window.
	float light = 1.0f;
	if( u.mainsDepth > 0.0f )
	{
		if( E < 1e-7f )
			light = 1.0f - u.mainsDepth * std::cos( u.mainsPhase - u.mainsOmega * tauR );
		else
			light = 1.0f
			        - u.mainsDepth
			              * ( std::sin( u.mainsPhase - u.mainsOmega * tauR )
			                  - std::sin( u.mainsPhase - u.mainsOmega * ( tauR + E ) ) )
			              / ( u.mainsOmega * E );
	}

	//= mirrored from kReadoutShader: premultiplied throughout, then the mix
	//against this frame's own pixel. GLSL's mix is x * ( 1 - a ) + y * a.
	const float result[ 4 ] = {
		colour[ 0 ] * light + flash[ 0 ] * colour[ 3 ],
		colour[ 1 ] * light + flash[ 1 ] * colour[ 3 ],
		colour[ 2 ] * light + flash[ 2 ] * colour[ 3 ],
		colour[ 3 ],
	};

	float source[ 4 ];
	frames.texel( 0, x, y, source );
	for( int c = 0; c < 4; ++c )
		out[ c ] = source[ c ] * ( 1.0f - u.mixAmount ) + result[ c ] * u.mixAmount;
}

} // namespace sensor
} // namespace readout
