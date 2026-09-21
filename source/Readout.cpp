#include "Readout.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace readout;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Readout >,                                    // Create method
	"RO01",                                                      // Plugin unique ID of maximum length 4.
	"SW Readout",                                                // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"A CMOS rolling shutter.\n\nA sensor reads its rows one after another, and each row was exposed for a moment before it was read. Every pixel gets a sample time, and the picture is sampled there. Nothing else.\n\nWhat falls out: moving things lean (skew), a shaken camera wobbles the frame (jello), a flash shorter than the readout lights a band of rows, and a mains-driven light paints rolling bands.\n\nGlobal turns the readout off for comparison.",// Plugin description
	"Readout FFGL effect"                                        // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame is allowed to advance the clock by.
/// The host's clock is not ours: it jumps when the composition is scrubbed,
/// when a clip is retriggered, and by however long the machine was asleep.
constexpr double kMaxFrameDelta = 0.25;

constexpr double kTau = 6.283185307179586;

const char* const kDirectionNames[]     = { "Top Down", "Bottom Up", "Left Right", "Right Left" };
const char* const kInterpolationNames[] = { "Blend", "Hold" };
const char* const kTriggerNames[]       = { "Off", "Beat", "Bar", "Onset", "Interval" };
const char* const kMainsNames[]         = { "Off", "50 Hz", "60 Hz" };

/// Which part of the spectrum shakes the camera. A subwoofer's air moves a
/// tripod; a hi-hat does not, but a snare on a stage floor might.
const char* const kBandNames[] = { "Full Range", "Low", "Mid", "High" };
constexpr int kBandCount       = 4;
constexpr int kBandRange[ kBandCount ][ 2 ] = {
	{ 0, 63 },
	{ 0, 7 },
	{ 8, 27 },
	{ 28, 63 },
};

/// The band-limited noise: four sinusoids around the base frequency, with
/// incommensurable ratios so the sum never repeats within a take, and
/// weights falling off away from the centre. Normalised so the peak
/// displacement is the amplitude the control names.
constexpr float kShakeRatio[ Readout::kShakeComponents ]  = { 1.0f, 1.47f, 0.63f, 2.31f };
constexpr float kShakeWeight[ Readout::kShakeComponents ] = { 1.0f, 0.45f, 0.35f, 0.18f };
constexpr float kShakeWeightSum                           = 1.98f;

