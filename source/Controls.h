#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
	float in 0..1, including the ones that stand for milliseconds or hertz.
	That is not a style preference: `CFFGLPluginManager::SetParamInfo` clamps
	a standard default into 0..1 *before* returning, and `SetParamRange` can
	only be called afterwards -- so a parameter declared in milliseconds cannot
	declare a default in milliseconds, and 20 would silently become 1. The
	conversions live here instead, in one file the plugin and the harness both
	use, so there is only ever one answer to what a slider position means.

	Every mapping has an inverse. The harness needs to say "a readout time of
	exactly one frame" and "a flash of exactly eight milliseconds" in the
	slider's own units, and computing the position by hand is how a check ends
	up testing a number near the one it claims.

	Where a mapping is geometric it is because the interesting range is at one
	end: the difference between a 2 ms and a 4 ms flash is a different band,
	the difference between 18 ms and 20 ms is not.
*/
namespace readout::controls
{

/// 1 to 60 ms, geometrically. How long the sensor takes to read every row.
/// One host frame at 60 fps is 16.7 ms, which is where the classic skew
/// lives; 60 ms is three and a half frames and comes apart into ribbons.
float ReadoutSeconds( float value );
float ReadoutParam( float seconds );

/// 0 to 40 ms, linear. How long each row integrates before it is read. Zero
/// is an instantaneous sample -- no motion blur -- and is a legitimate
/// setting, which is why this one is not geometric.
float ExposureSeconds( float value );
float ExposureParam( float seconds );

/// Shake amplitude: 0 to 0.06 of the picture height, geometric from a floor
/// and floored to exactly zero at the bottom of the travel.
float ShakeAmplitude( float value );

/// Shake frequency: 2 to 80 Hz, geometrically. Where the jello lives -- a
/// wobble slower than the readout leans the frame, one faster ripples it.
float ShakeFrequencyHz( float value );
float ShakeFrequencyParam( float hz );

/// Rotation amplitude: 0 to 2 degrees, in radians, floored to zero.
float ShakeRotationRadians( float value );

/// Audio drive: 0 to 4, linear. How many times the shake grows at full level.
float AudioDrive( float value );

/// Flash interval: 0.1 to 4 s, geometrically.
float FlashIntervalSeconds( float value );

/// Flash length: 0.1 to 20 ms, geometrically. A xenon strobe is a few hundred
/// microseconds; a "flash" from an LED wall cue can be a whole field.
float FlashLengthSeconds( float value );
float FlashLengthParam( float seconds );

/// Flash phase: 0 to 1 of the readout, linear. Where in the frame the pulse
/// lands.
float FlashPhase( float value );

/// Flash level: 0 to 2, linear. How much light the pulse adds to a row whose
/// exposure catches all of it.
float FlashLevel( float value );

/// Mains flicker depth: 0 to 1, linear. The modulation depth of the light,
/// about a mean of one.
float FlickerDepth( float value );

/// The mains frequency an option value stands for: 0 for Off, else 50 or 60.
float MainsHz( float optionValue );

/// The colour a Flash Colour option stands for, as linear RGB.
void FlashColour( float optionValue, float rgb[ 3 ] );
constexpr int kFlashColourCount = 8;
const char* FlashColourName( int index );

} // namespace readout::controls
