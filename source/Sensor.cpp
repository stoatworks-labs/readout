#include "Sensor.h"

#include "Controls.h"

namespace readout::sensor
{
namespace
{
/// The band-limited noise: four sinusoids around the base frequency, with
/// incommensurable ratios so the sum never repeats within a take, and
/// weights falling off away from the centre. Normalised so the peak
/// displacement is the amplitude the control names.
constexpr float kShakeRatio[ kShakeComponents ]  = { 1.0f, 1.47f, 0.63f, 2.31f };
constexpr float kShakeWeight[ kShakeComponents ] = { 1.0f, 0.45f, 0.35f, 0.18f };
constexpr float kShakeWeightSum                  = 1.98f;

/// Fixed phase offsets per component and axis, so x, y and rotation are not
/// in step. Golden-angle spaced, which is as far from a pattern as three
/// numbers get.
constexpr double kPhaseOffset[ kShakeComponents ][ 3 ] = {
	{ 0.0, 2.399963, 4.799926 },
	{ 0.916298, 3.316261, 5.716224 },
	{ 1.832596, 4.232559, 0.349337 },
	{ 2.748894, 5.148857, 1.265635 },
};

/// The FFGL build's Interval fires when `now + 1e-6 >= next`. The same
/// allowance, so a schedule that lands on a frame exactly lands on it here
/// too rather than a frame late.
constexpr double kFireSlack = 1e-6;
} // namespace

float reducedAngle( double omega, double t )
{
	const double a = std::fmod( omega * t, kTau );
	return static_cast< float >( a < 0.0 ? a + kTau : a );
}

int ringSlots( float readoutSeconds, float exposureSeconds, double frameSeconds )
{
	const double windowFrames = ( static_cast< double >( readoutSeconds ) + exposureSeconds ) / frameSeconds;
	int slots                 = static_cast< int >( std::ceil( windowFrames ) ) + 2;
	return std::clamp( ( ( slots + 3 ) / 4 ) * 4, 4, kMaxSlots );
}

void setShake( Uniforms& u, float baseHz, float translate, float rotate, double now )
{
	for( int i = 0; i < kShakeComponents; ++i )
	{
		const float f   = baseHz * kShakeRatio[ i ];
		const float w   = kShakeWeight[ i ] / kShakeWeightSum;
		u.shakeHz[ i ] = f;
		for( int axis = 0; axis < 3; ++axis )
			u.shakePhase[ i * 3 + axis ] = reducedAngle( kTau * f, now + kPhaseOffset[ i ][ axis ] / ( kTau * f ) );
		u.shakeAmp[ i * 3 + 0 ] = translate * w * 0.8f;
		u.shakeAmp[ i * 3 + 1 ] = translate * w;
		u.shakeAmp[ i * 3 + 2 ] = rotate * w;
	}
	u.shakeActive = ( translate > 0.0f || rotate > 0.0f ) ? 1 : 0;
}

void setShakeForTest( Uniforms& u, float hz, float amplitude, double now )
{
	for( int i = 0; i < kShakeComponents; ++i )
		u.shakeHz[ i ] = 0.0f;
	for( int i = 0; i < kShakeComponents * 3; ++i )
	{
		u.shakePhase[ i ] = 0.0f;
		u.shakeAmp[ i ]   = 0.0f;
	}
	u.shakeHz[ 0 ]    = hz;
	u.shakeAmp[ 0 ]   = amplitude;
	u.shakePhase[ 0 ] = reducedAngle( kTau * hz, now );
	u.shakeActive     = amplitude > 0.0f ? 1 : 0;
}

Pulse firePulse( double now, float readoutSeconds, float phase, float length, float level, float colour )
{
	Pulse pulse;
	pulse.centre = now - static_cast< double >( readoutSeconds ) * ( 1.0 - controls::FlashPhase( phase ) );
	pulse.length = controls::FlashLengthSeconds( length );
	pulse.level  = controls::FlashLevel( level );
	controls::FlashColour( colour, pulse.rgb );
	return pulse;
}

bool pulseSpent( const Pulse& pulse, double now, float readoutSeconds, float exposureSeconds )
{
	const double begin = ( now - pulse.centre ) + 0.5 * pulse.length;
	return begin - pulse.length > readoutSeconds + exposureSeconds + 0.002;
}

void setFlashes( Uniforms& u, const Pulse* pulses, int count, double now )
{
	count        = std::clamp( count, 0, kMaxFlashes );
	u.flashCount = count;
	for( int j = 0; j < kMaxFlashes; ++j )
	{
		for( int c = 0; c < 4; ++c )
			u.flashPulse[ j * 4 + c ] = 0.0f;
		for( int c = 0; c < 3; ++c )
			u.flashRGB[ j * 3 + c ] = 0.0f;
	}
	for( int j = 0; j < count; ++j )
	{
		const Pulse& p           = pulses[ j ];
		u.flashPulse[ j * 4 + 0 ] = static_cast< float >( ( now - p.centre ) + 0.5 * p.length );
		u.flashPulse[ j * 4 + 1 ] = p.length;
		u.flashPulse[ j * 4 + 2 ] = p.level;
		u.flashRGB[ j * 3 + 0 ]   = p.rgb[ 0 ];
		u.flashRGB[ j * 3 + 1 ]   = p.rgb[ 1 ];
		u.flashRGB[ j * 3 + 2 ]   = p.rgb[ 2 ];
	}
}

void setMains( Uniforms& u, float mainsHz, float depth, double now )
{
	const double omega = kTau * 2.0 * mainsHz;
	u.mainsOmega       = static_cast< float >( omega );
	u.mainsPhase       = mainsHz > 0.0f ? reducedAngle( omega, now ) : 0.0f;
	u.mainsDepth       = mainsHz > 0.0f ? controls::FlickerDepth( depth ) : 0.0f;
}

//---------------------------------------------------------------------------
float readoutSecondsOf( const Settings& s )
{
	return s.global > 0.5f ? 0.0f : controls::ReadoutSeconds( s.readout );
}

int scheduledPulses( const Settings& s, Schedule schedule, double startSeconds, double fps, double now,
                     Pulse out[ kMaxFlashes ] )
{
	if( schedule == Schedule::Off || !( fps > 0.0 ) )
		return 0;

	const float readoutSeconds = readoutSecondsOf( s );
	const float exposure       = controls::ExposureSeconds( s.exposure );

	//The frame a scheduled instant actually fires on: the first frame at or
	//after it, which is the first frame whose `now` the FFGL build's test
	//`now + 1e-6 >= next` passes on.
	const auto fireTime = [ fps ]( double instant ) {
		return std::ceil( ( instant - kFireSlack ) * fps ) / fps;
	};
	const auto fired = [ now ]( double when ) { return when <= now + 1e-9; };

	//The newest firing at or before now. Interval counts n = 0, 1, 2 ... from
	//the start; Once is n = 0 alone.
	const double interval = controls::FlashIntervalSeconds( s.interval );
	long newest           = -1;
	if( schedule == Schedule::Once )
	{
		newest = fired( fireTime( startSeconds ) ) ? 0 : -1;
	}
	else
	{
		if( now + kFireSlack < startSeconds )
			return 0;
		newest = static_cast< long >( std::floor( ( now - startSeconds + kFireSlack ) / interval ) );
		//floor() on a quotient of doubles can land either side of an integer;
		//settle it against the frame each firing actually lands on.
		while( fired( fireTime( startSeconds + static_cast< double >( newest + 1 ) * interval ) ) )
			++newest;
		while( newest >= 0 && !fired( fireTime( startSeconds + static_cast< double >( newest ) * interval ) ) )
			--newest;
	}
	if( newest < 0 )
		return 0;

	//Oldest first, as the FFGL build keeps them, and only the ones whose light
	//can still reach a row.
	int count       = 0;
	const long from = std::max( 0L, newest - ( kMaxFlashes - 1 ) );
	for( long n = from; n <= newest; ++n )
	{
		const double when = fireTime( startSeconds + static_cast< double >( n ) * interval );
		const Pulse pulse = firePulse( when, readoutSeconds, s.phase, s.length, s.level, s.colour );
		if( !pulseSpent( pulse, now, readoutSeconds, exposure ) )
			out[ count++ ] = pulse;
	}
	return count;
}

int oldestAge( const Settings& s, double fps )
{
	const double frameSeconds  = 1.0 / fps;
	const float readoutSeconds = readoutSecondsOf( s );
	const float exposure       = controls::ExposureSeconds( s.exposure );
	const float readoutFrames  = static_cast< float >( readoutSeconds / frameSeconds );
	const float exposureFrames = static_cast< float >( exposure / frameSeconds );

	//The first row read has the oldest window: tau = Readout exactly. Its
	//far edge plus one neighbour is the oldest frame either basis can weigh.
	const float far = readoutFrames + exposureFrames;
	const int slots = ringSlots( readoutSeconds, exposure, frameSeconds );
	return std::clamp( static_cast< int >( std::floor( far ) ) + 1, 0, slots - 1 );
}

Uniforms uniformsAt( const Settings& s, double now, double fps, int width, int height, int filled,
                     const Pulse* pulses, int pulseCount )
{
	const double frameSeconds  = 1.0 / fps;
	const float readoutSeconds = readoutSecondsOf( s );
	const float exposure       = controls::ExposureSeconds( s.exposure );

	Uniforms u;
	u.slots  = ringSlots( readoutSeconds, exposure, frameSeconds );
	u.filled = std::clamp( filled, 1, u.slots );
	u.width  = width;
	u.height = height;

	u.frameSeconds   = static_cast< float >( frameSeconds );
	u.readoutFrames  = static_cast< float >( readoutSeconds / frameSeconds );
	u.exposureFrames = static_cast< float >( exposure / frameSeconds );
	u.direction      = std::clamp( static_cast< int >( std::lround( s.direction ) ), 0, 3 );
	u.hold           = std::lround( s.interpolation ) == 1 ? 1 : 0;

	//The FFGL build's translate is `Amplitude * ( 1 + drive * level ) +
	//drive * 0.004 * level`, and with no audio both terms are exactly zero.
	setShake( u, controls::ShakeFrequencyHz( s.frequency ), controls::ShakeAmplitude( s.amount ),
	          controls::ShakeRotationRadians( s.rotation ), now );

	setFlashes( u, pulses, pulseCount, now );
	setMains( u, controls::MainsHz( s.mains ), s.depth, now );
	u.mixAmount = s.mix;
	return u;
}

} // namespace readout::sensor
