/// The OpenFX build of Readout, for DaVinci Resolve, Vegas, Nuke, Natron and
/// other OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// Everything that decides a number. `Controls.cpp` says what a slider
/// position means; `Sensor.cpp` holds the shake's sinusoids and their phases,
/// the flash pulse, the mains light, the ring depth and the whole `Uniforms`
/// struct the readout pass is handed. Both builds call the same functions, so
/// a 20 ms readout is the same 20 ms in Resolume and in Resolve.
///
/// What is MIRRORED -- written twice -- is the readout shader's per-pixel
/// main, as `sensor::readoutPixel` in Sensor.h, and only that. Every mirrored
/// stage there is marked `//= mirrored`. Change `kReadoutShader`, change
/// `readoutPixel`; `rotest --mirror` renders both and compares them.
///
/// ------------------------------------------------- the one real difference
///
/// **The ring is not a ring here.** The FFGL build keeps the last N frames in
/// an array texture because FFGL hands it one frame at a time, in order, and
/// there is no way to ask for a frame it was not given. OFX is the opposite:
/// frames render in any order, alone and concurrently, and the host will
/// fetch any frame of the source clip on request. So there is nothing to
/// keep -- ring age `k` is simply the source at `t - k`, fetched through
/// temporal clip access, and the readout weighs those frames exactly as the
/// shader weighs ring layers. (`afterglow` made the same substitution first.)
///
/// **The window is bounded at 16 frames**, the FFGL ring's own depth
/// (`kMaxSlots`), and within it only the frames some row can weigh are
/// fetched: `t` back to `t - oldestAge`, which is the first row's
/// `tau + exposure` plus one interpolation neighbour. At 60 fps the defaults
/// fetch three frames and the longest window the controls allow (60 ms +
/// 40 ms) fetches seven. Past about 140 fps the longest window outgrows 16
/// frames, and the oldest rows lose the part of it that falls beyond -- the
/// same thing the FFGL ring does, by the same rule.
///
/// Before the clip's first frame there is no history, and an age the clip
/// has not got reads the oldest frame there is -- which is exactly what the
/// FFGL ring does in the frames after it fills. So frame `N` rendered alone
/// is the frame the FFGL build renders at `N` after playing 0 to N-1.
///
/// ------------------------------------------------------ what is missing
///
/// **The audio side.** OFX has no spectrum to offer, so Audio Drive, Audio
/// Band and the Onset trigger are not declared rather than declared and dead.
/// Beat and Bar read Resolume's transport, and OFX has none.
///
/// **The Fire button** is an event at wall-clock time, and a frame that may be
/// rendered before the frame preceding it cannot depend on one. It becomes a
/// schedule anchored to the timeline instead -- Trigger `Once` fires on the
/// first frame at or after `Fire At`, and `Interval` fires on the first frame
/// at or after every `Fire At + n * Interval` (the FFGL build's Interval
/// counts from whichever frame it was switched on at). Each pulse is then the
/// pulse the FFGL build fires on that frame: centred on the Phase point of
/// that frame's readout, and alive until every row's window has passed it.
///
/// ------------------------------------------------------------- and tiles
///
/// Camera shake samples the picture away from the pixel being drawn, by up to
/// six percent of the frame and a two-degree turn, so no tile is safe.
/// `setSupportsTiles( false )` is a fact about the effect.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Sensor.h"

