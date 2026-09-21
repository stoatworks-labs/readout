#pragma once

#include "Ring.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
	Readout -- a CMOS rolling shutter, as an FFGL effect.

	**The one idea.** A CMOS sensor exposes and reads its rows one after
	another. Row r of H is read `Readout * ( 1 - r / H )` before the frame's
	timestamp, and was exposed for `Exposure` before that. Every pixel gets a
	sample window, and the scene is sampled there. Nothing else. What falls
	out, rather than being drawn:

	  - skew: a bar moving at v leans by v * Readout;
	  - jello: camera shake sampled per row wobbles the whole frame;
	  - flash banding: a pulse shorter than the readout lights only the rows
	    whose window overlaps it, in a band ( Exposure + Length ) / Readout
	    of the frame high, with soft edges where the overlap is partial;
	  - flicker banding: a mains-driven light beats against the readout and
	    paints rolling bands whose period is ( 1 / 2f ) / Readout of the frame;
	  - the propeller and the partial-frame looks, from moving content.

	**Two passes**, in `Shaders.h`: capture the input into a ring of the last
	N frames, then read the ring out through the sample window. The ring is
	one array texture, see `Ring.h`. The camera shake, the flash schedule and
	the audio all happen on the CPU and reach the shader as a handful of
	uniforms; the shader knows nothing about beats or spectra, only about
	when a row was read.

	See AGENTS.md for the traps.
*/
class Readout : public CFFGLPlugin
{
public:
	Readout();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook. The offline harness DECLARES its unit rather than
	/// leaving the calibration to infer one -- an absolute time handed over in
	/// a single frame is genuinely ambiguous.
	void SetClockScaleForTest( double scale );

	/// Shake test hook: replace the band-limited noise with ONE sinusoid in x
	/// of this frequency and amplitude (in picture heights), phase zero at
	/// host time zero. The shader path is the shipped one; only the component
	/// table differs, which is what lets `rotest --jello` measure a period.
	void SetShakeForTest( float hz, float amplitude );

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Sensor
		PT_READOUT,
		PT_EXPOSURE,
		PT_DIRECTION,
		PT_INTERPOLATION,
		PT_GLOBAL,

		//Shake
		PT_AMOUNT,
		PT_FREQUENCY,
		PT_ROTATION,
		PT_AUDIO_FFT,
		PT_AUDIO_DRIVE,
		PT_AUDIO_BAND,

		//Flash
		PT_FIRE,
		PT_TRIGGER,
		PT_INTERVAL,
		PT_LENGTH,
		PT_PHASE,
		PT_LEVEL,
		PT_COLOUR,

		//Light
		PT_MAINS,
		PT_DEPTH,

		//Output
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	enum Direction
	{
		kTopDown = 0,
		kBottomUp,
		kLeftRight,
		kRightLeft,
		kDirectionCount
	};

	enum Trigger
	{
		kTriggerOff = 0,
		kTriggerBeat,
		kTriggerBar,
		kTriggerOnset,
		kTriggerInterval,
		kTriggerCount
	};

	/// Number of FFT bins the host is asked for. The fleet's figure, and what
	/// Resolume delivers.
	static constexpr int kAudioBins = 64;

	/// Pulses kept in flight. A pulse is dropped once every row's window has
	/// passed it; four covers a strobe at the shortest interval.
	static constexpr int kMaxFlashes = 4;

	static constexpr int kShakeComponents = 4;

private:
	/// The host's clock in seconds, whatever unit it arrived in, and the
	/// frame period measured from it.
	double nowSeconds();

	/// Read the host's spectrum, fold it to one level, and look for an onset.
	void updateAudio( double now, double dt );

	/// Queue a flash that begins at this frame's phase.
	void fire( double now, float readoutSeconds );

	ffglex::FFGLShader captureShader;
	ffglex::FFGLShader readoutShader;
	ffglex::FFGLScreenQuad quad;

	readout::Ring ring;

	//--- the ring ------------------------------------------------------------
	int head   = 0;///< the layer holding the newest frame
	int filled = 0;///< how many layers hold a frame at all

	//--- the clock -----------------------------------------------------------
	// Resolume sends SetTime in MILLISECONDS; this repo's harness sends
	// seconds; the header says nothing. Decided by comparing the host's clock
	// against a steady one over several frames.
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	/// The host's frame period, measured from its own deltas. It is what
	/// turns milliseconds of readout into frames of ring, so it has to be
	/// real: assume 60 and every skew is wrong at 50.
	double frameSeconds = 1.0 / 60.0;

	//--- the flash -----------------------------------------------------------
	struct Flash
	{
		double centre;///< host seconds the middle of the pulse landed
		float length;
		float level;
		float rgb[ 3 ];
	};
	std::vector< Flash > flashes;

	/// Set by the Fire button on its rising edge. -2 is "pressed, but this
	/// call has no idea what time it is"; ProcessOpenGL turns it into a pulse.
	bool firePending      = false;
	int lastBeatIndex     = -1;
	float lastBarPhase    = -1.0f;
	double nextInterval   = -1.0;

	//--- audio ---------------------------------------------------------------
	float audioBins[ kAudioBins ] = {};
	float audioLevel   = 0.0f;
	float audioSlow    = 0.0f;
	float audioExcess  = 0.0f;///< last frame's level above the slow average
	double lastOnset   = -1.0;
	float onsetAmp     = 0.0f;

	//--- the shake -----------------------------------------------------------
	bool shakeOverride  = false;
	float shakeTestHz   = 0.0f;
	float shakeTestAmp  = 0.0f;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
