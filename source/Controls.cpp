#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace readout::controls
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

inline float unlerp( float from, float to, float value )
{
	return clamp01( ( value - from ) / ( to - from ) );
}

/// Geometric interpolation. Equal slider movements are equal *ratios*.
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

inline float ungeometric( float from, float to, float value )
{
	return clamp01( std::log( value / from ) / std::log( to / from ) );
}

constexpr float kPi = 3.14159265358979323846f;

/// Linear RGB. Warm and Cool are a tungsten and a daylight LED against a
/// neutral flash; the saturated ones are gels, because a coloured strobe on
/// a rolling shutter is the whole reason anybody asks for a flash colour.
const float kFlashColours[ kFlashColourCount ][ 3 ] = {
	{ 1.00f, 1.00f, 1.00f },//White
	{ 1.00f, 0.80f, 0.55f },//Warm
	{ 0.75f, 0.88f, 1.00f },//Cool
	{ 1.00f, 0.55f, 0.10f },//Amber
	{ 1.00f, 0.08f, 0.08f },//Red
	{ 0.08f, 1.00f, 0.15f },//Green
	{ 0.10f, 0.25f, 1.00f },//Blue
	{ 1.00f, 0.10f, 0.90f },//Magenta
};

const char* const kFlashColourNames[ kFlashColourCount ] = {
	"White", "Warm", "Cool", "Amber", "Red", "Green", "Blue", "Magenta"
};
} // namespace

float ReadoutSeconds( float value )
{
	return geometric( 0.001f, 0.060f, value );
}

float ReadoutParam( float seconds )
{
	return ungeometric( 0.001f, 0.060f, seconds );
}

float ExposureSeconds( float value )
{
	return lerp( 0.0f, 0.040f, value );
}

float ExposureParam( float seconds )
{
	return unlerp( 0.0f, 0.040f, seconds );
}

float ShakeAmplitude( float value )
{
	//Geometric from a floor and floored to zero at the very bottom: a geometric
	//mapping cannot reach zero, and a camera that never quite stops shaking is
	//a control that has to be fought rather than used.
	if( value <= 0.0f )
		return 0.0f;

	return geometric( 0.0005f, 0.06f, value );
}

float ShakeFrequencyHz( float value )
{
	return geometric( 2.0f, 80.0f, value );
}

float ShakeFrequencyParam( float hz )
{
	return ungeometric( 2.0f, 80.0f, hz );
}

float ShakeRotationRadians( float value )
{
	if( value <= 0.0f )
		return 0.0f;

	return geometric( 0.02f, 2.0f, value ) * kPi / 180.0f;
}

float AudioDrive( float value )
{
	return lerp( 0.0f, 4.0f, value );
}

float FlashIntervalSeconds( float value )
{
	return geometric( 0.1f, 4.0f, value );
}

float FlashLengthSeconds( float value )
{
	return geometric( 0.0001f, 0.020f, value );
}

float FlashLengthParam( float seconds )
{
	return ungeometric( 0.0001f, 0.020f, seconds );
}

float FlashPhase( float value )
{
	return clamp01( value );
}

float FlashLevel( float value )
{
	return lerp( 0.0f, 2.0f, value );
}

float FlickerDepth( float value )
{
	return clamp01( value );
}

float MainsHz( float optionValue )
{
	switch( static_cast< int >( std::lround( optionValue ) ) )
	{
	case 1: return 50.0f;
	case 2: return 60.0f;
	default: return 0.0f;
	}
}

void FlashColour( float optionValue, float rgb[ 3 ] )
{
	const int index = std::clamp( static_cast< int >( std::lround( optionValue ) ), 0, kFlashColourCount - 1 );
	rgb[ 0 ]        = kFlashColours[ index ][ 0 ];
	rgb[ 1 ]        = kFlashColours[ index ][ 1 ];
	rgb[ 2 ]        = kFlashColours[ index ][ 2 ];
}

const char* FlashColourName( int index )
{
	return kFlashColourNames[ std::clamp( index, 0, kFlashColourCount - 1 ) ];
}

} // namespace readout::controls