/// Fixed phase offsets per component and axis, so x, y and rotation are not
/// in step. Golden-angle spaced, which is as far from a pattern as three
/// numbers get.
constexpr double kPhaseOffset[ Readout::kShakeComponents ][ 3 ] = {
	{ 0.0, 2.399963, 4.799926 },
	{ 0.916298, 3.316261, 5.716224 },
	{ 1.832596, 4.232559, 0.349337 },
	{ 2.748894, 5.148857, 1.265635 },
};

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// An angle reduced into [0, 2pi) in double, before it is handed to a
/// float. `fmod( omega * t, 2pi )` at t = 500,000 s is exact to about 1e-10
/// in double and meaningless in float.
float reducedAngle( double omega, double t )
{
	const double a = std::fmod( omega * t, kTau );
	return static_cast< float >( a < 0.0 ? a + kTau : a );
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

//---------------------------------------------------------------------------
Readout::Readout()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Everything here is a function of when a row was read, so the effect
	//needs the host's clock: a re-render of the same composition must skew
	//the same frame the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults, in the slider's units, computed from the physical value so
	// that what the README says the default is, is the default. SetParamInfof
	// reads each one back out of GetFloatParameter.
	//
	// They add up to a 20 ms readout with a 4 ms exposure and nothing else:
	// a sensor a little slower than a phone's, on a still tripod, with no
	// flash and no flicker. Moving content leans; nothing else happens until
	// asked. The null is Global.
	//---------------------------------------------------------------------
	params[ PT_READOUT ]       = controls::ReadoutParam( 0.020f );
	params[ PT_EXPOSURE ]      = controls::ExposureParam( 0.004f );
	params[ PT_DIRECTION ]     = static_cast< float >( kTopDown );
	params[ PT_INTERPOLATION ] = 0.0f;//Blend
	params[ PT_GLOBAL ]        = 0.0f;

	params[ PT_AMOUNT ]      = 0.0f;
	params[ PT_FREQUENCY ]   = controls::ShakeFrequencyParam( 8.0f );
	params[ PT_ROTATION ]    = 0.0f;
	params[ PT_AUDIO_DRIVE ] = 0.0f;//off until somebody routes audio to it
	params[ PT_AUDIO_BAND ]  = 1.0f;//Low: the air that moves a tripod

	params[ PT_FIRE ]     = 0.0f;
	params[ PT_TRIGGER ]  = static_cast< float >( kTriggerOff );
	params[ PT_INTERVAL ] = 0.624f;//about one second
	params[ PT_LENGTH ]   = controls::FlashLengthParam( 0.002f );
	params[ PT_PHASE ]    = 0.3f;
	params[ PT_LEVEL ]    = 0.5f;//unity after mapping
	params[ PT_COLOUR ]   = 0.0f;//White

	params[ PT_MAINS ] = 0.0f;//Off
	params[ PT_DEPTH ] = 0.5f;

	params[ PT_MIX ] = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every ranged parameter is a plain 0..1 float even where
	// it stands for milliseconds: SetParamInfo clamps an FF_TYPE_STANDARD
	// default into 0..1 *before* a range can be attached (SDK b1afaf9). The
	// conversions live in Controls.cpp.
	//
	// Option lists are declared in their natural order and NOT sorted: every
	// one here is a progression (a direction, a schedule, a frequency) or a
	// palette, and alphabetical would file 50 Hz under 5 and Off under O.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* const* names, int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), names[ i ], static_cast< float >( i ) );
	};

	SetParamInfof( PT_READOUT, "Readout Time", FF_TYPE_STANDARD );
	SetParamInfof( PT_EXPOSURE, "Exposure", FF_TYPE_STANDARD );
	declareOptions( PT_DIRECTION, "Direction", kDirectionNames, kDirectionCount );
	declareOptions( PT_INTERPOLATION, "Interpolation", kInterpolationNames, 2 );
	SetParamInfo( PT_GLOBAL, "Global", FF_TYPE_BOOLEAN, false );

	SetParamInfof( PT_AMOUNT, "Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_FREQUENCY, "Frequency", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROTATION, "Rotation", FF_TYPE_STANDARD );

	// The spectrum. Declared with a real element list so the host knows how
	// many bins to fill. Audio Drive defaults to zero: with no audio routed
	// the camera sits still rather than twitching to a phantom signal.
	SetBufferParamInfo( PT_AUDIO_FFT, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO_FFT, static_cast< unsigned int >( i ), "", 0.0f );
	SetParamInfof( PT_AUDIO_DRIVE, "Audio Drive", FF_TYPE_STANDARD );
	declareOptions( PT_AUDIO_BAND, "Audio Band", kBandNames, kBandCount );

	//An event, which the host draws as a button. The only control here that
	//is an instruction rather than a value.
	SetParamInfo( PT_FIRE, "Fire", FF_TYPE_EVENT, false );
	declareOptions( PT_TRIGGER, "Trigger", kTriggerNames, kTriggerCount );
	SetParamInfof( PT_INTERVAL, "Interval", FF_TYPE_STANDARD );
	SetParamInfof( PT_LENGTH, "Length", FF_TYPE_STANDARD );
	SetParamInfof( PT_PHASE, "Phase", FF_TYPE_STANDARD );
	SetParamInfof( PT_LEVEL, "Level", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_COLOUR, "Colour", controls::kFlashColourCount, params[ PT_COLOUR ] );
	for( int i = 0; i < controls::kFlashColourCount; ++i )
		SetParamElementInfo( PT_COLOUR, static_cast< unsigned int >( i ), controls::FlashColourName( i ),
		                     static_cast< float >( i ) );

	declareOptions( PT_MAINS, "Mains", kMainsNames, 3 );
	SetParamInfof( PT_DEPTH, "Depth", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//Twenty-one parameters is past the point where an ungrouped list in
	//somebody else's inspector stops being readable.
	for( FFUInt32 i = PT_READOUT; i <= PT_GLOBAL; ++i )
		SetParamGroup( i, "Sensor" );
	for( FFUInt32 i = PT_AMOUNT; i <= PT_AUDIO_BAND; ++i )
		SetParamGroup( i, "Shake" );
	for( FFUInt32 i = PT_FIRE; i <= PT_COLOUR; ++i )
		SetParamGroup( i, "Flash" );
	for( FFUInt32 i = PT_MAINS; i <= PT_DEPTH; ++i )
		SetParamGroup( i, "Light" );
	SetParamGroup( PT_MIX, "Output" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Readout effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Readout::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not
	//compile it is almost always the driver or the GL version, and knowing
	//which machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &captureShader, shaders::kCaptureShader, "capture" },
		{ &readoutShader, shaders::kReadoutShader, "readout" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Readout: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Readout: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	head   = 0;
	filled = 0;

	diag::info( "initialised, ring up to " + std::to_string( kMaxSlots ) + " frames" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Readout::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

double Readout::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
	{
		//No host clock at all. The wall clock is already in seconds, so the
		//unit question does not arise.
		return wallNow - wallStart;
	}

	const double raw = hostTime;

	//Decide the unit by measuring the host's clock against a real one: the
	//ratio is ~1 for a seconds host and ~1000 for a milliseconds host, and
	//nothing plausible sits between. Several frames rather than one, so a
	//single odd frame cannot decide it alone.
	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
void Readout::updateAudio( double now, double dt )
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO_FFT );
	if( info == nullptr )
		return;

	//Fast up, slow down. A shake that arrives a frame late reads as broken;
	//one that takes a moment to die away reads as a room with a speaker in
	//it. About 120 ms.
	const float release = dt > 0.0 ? 1.0f - std::exp( static_cast< float >( -dt / 0.12 ) ) : 1.0f;

	const size_t bins = std::min< size_t >( info->elements.size(), kAudioBins );
	for( size_t i = 0; i < bins; ++i )
	{
		//sqrt because bin magnitudes bunch hard against zero: a spectrum used
		//raw moves the picture for the kick drum and for nothing else.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );
		if( raw >= audioBins[ i ] )
			audioBins[ i ] = raw;
		else
			audioBins[ i ] += ( raw - audioBins[ i ] ) * release;
	}

	//Fold the chosen band down to one number: the mean, not the peak. A peak
	//follows whichever bin is loudest and jumps between them.
	const int band = std::clamp( static_cast< int >( std::lround( params[ PT_AUDIO_BAND ] ) ), 0, kBandCount - 1 );
	const int from = kBandRange[ band ][ 0 ];
	const int to   = std::min( kBandRange[ band ][ 1 ], static_cast< int >( bins ) - 1 );

	float sum   = 0.0f;
	int counted = 0;
	for( int i = from; i <= to; ++i )
	{
		sum += audioBins[ i ];
		++counted;
	}
	audioLevel = counted > 0 ? std::clamp( sum / static_cast< float >( counted ), 0.0f, 1.0f ) : 0.0f;

	//An onset is the level jumping clear of its own recent average: a rising
	//crossing, with a short lockout so a level dithering on the line does
	//not fire every frame. The average has a half-second memory, which is
	//long enough to sit under a beat and short enough to follow a change of
	//song.
	const float follow = static_cast< float >( std::min( 1.0, dt / 0.5 ) );
	audioSlow += ( audioLevel - audioSlow ) * follow;
	const float excess = audioLevel - audioSlow;

	constexpr float kOnsetStep = 0.10f;
	const bool crossed         = excess > kOnsetStep && audioExcess <= kOnsetStep;
	const bool armed           = lastOnset < 0.0 || ( now - lastOnset ) > 0.12;
	audioExcess                = excess;

	if( crossed && armed )
	{
		lastOnset = now;
		//The thump: a fraction of a picture height, scaled by how hard the
		//audio is allowed to push and how far the level jumped.
		onsetAmp = controls::AudioDrive( params[ PT_AUDIO_DRIVE ] ) * 0.004f * std::min( 1.0f, excess * 4.0f );

		if( std::lround( params[ PT_TRIGGER ] ) == kTriggerOnset )
			firePending = true;
	}
}

//---------------------------------------------------------------------------
void Readout::fire( double now, float readoutSeconds )
{
	Flash flash;
	//The pulse is centred on the phase point of THIS frame's readout: the
	//first row was read `readout` ago, the last row now, so phase p is
	//`readout * ( 1 - p )` seconds before now.
	flash.centre = now - static_cast< double >( readoutSeconds ) * ( 1.0 - controls::FlashPhase( params[ PT_PHASE ] ) );
	flash.length = controls::FlashLengthSeconds( params[ PT_LENGTH ] );
	flash.level  = controls::FlashLevel( params[ PT_LEVEL ] );
	controls::FlashColour( params[ PT_COLOUR ], flash.rgb );

	flashes.push_back( flash );
	while( flashes.size() > static_cast< size_t >( kMaxFlashes ) )
		flashes.erase( flashes.begin() );
}

//---------------------------------------------------------------------------
FFResult Readout::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it. The
	//capture pass sizes the viewport to the ring, and nothing in the SDK
	//puts a viewport back -- ScopedFBOBinding restores the framebuffer
	//binding and only that. The readout pass draws to the host's own
	//framebuffer and has nothing of its own to size itself from.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock, and the frame period measured from it.
	//---------------------------------------------------------------------
	const double now = nowSeconds();

	double dt = 0.0;
	if( lastNow >= 0.0 && now > lastNow )
	{
		dt = std::min( now - lastNow, kMaxFrameDelta );
		//Smoothed, because a single long frame must not turn a 20 ms readout
		//into half a frame for one frame. A steady host converges exactly.
		const double delta = std::clamp( dt, 1.0 / 240.0, 1.0 / 10.0 );
		frameSeconds += ( delta - frameSeconds ) * 0.15;
	}
	lastNow = now;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clockScale ) + " seconds=" + std::to_string( now )
		            + " frame=" + std::to_string( frameSeconds ) + " bpm=" + std::to_string( bpm )
		            + " barPhase=" + std::to_string( barPhase ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const bool global          = params[ PT_GLOBAL ] > 0.5f;
	const float readoutSeconds = global ? 0.0f : controls::ReadoutSeconds( params[ PT_READOUT ] );
	const float exposure       = controls::ExposureSeconds( params[ PT_EXPOSURE ] );
	const int direction        = std::clamp( static_cast< int >( std::lround( params[ PT_DIRECTION ] ) ), 0, kDirectionCount - 1 );
	const int hold             = std::lround( params[ PT_INTERPOLATION ] ) == 1 ? 1 : 0;
	const int trigger          = std::clamp( static_cast< int >( std::lround( params[ PT_TRIGGER ] ) ), 0, kTriggerCount - 1 );
	const float drive          = controls::AudioDrive( params[ PT_AUDIO_DRIVE ] );

	//Before the triggers, because an onset fires a flash and that has to be
	//visible on this frame rather than the next.
	updateAudio( now, dt );

	//---------------------------------------------------------------------
	// The flash schedule.
	//
	// Beat and Bar read the host's transport: the position within the bar
	// arrives every frame, and a beat is that position crossing a quarter.
	// Interval is a counter from the first frame it was switched on, not
	// arithmetic on the clock: `floor( now / interval )` lands a hair under
	// the integer on exactly the frames that should fire.
	//---------------------------------------------------------------------
	{
		const float within  = std::clamp( barPhase, 0.0f, 0.999999f );
		const int beatIndex = static_cast< int >( std::floor( within * 4.0f ) );

		if( trigger == kTriggerBeat && lastBeatIndex >= 0 && beatIndex != lastBeatIndex )
			firePending = true;
		if( trigger == kTriggerBar && lastBarPhase >= 0.0f && within < lastBarPhase )
			firePending = true;

		lastBeatIndex = beatIndex;
		lastBarPhase  = within;

		if( trigger == kTriggerInterval )
		{
			if( nextInterval < 0.0 || now + 1e-6 >= nextInterval )
			{
				firePending  = true;
				nextInterval = now + controls::FlashIntervalSeconds( params[ PT_INTERVAL ] );
			}
		}
		else
		{
			nextInterval = -1.0;
		}
	}

	if( firePending )
	{
		firePending = false;
		fire( now, readoutSeconds );
	}

	//Drop every pulse that every row's window has passed.
	for( size_t i = 0; i < flashes.size(); )
	{
		const double begin = ( now - flashes[ i ].centre ) + 0.5 * flashes[ i ].length;
		if( begin - flashes[ i ].length > readoutSeconds + exposure + 0.002 )
			flashes.erase( flashes.begin() + static_cast< long >( i ) );
		else
			++i;
	}

	//---------------------------------------------------------------------
	// The ring. Deep enough for the oldest row's window, plus a frame each
	// side for the interpolation, rounded up to a multiple of four so that
	// dragging Readout Time reallocates -- and empties -- the ring at four
	// boundaries rather than at every millisecond.
	//
	// Every Ensure() happens here, before anything binds a texture. Not
	// tidiness: allocating a texture leaves unit 0 bound to nothing, and the
	// symptom of getting the order wrong is correct on every frame except
	// the one that allocates.
	//---------------------------------------------------------------------
	const double windowFrames = ( static_cast< double >( readoutSeconds ) + exposure ) / frameSeconds;
	int slots                 = static_cast< int >( std::ceil( windowFrames ) ) + 2;
	slots                     = std::clamp( ( ( slots + 3 ) / 4 ) * 4, 4, kMaxSlots );

	if( !ring.Ensure( pictureWidth, pictureHeight, slots ) )
	{
		diag::error( "could not allocate the frame ring: " + std::to_string( slots ) + " x "
		             + std::to_string( pictureWidth ) + "x" + std::to_string( pictureHeight )
		             + " - try a shorter Readout Time or Exposure" );
		return FF_FAIL;
	}
	if( ring.WasReshaped() )
	{
		head   = 0;
		filled = 0;
		diag::info( "ring rebuilt: " + std::to_string( slots ) + " x " + std::to_string( pictureWidth ) + "x"
		            + std::to_string( pictureHeight ) + " = "
		            + std::to_string( ( static_cast< long long >( slots ) * pictureWidth * pictureHeight * 4 )
		                              / ( 1024 * 1024 ) )
		            + " MB" );
	}
	slots = ring.Slots();

	//---------------------------------------------------------------------
	// 1. Capture. The head advances every frame; the input goes into it.
	//---------------------------------------------------------------------
	if( filled > 0 )
		head = ( head + 1 ) % slots;
	filled = std::min( filled + 1, slots );

	{
		ring.BeginCapture( head );
		ScopedShaderBinding shader( captureShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		captureShader.Set( "InputTexture", 0 );
		captureShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		captureShader.Set( "HalfTexel", 0.5f / static_cast< float >( pictureWidth ),
		                   0.5f / static_cast< float >( pictureHeight ) );
		quad.Draw();
		ring.EndCapture();
	}

	//---------------------------------------------------------------------
	// 2. Readout, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( readoutShader.GetGLID() );
		const GLuint program = readoutShader.GetGLID();

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D_ARRAY, ring.TextureID() );

		readoutShader.Set( "Ring", 0 );
		readoutShader.Set( "Slots", slots );
		readoutShader.Set( "Head", head );
		readoutShader.Set( "Filled", filled );
		readoutShader.Set( "PictureSize", static_cast< float >( pictureWidth ), static_cast< float >( pictureHeight ) );

		//In frames, computed in double: the harness sets a readout of exactly
		//one frame, and 1.0 has to arrive as 1.0.
		readoutShader.Set( "FrameSeconds", static_cast< float >( frameSeconds ) );
		readoutShader.Set( "ReadoutFrames", static_cast< float >( readoutSeconds / frameSeconds ) );
		readoutShader.Set( "ExposureFrames", static_cast< float >( exposure / frameSeconds ) );
		readoutShader.Set( "Direction", direction );
		readoutShader.Set( "Hold", hold );

		//--- the camera ---------------------------------------------------
		float hz[ kShakeComponents ]         = {};
		float phase[ kShakeComponents * 3 ]  = {};
		float amp[ kShakeComponents * 3 ]    = {};
		bool active                          = false;

		if( shakeOverride )
		{
			hz[ 0 ]    = shakeTestHz;
			amp[ 0 ]   = shakeTestAmp;
			phase[ 0 ] = reducedAngle( kTau * shakeTestHz, now );
			active     = shakeTestAmp > 0.0f;
		}
		else
		{
			const float base       = controls::ShakeFrequencyHz( params[ PT_FREQUENCY ] );
			const float audioGain  = 1.0f + drive * audioLevel;
			//The audio's own contribution stands even with Amount at zero: a
			//speaker next to a still tripod still moves it.
			const float translate  = controls::ShakeAmplitude( params[ PT_AMOUNT ] ) * audioGain + drive * 0.004f * audioLevel;
			const float rotate     = controls::ShakeRotationRadians( params[ PT_ROTATION ] ) * audioGain;

			for( int i = 0; i < kShakeComponents; ++i )
			{
				const float f = base * kShakeRatio[ i ];
				const float w = kShakeWeight[ i ] / kShakeWeightSum;
				hz[ i ]       = f;
				for( int axis = 0; axis < 3; ++axis )
					phase[ i * 3 + axis ] = reducedAngle( kTau * f, now + kPhaseOffset[ i ][ axis ] / ( kTau * f ) );
				amp[ i * 3 + 0 ] = translate * w * 0.8f;
				amp[ i * 3 + 1 ] = translate * w;
				amp[ i * 3 + 2 ] = rotate * w;
			}
			active = translate > 0.0f || rotate > 0.0f;
		}

		float onsetTau = -1.0f;
		if( lastOnset >= 0.0 && onsetAmp > 0.0f && now - lastOnset < 2.0 )
		{
			onsetTau = static_cast< float >( now - lastOnset );
			active   = true;
		}

		//FFGLShader::Set has no array overloads; these go through GL directly.
		glUniform1fv( glGetUniformLocation( program, "ShakeHz" ), kShakeComponents, hz );
		glUniform3fv( glGetUniformLocation( program, "ShakePhase" ), kShakeComponents, phase );
		glUniform3fv( glGetUniformLocation( program, "ShakeAmp" ), kShakeComponents, amp );
		readoutShader.Set( "ShakeActive", active ? 1 : 0 );
		readoutShader.Set( "OnsetTau", onsetTau );
		readoutShader.Set( "OnsetAmp", onsetAmp );
		readoutShader.Set( "OnsetHz", controls::ShakeFrequencyHz( params[ PT_FREQUENCY ] ) );
		readoutShader.Set( "OnsetDecay", 0.25f );

		//--- the flash -----------------------------------------------------
		float pulse[ kMaxFlashes * 4 ] = {};
		float rgb[ kMaxFlashes * 3 ]   = {};
		const int count                = static_cast< int >( flashes.size() );
		for( int j = 0; j < count; ++j )
		{
			const Flash& f     = flashes[ static_cast< size_t >( j ) ];
			pulse[ j * 4 + 0 ] = static_cast< float >( ( now - f.centre ) + 0.5 * f.length );
			pulse[ j * 4 + 1 ] = f.length;
			pulse[ j * 4 + 2 ] = f.level;
			rgb[ j * 3 + 0 ]   = f.rgb[ 0 ];
			rgb[ j * 3 + 1 ]   = f.rgb[ 1 ];
			rgb[ j * 3 + 2 ]   = f.rgb[ 2 ];
		}
		readoutShader.Set( "FlashCount", count );
		glUniform4fv( glGetUniformLocation( program, "FlashPulse" ), kMaxFlashes, pulse );
		glUniform3fv( glGetUniformLocation( program, "FlashRGB" ), kMaxFlashes, rgb );

		//--- the light -----------------------------------------------------
		const float mainsHz = controls::MainsHz( params[ PT_MAINS ] );
		const double omega  = kTau * 2.0 * mainsHz;
		readoutShader.Set( "MainsOmega", static_cast< float >( omega ) );
		readoutShader.Set( "MainsPhase", mainsHz > 0.0f ? reducedAngle( omega, now ) : 0.0f );
		readoutShader.Set( "MainsDepth", mainsHz > 0.0f ? controls::FlickerDepth( params[ PT_DEPTH ] ) : 0.0f );

		readoutShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();

		glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Readout::DeInitGL()
{
	captureShader.FreeGLResources();
	readoutShader.FreeGLResources();
	quad.Release();
	ring.Destroy();

	head   = 0;
	filled = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Readout::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_FIRE )
	{
		// Rising edge only. An event parameter goes to 1 and back to 0 as the
		// host draws the press and the release, and firing on both would flash
		// twice for one click. The press is only recorded here: this call runs
		// outside the render and may well arrive before the plugin has been
		// told what time it is, so the next ProcessOpenGL turns it into a pulse.
		if( value >= 0.5f && params[ PT_FIRE ] < 0.5f )
			firePending = true;
		params[ PT_FIRE ] = value;
		return FF_SUCCESS;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Readout::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Readout::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Readout::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Readout::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Readout::SetShakeForTest( float hz, float amplitude )
{
	shakeOverride = true;
	shakeTestHz   = hz;
	shakeTestAmp  = amplitude;
}