namespace
{
using namespace readout;

constexpr const char* kPluginIdentifier = "com.stoatworks.readout";
constexpr const char* kPluginName       = "Readout";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"A CMOS rolling shutter.\n\n"
	"A sensor reads its rows one after another, and each row was exposed for a "
	"moment before it was read. Every pixel gets a sample time, and the picture "
	"is sampled there. Nothing else.\n\n"
	"What falls out: moving things lean (skew), a shaken camera wobbles the frame "
	"(jello), a flash shorter than the readout lights a band of rows, and a "
	"mains-driven light paints rolling bands. Global turns the readout off for "
	"comparison.\n\n"
	"Each frame is read out of the clip's own previous frames -- up to 16 of them, "
	"fetched from the timeline -- so any frame renders on its own and the same way "
	"every time.\n\n"
	"The Resolume build's audio group (Audio Drive, Audio Band) and its Beat, Bar "
	"and Onset triggers are not here: OpenFX gives a plugin no audio and no "
	"transport. Its Fire button becomes Trigger: Once, at the time Fire At "
	"names.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamReadout       = "readoutTime";
constexpr const char* kParamExposure      = "exposure";
constexpr const char* kParamDirection     = "direction";
constexpr const char* kParamInterpolation = "interpolation";
constexpr const char* kParamGlobal        = "global";
constexpr const char* kParamAmount        = "amount";
constexpr const char* kParamFrequency     = "frequency";
constexpr const char* kParamRotation      = "rotation";
constexpr const char* kParamTrigger       = "trigger";
constexpr const char* kParamFireAt        = "fireAt";
constexpr const char* kParamInterval      = "interval";
constexpr const char* kParamLength        = "length";
constexpr const char* kParamPhase         = "phase";
constexpr const char* kParamLevel         = "level";
constexpr const char* kParamColour        = "colour";
constexpr const char* kParamMains         = "mains";
constexpr const char* kParamDepth         = "depth";
constexpr const char* kParamMix           = "mix";

/// The OpenFX Trigger list. NOT the FFGL one -- Beat, Bar and Onset need a
/// transport or an audio input, and Once stands in for the Fire button -- so
/// the indices differ between builds. Nothing crosses between them by index:
/// there are no factory presets.
const char* const kTriggerNames[] = { "Off", "Once", "Interval" };
constexpr int kTriggerCount       = 3;

//---------------------------------------------------------------------------
// The ring, as OFX has it: the source at t, t-1, t-2 ... as the host handed
// them over, read in place. Every layer shares the current frame's bounds and
// pixel format; render() stops fetching at the first one that does not.
//---------------------------------------------------------------------------
template< class PIX, int nComponents, int maxValue >
class History
{
public:
	struct Layer
	{
		const unsigned char* base = nullptr;///< the pixel at the bounds' (x1, y1)
		std::ptrdiff_t rowBytes   = 0;
	};

	Layer layers[ kMaxSlots ];
	int count          = 0;
	bool premultiplied = true;

	/// One texel of the frame `age` old, premultiplied float. `x` and `y` are
	/// picture coordinates, row 0 at the bottom.
	void texel( int age, int x, int y, float rgba[ 4 ] ) const
	{
		const Layer& layer = layers[ age ];
		const PIX* p       = reinterpret_cast< const PIX* >( layer.base + static_cast< std::ptrdiff_t >( y ) * layer.rowBytes )
		               + static_cast< std::size_t >( x ) * nComponents;

		//A unorm texel is v / max to the GPU as well; dividing rather than
		//multiplying by a reciprocal keeps the last bit the same.
		for( int c = 0; c < 3; ++c )
			rgba[ c ] = maxValue == 1 ? static_cast< float >( p[ c ] )
			                          : static_cast< float >( p[ c ] ) / static_cast< float >( maxValue );
		rgba[ 3 ] = nComponents == 4 ? ( maxValue == 1 ? static_cast< float >( p[ 3 ] )
		                                               : static_cast< float >( p[ 3 ] ) / static_cast< float >( maxValue ) )
		                             : 1.0f;

		//The model works premultiplied, as the FFGL build does. A clip the
		//host says is straight is multiplied up on the way in, or the blend
		//between two frames weighs colour by the wrong thing.
		if( !premultiplied && nComponents == 4 )
			for( int c = 0; c < 3; ++c )
				rgba[ c ] *= rgba[ 3 ];
	}
};

//---------------------------------------------------------------------------
// The per-pixel pass, across the host's threads.
//---------------------------------------------------------------------------
template< class PIX, int nComponents, int maxValue >
class ReadoutProcessor : public OFX::ImageProcessor
{
public:
	ReadoutProcessor( OFX::ImageEffect& effect, const sensor::Uniforms& uniforms,
	                  const History< PIX, nComponents, maxValue >& history, int originX, int originY ) :
		OFX::ImageProcessor( effect ),
		u( uniforms ),
		frames( history ),
		x0( originX ),
		y0( originY )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dst = static_cast< PIX* >( _dstImg->getPixelAddress( window.x1, y ) );
			if( dst == nullptr )
				continue;

			const int py = y - y0;
			for( int x = window.x1; x < window.x2; ++x, dst += nComponents )
			{
				const int px  = x - x0;
				float out[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
				if( px >= 0 && px < u.width && py >= 0 && py < u.height )
					sensor::readoutPixel( u, px, py, frames, out );

				float a = out[ 3 ];
				if( maxValue != 1 )
					a = std::clamp( a, 0.0f, 1.0f );

				for( int c = 0; c < 3; ++c )
				{
					float v = out[ c ];
					if( !frames.premultiplied && nComponents == 4 )
						v = a > 0.0f ? v / a : 0.0f;
					dst[ c ] = quantise( v );
				}
				if( nComponents == 4 )
					dst[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// Integer formats clamp and round, which is what the GPU's RGBA8
	/// framebuffer does to the FFGL build's output. Float is left alone: a
	/// flash at Level 2 is legitimately brighter than white, and a float host
	/// may want to keep it.
	static PIX quantise( float v )
	{
		if( maxValue == 1 )
			return static_cast< PIX >( v );
		v = std::clamp( v, 0.0f, 1.0f );
		return static_cast< PIX >( std::lround( v * static_cast< float >( maxValue ) ) );
	}

	const sensor::Uniforms& u;
	const History< PIX, nComponents, maxValue >& frames;
	int x0 = 0;
	int y0 = 0;
};

//---------------------------------------------------------------------------
class ReadoutPlugin : public OFX::ImageEffect
{
public:
	explicit ReadoutPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		readout       = fetchDoubleParam( kParamReadout );
		exposure      = fetchDoubleParam( kParamExposure );
		direction     = fetchChoiceParam( kParamDirection );
		interpolation = fetchChoiceParam( kParamInterpolation );
		global        = fetchBooleanParam( kParamGlobal );
		amount        = fetchDoubleParam( kParamAmount );
		frequency     = fetchDoubleParam( kParamFrequency );
		rotation      = fetchDoubleParam( kParamRotation );
		trigger       = fetchChoiceParam( kParamTrigger );
		fireAt        = fetchDoubleParam( kParamFireAt );
		interval      = fetchDoubleParam( kParamInterval );
		length        = fetchDoubleParam( kParamLength );
		phase         = fetchDoubleParam( kParamPhase );
		level         = fetchDoubleParam( kParamLevel );
		colour        = fetchChoiceParam( kParamColour );
		mains         = fetchChoiceParam( kParamMains );
		depth         = fetchDoubleParam( kParamDepth );
		mix           = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );
		if( !dst || !src )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depthEnum   = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		if( src->getPixelDepth() != depthEnum || src->getPixelComponents() != comps )
			OFX::throwSuiteStatusException( kOfxStatErrImageFormat );

		switch( depthEnum )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA ? renderAs< unsigned char, 4, 255 >( args, *dst, *src )
			                                  : renderAs< unsigned char, 3, 255 >( args, *dst, *src );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA ? renderAs< unsigned short, 4, 65535 >( args, *dst, *src )
			                                  : renderAs< unsigned short, 3, 65535 >( args, *dst, *src );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA ? renderAs< float, 4, 1 >( args, *dst, *src )
			                                  : renderAs< float, 3, 1 >( args, *dst, *src );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	/// The readout is a window of previous frames; tell the host so it can
	/// prefetch them. Without this a host is entitled to refuse the fetches,
	/// and every row would read the current frame -- a global shutter, with no
	/// error anywhere.
	void getFramesNeeded( const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames ) override
	{
		const int oldest = sensor::oldestAge( settingsAt( args.time ), frameRate() );
		OfxRangeD range;
		range.min = args.time - static_cast< double >( oldest );
		range.max = args.time;
		frames.setFramesNeeded( *srcClip, range );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		stoatworks::about::ofx::changedParam( args, paramName );
	}

private:
	/// Frames per second of the timeline this effect is rendering on. OFX time
	/// is in frames, and everything in the model is in seconds.
	double frameRate() const
	{
		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = srcClip->getFrameRate();
		return fps > 0.0 ? fps : 24.0;
	}

	sensor::Settings settingsAt( double t ) const
	{
		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		const auto choice = [ t ]( OFX::ChoiceParam* param ) {
			int value = 0;
			param->getValueAtTime( t, value );
			return static_cast< float >( value );
		};
		const auto slider = [ t ]( OFX::DoubleParam* param ) {
			return static_cast< float >( param->getValueAtTime( t ) );
		};

		sensor::Settings s;
		s.readout       = slider( readout );
		s.exposure      = slider( exposure );
		s.direction     = choice( direction );
		s.interpolation = choice( interpolation );
		s.global        = global->getValueAtTime( t ) ? 1.0f : 0.0f;
		s.amount        = slider( amount );
		s.frequency     = slider( frequency );
		s.rotation      = slider( rotation );
		s.interval      = slider( interval );
		s.length        = slider( length );
		s.phase         = slider( phase );
		s.level         = slider( level );
		s.colour        = choice( colour );
		s.mains         = choice( mains );
		s.depth         = slider( depth );
		s.mix           = slider( mix );
		return s;
	}

	template< class PIX, int nComponents, int maxValue >
	void renderAs( const OFX::RenderArguments& args, OFX::Image& dst, OFX::Image& src )
	{
		const OfxRectI bounds = src.getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		const double t   = args.time;
		const double fps = frameRate();
		const double now = t / fps;

		const sensor::Settings s = settingsAt( t );

		//An RGB clip has no alpha to be premultiplied by, and a host that says
		//"unpremultiplied" about one is describing something that does not
		//exist. Treating it as premultiplied is what makes the round trip an
		//identity there.
		History< PIX, nComponents, maxValue > history;
		history.premultiplied = nComponents != 4 || srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;
		history.layers[ 0 ].base     = static_cast< const unsigned char* >( src.getPixelData() );
		history.layers[ 0 ].rowBytes = src.getRowBytes();
		history.count                = 1;

		//Ring age k is the source at t - k. Stop at the first frame the clip
		//has not got -- before its start, refused, or a different shape -- and
		//every older age then reads the oldest frame there is, as the FFGL ring
		//does while it fills.
		const int oldest       = sensor::oldestAge( s, fps );
		const OfxRangeD range  = srcClip->getFrameRange();
		std::vector< std::unique_ptr< OFX::Image > > past;
		for( int k = 1; k <= oldest; ++k )
		{
			const double when = t - static_cast< double >( k );
			if( when < range.min - 1e-6 )
				break;

			std::unique_ptr< OFX::Image > image( srcClip->fetchImage( when ) );
			if( !image )
				break;

			const OfxRectI b = image->getBounds();
			if( b.x1 != bounds.x1 || b.y1 != bounds.y1 || b.x2 != bounds.x2 || b.y2 != bounds.y2
			    || image->getPixelDepth() != src.getPixelDepth()
			    || image->getPixelComponents() != src.getPixelComponents() )
				break;

			history.layers[ k ].base     = static_cast< const unsigned char* >( image->getPixelData() );
			history.layers[ k ].rowBytes = image->getRowBytes();
			history.count                = k + 1;
			past.push_back( std::move( image ) );
		}

		int schedule = 0;
		trigger->getValueAtTime( t, schedule );
		sensor::Pulse pulses[ sensor::kMaxFlashes ];
		const int pulseCount = sensor::scheduledPulses( s, static_cast< sensor::Schedule >( schedule ),
		                                                fireAt->getValueAtTime( t ), fps, now, pulses );

		const sensor::Uniforms u = sensor::uniformsAt( s, now, fps, width, height, history.count, pulses, pulseCount );

		ReadoutProcessor< PIX, nComponents, maxValue > processor( *this, u, history, bounds.x1, bounds.y1 );
		processor.setDstImg( &dst );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::DoubleParam* readout        = nullptr;
	OFX::DoubleParam* exposure       = nullptr;
	OFX::ChoiceParam* direction      = nullptr;
	OFX::ChoiceParam* interpolation  = nullptr;
	OFX::BooleanParam* global        = nullptr;
	OFX::DoubleParam* amount         = nullptr;
	OFX::DoubleParam* frequency      = nullptr;
	OFX::DoubleParam* rotation       = nullptr;
	OFX::ChoiceParam* trigger        = nullptr;
	OFX::DoubleParam* fireAt         = nullptr;
	OFX::DoubleParam* interval       = nullptr;
	OFX::DoubleParam* length         = nullptr;
	OFX::DoubleParam* phase          = nullptr;
	OFX::DoubleParam* level          = nullptr;
	OFX::ChoiceParam* colour         = nullptr;
	OFX::ChoiceParam* mains          = nullptr;
	OFX::DoubleParam* depth          = nullptr;
	OFX::DoubleParam* mix            = nullptr;
};

//---------------------------------------------------------------------------
// Describing.
//---------------------------------------------------------------------------
OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( name, name, name );
	page->addChild( *group );
	return group;
}

void defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                   OFX::GroupParamDescriptor* group, const char* name, const char* label, const char* hint,
                   double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                   OFX::GroupParamDescriptor* group, const char* name, const char* label, const char* hint,
                   int count, const char* ( *labelFor )( int ), int value )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( labelFor( i ) );
	param->setDefault( value );
	param->setAnimates( false );
	param->setParent( *group );
	page->addChild( *param );
}

mDeclarePluginFactory( ReadoutPluginFactory, {}, {} );
} // namespace

void ReadoutPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The shake samples away from the pixel being drawn, so no tiles. Temporal
	// access is the effect rather than a detail of it: every row is a different
	// moment, and the moments are earlier frames.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( true );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void ReadoutPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );
	srcClip->setTemporalClipAccess( true );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same 0..1 ranges, same defaults and same groups as the
	// FFGL build wherever the concept carries over, so the two inspectors read
	// alike and one set of docs covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	using namespace controls;

	//----------------------------------------------------------------- Sensor
	OFX::GroupParamDescriptor* sensorGroup = defineGroup( desc, page, "Sensor" );

	defineSlider( desc, page, sensorGroup, kParamReadout, "Readout Time",
	              "How long the sensor takes to read every row: 1 to 60 ms, geometric. One frame at "
	              "60 fps is 16.7 ms, which is where the classic skew lives; 60 ms comes apart into "
	              "ribbons.",
	              ReadoutParam( defaults::kReadoutSeconds ) );
	defineSlider( desc, page, sensorGroup, kParamExposure, "Exposure",
	              "How long each row integrates before it is read: 0 to 40 ms. Zero is an "
	              "instantaneous sample, with no motion blur at all.",
	              ExposureParam( defaults::kExposureSeconds ) );
	defineChoice( desc, page, sensorGroup, kParamDirection, "Direction",
	              "Which edge the sensor reads first. Left Right and Right Left are a sensor mounted "
	              "sideways: the rows run down the picture instead of across it.",
	              kDirectionCount, DirectionName, 0 );
	defineChoice( desc, page, sensorGroup, kParamInterpolation, "Interpolation",
	              "What happened between two source frames. Blend interpolates, and a moving bar leans "
	              "continuously; Hold keeps the older frame, and the lean stairsteps the way a sensor "
	              "fed by a low frame rate does.",
	              kInterpolationCount, InterpolationName, 0 );

	OFX::BooleanParamDescriptor* globalParam = desc.defineBooleanParam( kParamGlobal );
	globalParam->setLabels( "Global", "Global", "Global" );
	globalParam->setHint( "A global shutter, for comparison: every row read at once. With nothing else "
	                      "on, the picture comes back untouched." );
	globalParam->setDefault( false );
	globalParam->setParent( *sensorGroup );
	page->addChild( *globalParam );

	//------------------------------------------------------------------ Shake
	OFX::GroupParamDescriptor* shakeGroup = defineGroup( desc, page, "Shake" );

	defineSlider( desc, page, shakeGroup, kParamAmount, "Amount",
	              "How far the camera moves, up to 6% of the picture height. Every row sees it at a "
	              "different moment, and that is the jello.",
	              0.0 );
	defineSlider( desc, page, shakeGroup, kParamFrequency, "Frequency",
	              "2 to 80 Hz. A wobble slower than the readout leans the frame; one faster ripples it.",
	              ShakeFrequencyParam( defaults::kShakeHz ) );
	defineSlider( desc, page, shakeGroup, kParamRotation, "Rotation",
	              "How far the camera turns, up to two degrees.", 0.0 );

	//------------------------------------------------------------------ Flash
	OFX::GroupParamDescriptor* flashGroup = defineGroup( desc, page, "Flash" );

	OFX::ChoiceParamDescriptor* triggerParam = desc.defineChoiceParam( kParamTrigger );
	triggerParam->setLabels( "Trigger", "Trigger", "Trigger" );
	triggerParam->setHint( "When a flash fires. Once fires at Fire At; Interval fires at Fire At and every "
	                       "Interval after it. The Resolume build's Fire button, Beat, Bar and Onset do not "
	                       "exist here: an OpenFX host has no transport and no audio, and renders frames in "
	                       "any order." );
	for( const char* name : kTriggerNames )
		triggerParam->appendOption( name );
	triggerParam->setDefault( 0 );
	triggerParam->setAnimates( false );
	triggerParam->setParent( *flashGroup );
	page->addChild( *triggerParam );

	OFX::DoubleParamDescriptor* fireAtParam = desc.defineDoubleParam( kParamFireAt );
	fireAtParam->setLabels( "Fire At", "Fire At", "Fire At" );
	fireAtParam->setHint( "Seconds, on this effect's clock (frame number / frame rate), at which the flash "
	                      "fires -- on the first frame at or after it, centred on that frame's Phase point. "
	                      "Keyframe it to move the flash along the edit." );
	fireAtParam->setRange( 0.0, 86400.0 );
	fireAtParam->setDisplayRange( 0.0, 60.0 );
	fireAtParam->setDefault( 0.0 );
	fireAtParam->setParent( *flashGroup );
	page->addChild( *fireAtParam );

	defineSlider( desc, page, flashGroup, kParamInterval, "Interval",
	              "0.1 to 4 s between flashes, geometric, for the Interval trigger.", defaults::kInterval );
	defineSlider( desc, page, flashGroup, kParamLength, "Length",
	              "0.1 to 20 ms, geometric. A xenon strobe is a few hundred microseconds; shorter than "
	              "the readout, it lights a band rather than the frame.",
	              FlashLengthParam( defaults::kFlashLengthSeconds ) );
	defineSlider( desc, page, flashGroup, kParamPhase, "Phase",
	              "Where in the frame's readout the pulse lands, top to bottom.", defaults::kPhase );
	defineSlider( desc, page, flashGroup, kParamLevel, "Level",
	              "0 to 2: how much light the pulse adds to a row that catches all of it.",
	              defaults::kLevel );
	defineChoice( desc, page, flashGroup, kParamColour, "Colour",
	              "White, two colour temperatures and five gels.", kFlashColourCount, FlashColourName, 0 );

	//------------------------------------------------------------------ Light
	OFX::GroupParamDescriptor* lightGroup = defineGroup( desc, page, "Light" );

	defineChoice( desc, page, lightGroup, kParamMains, "Mains",
	              "A mains-driven lamp flickers at twice the mains frequency, and beats against the "
	              "readout into rolling bands.",
	              kMainsCount, MainsName, 0 );
	defineSlider( desc, page, lightGroup, kParamDepth, "Depth", "How deep the flicker is.", defaults::kDepth );

	//----------------------------------------------------------------- Output
	OFX::GroupParamDescriptor* outputGroup = defineGroup( desc, page, "Output" );
	defineSlider( desc, page, outputGroup, kParamMix, "Mix", "Wet/dry against the untouched clip.",
	              defaults::kMix );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* ReadoutPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new ReadoutPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static ReadoutPluginFactory* factory =
		new ReadoutPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
