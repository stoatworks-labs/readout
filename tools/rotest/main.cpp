/**
	rotest -- render Readout offline, and measure what its sensor is doing.

	Where a bar lands once a rolling shutter has read it is a fact. So is how
	tall a flash band is, how far apart the flicker bands sit, and what period
	a shaken camera wobbles the rows at. None of those is "looks about right":
	each is a closed-form number from the spec, and each check here renders a
	synthetic scene through the REAL plugin class in a headless GL context,
	reads the pixels back, and measures.

		rotest --out /tmp/frame.png     a picture, on the moving test card
		rotest --list                   every parameter, its kind and default
		rotest --skew                   a bar moving at v leans by v
		rotest --band                   a flash bands ( E + L ) / T_read of the frame
		rotest --flicker                mains bands sit T_light / T_read of the frame apart
		rotest --global                 Global on returns the input bit-exactly
		rotest --jello                  a sinusoidal shake wobbles at 1/f / T_read of the frame
		rotest --bench                  the render cost
		rotest --pipe                   raw frames in, raw frames out

	Every check drives a synthetic 60 fps clock through `SetTime`, so one host
	frame is exactly 16.667 ms and "a readout of one frame" means exactly that.
	The transport is driven too (120 bpm), and a spectrum can be injected with
	`--audio`, because the host is the only thing that ever supplies either
	and without them the Beat, Bar and Audio controls are correctly dead.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the same format as the fleet's other harnesses, so one build script can
	film any of them. `--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | rotest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Readout.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace readout;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources. Every one is a function of the frame number, because this plugin's
// entire output is the difference between one frame and the last: on a still
// picture a rolling shutter is provably invisible.
//
// Rows are top-first in every source, the way a picture is, and flipped on
// the way into GL. The checks read back through the same flip, so "row 0" is
// the top of the picture everywhere in this file.
//---------------------------------------------------------------------------
enum class Source
{
	Card,
	Bar,
	Flat
};

Source sourceFromName( const std::string& name )
{
	if( name == "bar" )
		return Source::Bar;
	if( name == "flat" )
		return Source::Flat;
	return Source::Card;
}

/// The test card. A fast bar for the skew, a slow bar for the propeller
/// look, six saturated colour bars for the flash colour, a smooth ramp for
/// the flicker's banding to show on, and a fine checker for the interpolation
/// to be readable against.
std::vector< unsigned char > buildCard( int width, int height, int frame )
{
	std::vector< unsigned char > card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	//The fast bar: 24 px/frame at 720p, scaled with the raster so the skew
	//is the same fraction of the picture at every size.
	const float fastX = std::fmod( 0.10f * w + t * 24.0f * ( w / 1280.0f ), 0.80f * w ) + 0.10f * w;
	const float fastW = 0.012f * w;

	//A slow one going the other way.
	const float slowX = 0.85f * w - std::fmod( t * 6.0f * ( w / 1280.0f ), 0.70f * w );
	const float slowW = 0.03f * w;

	//A disc on a Lissajous path: something with a curved edge.
	const float discX = 0.5f * w + 0.30f * w * std::sin( t * 0.11f );
	const float discY = 0.45f * h + 0.20f * h * std::sin( t * 0.077f + 1.1f );
	const float discR = 0.06f * h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			//A mid-grey graded surround, so a dimming flicker band and a
			//brightening flash both have room.
			float r = 0.30f + 0.10f * v;
			float g = 0.30f + 0.10f * v;
			float b = 0.34f + 0.10f * v;

			//Six saturated bars across the bottom eighth.
			if( v > 0.875f )
			{
				static const float bars[ 6 ][ 3 ] = {
					{ 1.0f, 0.1f, 0.1f }, { 0.1f, 1.0f, 0.1f }, { 0.1f, 0.1f, 1.0f },
					{ 0.1f, 1.0f, 1.0f }, { 1.0f, 0.1f, 1.0f }, { 1.0f, 1.0f, 0.1f }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			//A smooth ramp above them.
			else if( v > 0.75f )
			{
				r = g = b = 0.1f + 0.8f * u;
			}
			//A four-pixel checker in the top-left.
			else if( u < 0.25f && v < 0.25f )
			{
				const bool on = ( ( x / 4 ) + ( y / 4 ) ) % 2 == 0;
				r = g = b = on ? 0.85f : 0.10f;
			}

			const float px = static_cast< float >( x ) + 0.5f;
			if( std::fabs( px - slowX ) < slowW * 0.5f && v > 0.05f && v < 0.75f )
			{
				r = 0.95f;
				g = 0.65f;
				b = 0.20f;
			}
			if( std::fabs( px - fastX ) < fastW * 0.5f && v > 0.05f && v < 0.75f )
			{
				r = 0.95f;
				g = 0.95f;
				b = 0.95f;
			}

			const float dx   = px - discX;
			const float dy   = ( static_cast< float >( y ) + 0.5f ) - discY;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.2f ) );
				r                = r + ( 0.2f - r ) * edge;
				g                = g + ( 0.7f - g ) * edge;
				b                = b + ( 0.9f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}
	}

	return card;
}

/// A white vertical bar, `barWidth` pixels wide, on black, whose left edge
/// starts at `x0` and moves `speed` pixels per frame. Hard-edged: the
/// centroid of a hard bar is a sub-pixel measurement of where it is.
std::vector< unsigned char > buildBar( int width, int height, int frame, float x0, float speed, int barWidth )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );
	const float left  = x0 + speed * static_cast< float >( frame );
	const float right = left + static_cast< float >( barWidth );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float px = static_cast< float >( x ) + 0.5f;
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			const bool on  = px >= left && px < right;
			image[ i + 0 ] = image[ i + 1 ] = image[ i + 2 ] = on ? 255 : 0;
			image[ i + 3 ]                                   = 255;
		}
	}
	return image;
}

std::vector< unsigned char > buildFlat( int width, int height, float level )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4 );
	const unsigned char value = static_cast< unsigned char >( std::clamp( level, 0.0f, 1.0f ) * 255.0f + 0.5f );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = image[ i + 1 ] = image[ i + 2 ] = value;
		image[ i + 3 ]                                   = 255;
	}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Readout::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Readout& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Readout::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Readout& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Readout& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}

	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

bool set( Readout& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A synthetic spectrum, so the Audio controls have something to be a
// property of offline. Shaped, and shaped differently in each band: a
// pink-ish tilt with the bass loudest, a kick on a half-second grid confined
// to the low bins, and a steady hiss in the top ones. `SetParamElementValue`
// is the same public entry point the host uses, so the plugin cannot tell
// this from a real spectrum.
//---------------------------------------------------------------------------
void injectSpectrum( Readout& plugin, int fftIndex, float level, double seconds )
{
	if( fftIndex < 0 )
		return;

	const float kick = static_cast< float >( std::exp( -6.0 * std::fmod( seconds, 0.5 ) ) );

	for( int i = 0; i < Readout::kAudioBins; ++i )
	{
		const float t = static_cast< float >( i ) / 63.0f;
		float value   = 0.20f * std::exp( -3.2f * t );
		if( i < 8 )
			value += kick;
		if( i >= 28 )
			value += 0.12f;

		plugin.SetParamElementValue( static_cast< unsigned int >( fftIndex ), static_cast< unsigned int >( i ),
		                             std::clamp( value * level, 0.0f, 1.0f ) );
	}
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Readout plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct   = {};
	FFGLTextureStruct* inputs[ 1 ]  = { nullptr };
	ProcessOpenGLStruct process     = {};

	int fireIndex = -1;
	int fftIndex  = -1;
	float audio   = -1.0f;///< spectrum level, or negative for none
	int fireFrame = -1;   ///< press Fire before this frame, or never

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		fireIndex = indexOfParameter( plugin, "Fire" );
		fftIndex  = indexOfParameter( plugin, "Audio" );

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		sourceTexture = makeTexture( width, height, nullptr );
		outputTexture = makeTexture( width, height, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;

		//A synthetic transport, so the Beat and Bar triggers have a grid to
		//land on. Without it they read as dead: the host is the only thing
		//that ever supplies a tempo.
		plugin.SetBeatInfo( 120.0f, 0.0f );
		return true;
	}

	/// Render one frame of `pixels` (top row first) at frame number `frame`.
	bool render( int frame, const std::vector< unsigned char >& pixels )
	{
		const double seconds = static_cast< double >( frame ) / fps;

		//A synthetic clock, and it has to be synthetic: left to the wall
		//clock the harness renders a hundred frames in a few milliseconds,
		//so the plugin measures a frame period of nothing and a readout of
		//twenty milliseconds becomes thousands of frames of ring. The unit
		//is declared rather than inferred.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( seconds );

		//Four beats to the bar at 120 bpm is two seconds.
		const double bars = seconds / 2.0;
		plugin.SetBeatInfo( 120.0f, static_cast< float >( bars - std::floor( bars ) ) );

		if( audio >= 0.0f )
			injectSpectrum( plugin, fftIndex, audio, seconds );

		if( frame == fireFrame && fireIndex >= 0 )
			plugin.SetFloatParameter( static_cast< unsigned int >( fireIndex ), 1.0f );

		//Flipped on the way in: a picture arrives top row first and GL wants
		//bottom row first.
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;

		if( frame == fireFrame && fireIndex >= 0 )
			plugin.SetFloatParameter( static_cast< unsigned int >( fireIndex ), 0.0f );

		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	/// The output, top row first.
	std::vector< unsigned char > readBack()
	{
		return flipRows( readBackRaw( outputFBO, width, height ), width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}
};

//---------------------------------------------------------------------------
// Measurements.
//---------------------------------------------------------------------------

/// Intensity-weighted centre of a row, in pixels from the left edge, or a
/// negative number when the row is dark.
double rowCentroid( const std::vector< unsigned char >& image, int width, int row )
{
	double sum = 0.0, weight = 0.0;
	for( int x = 0; x < width; ++x )
	{
		const size_t i = ( static_cast< size_t >( row ) * width + x ) * 4;
		const double v = image[ i ] + image[ i + 1 ] + image[ i + 2 ];
		sum += v * ( x + 0.5 );
		weight += v;
	}
	return weight > 0.0 ? sum / weight : -1.0;
}

/// Mean of the red channel across a row, 0..255.
double rowMean( const std::vector< unsigned char >& image, int width, int row )
{
	double sum = 0.0;
	for( int x = 0; x < width; ++x )
		sum += image[ ( static_cast< size_t >( row ) * width + x ) * 4 ];
	return sum / width;
}

/// The period of a signal from its upward zero crossings, with linear
/// interpolation between samples so the answer is not quantised to a row.
/// Returns 0 with fewer than two crossings.
double periodFromCrossings( const std::vector< double >& signal, int& crossings )
{
	double mean = 0.0;
	for( double v : signal )
		mean += v;
	mean /= static_cast< double >( signal.size() );

	std::vector< double > where;
	for( size_t i = 1; i < signal.size(); ++i )
	{
		const double a = signal[ i - 1 ] - mean;
		const double b = signal[ i ] - mean;
		if( a < 0.0 && b >= 0.0 )
			where.push_back( static_cast< double >( i - 1 ) + a / ( a - b ) );
	}
	crossings = static_cast< int >( where.size() );
	if( where.size() < 2 )
		return 0.0;
	return ( where.back() - where.front() ) / static_cast< double >( where.size() - 1 );
}

//---------------------------------------------------------------------------
// --skew
//
// A bar moving at v px/frame, read out over exactly one frame: the top row
// was read a whole frame before the bottom one, so it shows the bar a whole
// frame earlier. The lean, top to bottom, is v -- less the one row's worth
// the last row is read before the timestamp, which is v / H.
//---------------------------------------------------------------------------
int runSkew( int width, int height )
{
	const float speed   = 24.0f;
	const int barWidth  = 8;
	const float x0      = 0.15f * width;
	const int frames    = 8;
	const double expect = speed * ( static_cast< double >( height ) - 1.0 ) / height;

	struct Case
	{
		const char* name;
		float interpolation;
		float direction;
		double sign;
	};
	const Case cases[] = {
		{ "Blend, top down  ", 0.0f, 0.0f, +1.0 },
		{ "Hold,  top down  ", 1.0f, 0.0f, +1.0 },
		{ "Blend, bottom up ", 0.0f, 1.0f, -1.0 },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Session session;
		Readout& p = session.plugin;
		set( p, "Readout Time", controls::ReadoutParam( 1.0f / 60.0f ) );
		set( p, "Exposure", 0.0f );
		set( p, "Interpolation", c.interpolation );
		set( p, "Direction", c.direction );
		set( p, "Global", 0.0f );
		set( p, "Amount", 0.0f );
		set( p, "Trigger", 0.0f );
		set( p, "Mains", 0.0f );

		if( !session.begin( width, height ) )
			return 1;
		for( int frame = 0; frame < frames; ++frame )
			if( !session.render( frame, buildBar( width, height, frame, x0, speed, barWidth ) ) )
				return 1;

		const std::vector< unsigned char > out = session.readBack();
		session.end();

		const double top    = rowCentroid( out, width, 0 );
		const double bottom = rowCentroid( out, width, height - 1 );
		const double lean   = ( bottom - top ) * c.sign;
		const bool ok       = top >= 0.0 && bottom >= 0.0 && std::fabs( lean - expect ) <= 0.1;

		std::printf( "skew %s bar at %.3f (first row) and %.3f (last row): lean %.3f px, expected %.3f  %s\n",
		             c.name, c.sign > 0 ? top : bottom, c.sign > 0 ? bottom : top, lean, expect,
		             ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "skew: a bar moving at v leans by v" : "skew: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --band
//
// A flash pulse lights the rows whose exposure window overlaps it. The band
// is ( E + L ) / T_read of the frame high: E because a row exposed before the
// pulse still catches it, L because the pulse lasts. It is centred on the
// phase, shifted by half the exposure towards the rows read later, and its
// edges ramp over min( E, L ).
//---------------------------------------------------------------------------
int runBand( int width, int height )
{
	const float readout  = 0.040f;
	const float exposure = 0.004f;
	const float length   = 0.008f;
	const float phase    = 0.30f;
	const float grey     = 0.25f;
	const int frames     = 8;

	Session session;
	Readout& p = session.plugin;
	set( p, "Readout Time", controls::ReadoutParam( readout ) );
	set( p, "Exposure", controls::ExposureParam( exposure ) );
	set( p, "Length", controls::FlashLengthParam( length ) );
	set( p, "Phase", phase );
	set( p, "Level", 0.5f );//unity
	set( p, "Colour", 0.0f );
	set( p, "Global", 0.0f );
	set( p, "Amount", 0.0f );
	set( p, "Trigger", 0.0f );
	set( p, "Mains", 0.0f );

	session.fireFrame = frames - 1;
	if( !session.begin( width, height ) )
		return 1;
	const std::vector< unsigned char > flat = buildFlat( width, height, grey );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, flat ) )
			return 1;

	const std::vector< unsigned char > out = session.readBack();
	session.end();

	const double base = grey * 255.0;
	int first = -1, last = -1;
	double peak = 0.0;
	for( int row = 0; row < height; ++row )
	{
		const double excess = rowMean( out, width, row ) - base;
		peak                = std::max( peak, excess );
		if( excess >= 1.0 )
		{
			if( first < 0 )
				first = row;
			last = row;
		}
	}

	const double expectHeight = ( exposure + length ) / readout * height;
	const double expectCentre = ( phase + exposure / ( 2.0 * readout ) ) * height;
	const double gotHeight    = first < 0 ? 0.0 : static_cast< double >( last - first + 1 );
	const double gotCentre    = first < 0 ? -1.0 : ( first + last + 1 ) / 2.0;

	const bool ok = first >= 0 && std::fabs( gotHeight - expectHeight ) <= 2.0
	                && std::fabs( gotCentre - expectCentre ) <= 2.0 && peak >= 150.0;

	std::printf( "band rows %d..%d: height %.1f rows, expected %.1f; centre %.1f, expected %.1f; peak +%.0f/255  %s\n",
	             first, last, gotHeight, expectHeight, gotCentre, expectCentre, peak, ok ? "ok" : "FAILED" );
	std::printf( "%s\n", ok ? "band: a flash bands ( E + L ) / T_read of the frame, where the phase says"
	                        : "band: FAILED" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --flicker
//
// A mains-driven lamp flickers at twice the mains frequency. Against a
// readout of T_read the bands sit T_light / T_read of the frame apart.
//---------------------------------------------------------------------------
int runFlicker( int width, int height )
{
	const float readout = 0.060f;
	const int frames    = 8;

	struct Case
	{
		const char* name;
		float option;
		double hz;
	};
	const Case cases[] = {
		{ "50 Hz", 1.0f, 50.0 },
		{ "60 Hz", 2.0f, 60.0 },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Session session;
		Readout& p = session.plugin;
		set( p, "Readout Time", controls::ReadoutParam( readout ) );
		set( p, "Exposure", 0.0f );
		set( p, "Mains", c.option );
		set( p, "Depth", 0.5f );
		set( p, "Global", 0.0f );
		set( p, "Amount", 0.0f );
		set( p, "Trigger", 0.0f );

		if( !session.begin( width, height ) )
			return 1;
		const std::vector< unsigned char > flat = buildFlat( width, height, 0.5f );
		for( int frame = 0; frame < frames; ++frame )
			if( !session.render( frame, flat ) )
				return 1;

		const std::vector< unsigned char > out = session.readBack();
		session.end();

		std::vector< double > profile( static_cast< size_t >( height ) );
		for( int row = 0; row < height; ++row )
			profile[ static_cast< size_t >( row ) ] = rowMean( out, width, row );

		int crossings       = 0;
		const double period = periodFromCrossings( profile, crossings );
		const double expect = ( 1.0 / ( 2.0 * c.hz ) ) / readout * height;
		const bool ok       = crossings >= 2 && std::fabs( period - expect ) <= 1.0;

		std::printf( "flicker %s: period %.2f rows over %d crossings, expected %.2f  %s\n",
		             c.name, period, crossings, expect, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "flicker: mains bands sit T_light / T_read of the frame apart"
	                                    : "flicker: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --global
//
// Global turns the readout off, and with nothing else on the plugin must
// return the input bit-exactly -- an effect that quietly softens footage it
// is not meant to be touching is an effect that has to be switched off
// between cues. The negative control proves the check can fail: the same
// scene with Global off is NOT the input.
//---------------------------------------------------------------------------
int runGlobal( int width, int height )
{
	const int frames = 8;

	struct Case
	{
		const char* name;
		float global;
		float readout;
		bool expectExact;
	};
	const Case cases[] = {
		{ "Global on,  Readout 20 ms", 1.0f, controls::ReadoutParam( 0.020f ), true },
		{ "Global on,  Readout 60 ms", 1.0f, 1.0f, true },
		{ "Global off, Readout 20 ms", 0.0f, controls::ReadoutParam( 0.020f ), false },
	};

	int failures = 0;
	for( const Case& c : cases )
	{
		Session session;
		Readout& p = session.plugin;
		set( p, "Global", c.global );
		set( p, "Readout Time", c.readout );
		set( p, "Exposure", 0.0f );
		set( p, "Amount", 0.0f );
		set( p, "Rotation", 0.0f );
		set( p, "Audio Drive", 0.0f );
		set( p, "Trigger", 0.0f );
		set( p, "Mains", 0.0f );
		set( p, "Mix", 1.0f );

		if( !session.begin( width, height ) )
			return 1;
		std::vector< unsigned char > card;
		for( int frame = 0; frame < frames; ++frame )
		{
			card = buildCard( width, height, frame );
			if( !session.render( frame, card ) )
				return 1;
		}

		const std::vector< unsigned char > out = session.readBack();
		session.end();

		int worst = 0;
		for( size_t i = 0; i < out.size(); ++i )
			worst = std::max( worst, std::abs( static_cast< int >( out[ i ] ) - static_cast< int >( card[ i ] ) ) );

		const bool ok = c.expectExact ? worst == 0 : worst > 0;
		std::printf( "global %s: worst %3d/255 %s  %s\n", c.name, worst,
		             c.expectExact ? "(must be 0)" : "(must NOT be 0: the negative control)", ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "global: Global on returns the input bit-exactly" : "global: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --jello
//
// A camera shaking sinusoidally at f, read out over T_read: each row sees
// the camera at a different moment, so a straight bar comes out as a wave
// with one cycle every 1/f of readout, which is ( 1 / f ) / T_read of the
// frame. The amplitude is the shake's amplitude, in pixels.
//---------------------------------------------------------------------------
int runJello( int width, int height )
{
	const float readout   = 0.060f;
	const float hz        = 40.0f;
	const float amplitude = 0.02f;//picture heights
	const int frames      = 8;

	Session session;
	Readout& p = session.plugin;
	set( p, "Readout Time", controls::ReadoutParam( readout ) );
	set( p, "Exposure", 0.0f );
	set( p, "Global", 0.0f );
	set( p, "Trigger", 0.0f );
	set( p, "Mains", 0.0f );
	p.SetShakeForTest( hz, amplitude );

	if( !session.begin( width, height ) )
		return 1;
	const std::vector< unsigned char > bar = buildBar( width, height, 0, 0.5f * width - 4.0f, 0.0f, 8 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, bar ) )
			return 1;

	const std::vector< unsigned char > out = session.readBack();
	session.end();

	std::vector< double > centroid( static_cast< size_t >( height ) );
	double lo = 1e9, hi = -1e9;
	for( int row = 0; row < height; ++row )
	{
		const double c                        = rowCentroid( out, width, row );
		centroid[ static_cast< size_t >( row ) ] = c;
		lo                                    = std::min( lo, c );
		hi                                    = std::max( hi, c );
	}

	int crossings         = 0;
	const double period   = periodFromCrossings( centroid, crossings );
	const double expect   = ( 1.0 / hz ) / readout * height;
	const double swing    = hi - lo;
	const double expectPP = 2.0 * amplitude * height;
	const bool ok         = crossings >= 2 && std::fabs( period - expect ) <= 1.0 && std::fabs( swing - expectPP ) <= 1.0;

	std::printf( "jello %.0f Hz over %.0f ms: period %.2f rows over %d crossings, expected %.2f; swing %.2f px, expected %.2f  %s\n",
	             hz, readout * 1000.0, period, crossings, expect, swing, expectPP, ok ? "ok" : "FAILED" );
	std::printf( "%s\n", ok ? "jello: a shake at f wobbles the rows at 1/f / T_read of the frame" : "jello: FAILED" );
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Readout& plugin, int width, int height, int frames, double fps )
{
	Session session;
	//The session owns a plugin of its own; the caller's carries the settings.
	//Copy them across by name so --set applies to the benchmark.
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Readout::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is built once: this measures the plugin, not the card.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int frame = 0; frame < frames; ++frame )
		session.render( warmup + frame, card );
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	session.end();

	const double seconds = std::chrono::duration< double >( end - start ).count();
	return seconds * 1000.0 / static_cast< double >( frames );
}

int runBench( Readout& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	//Two things make this a measurement rather than a number: glFinish on both
	//sides, because GL calls queue and an unsynchronised version times how
	//fast a `for` loop runs; and a warm-up that is thrown away, because the
	//first frames pay for allocating the ring. The upload of the card into
	//the input texture is inside the timing, as it is in a host.
	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( plugin, size.width, size.height, frames, fps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0 );
	}

	std::printf( "\nCost is one capture and one readout pass at picture size; the readout\n"
	             "fetches one ring layer per frame the exposure window touches, so it\n"
	             "grows with Exposure and with Readout Time. Whatever the settings above\n"
	             "were, they are what was measured; run with --set to measure something else.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line. Same format as the rest
// of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"rotest -- render and measure the Readout rolling-shutter effect\n"
		"\n"
		"  --out PATH        render the moving test card through the plugin (default /tmp/readout.png)\n"
		"  --size WxH        raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N        frames to render before reading back (default 40)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --source S        card (default), bar, or flat\n"
		"  --speed V         the bar source's speed, px/frame (default 24)\n"
		"  --fire T          press Fire T seconds into the run\n"
		"  --audio L         feed a synthetic spectrum at overall level L (0..1)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1. Repeatable.\n"
		"  --list            print every parameter, its kind, default and range, then exit\n"
		"  --skew            a bar moving at v px/frame leans by v\n"
		"  --band            a flash bands ( E + L ) / T_read of the frame, where the phase says\n"
		"  --flicker         mains bands sit T_light / T_read of the frame apart\n"
		"  --global          Global on returns the input bit-exactly\n"
		"  --jello           a sinusoidal shake wobbles the rows at 1/f / T_read of the frame\n"
		"  --bench           time ProcessOpenGL at 720p through 4K\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/readout.png";
	std::string scriptPath;
	std::string sourceName = "card";
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	double fps     = 60.0;
	float speed    = 24.0f;
	float audio    = -1.0f;
	double fireAt  = -1.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--speed" && hasNext )
			speed = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--audio" && hasNext )
			audio = std::strtof( argv[ ++i ], nullptr );
		else if( argument == "--fire" && hasNext )
			fireAt = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--skew" || argument == "--band" || argument == "--flicker" || argument == "--global"
		         || argument == "--jello" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( wantList )
	{
		//No GL needed, so it is answered before a context is made -- which also
		//means it works on a machine where creating one fails, and in CI.
		Readout plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low,
			             p.high );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( !checks.empty() )
	{
		int failures = 0;
		for( const std::string& check : checks )
		{
			int result = 0;
			if( check == "--skew" )
				result = runSkew( width, height );
			else if( check == "--band" )
				result = runBand( width, height );
			else if( check == "--flicker" )
				result = runFlicker( width, height );
			else if( check == "--global" )
				result = runGlobal( width, height );
			else if( check == "--jello" )
				result = runJello( width, height );
			failures += result;
			std::printf( "\n" );
		}
		return finish( failures == 0 ? 0 : 1 );
	}

	Session session;
	session.fps   = fps;
	session.audio = audio;
	if( fireAt >= 0.0 )
		session.fireFrame = static_cast< int >( std::lround( fireAt * fps ) );

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantBench )
		return finish( runBench( session.plugin, frames, fps ) );

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		//Resolve the script's parameter names to indices once, up front, and
		//refuse to run on a name that is not a parameter. A misspelled name
		//that silently did nothing would produce a take that looks deliberate
		//and is wrong.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n",
					              entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			if( got < frame.size() )
				break;

			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			if( !session.render( index, frame ) )
				break;

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
		}

		session.end();
		return finish( 0 );
	}

	//A still, at the end of a run of frames. Several frames and not one: the
	//ring has to hold a different picture in every slot the window touches
	//before what comes out is the effect rather than the ring warming up.
	const Source source = sourceFromName( sourceName );
	for( int frame = 0; frame < frames; ++frame )
	{
		std::vector< unsigned char > pixels;
		switch( source )
		{
		case Source::Bar: pixels = buildBar( width, height, frame, 0.15f * width, speed, 8 ); break;
		case Source::Flat: pixels = buildFlat( width, height, 0.5f ); break;
		case Source::Card:
		default: pixels = buildCard( width, height, frame ); break;
		}
		if( !session.render( frame, pixels ) )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();

	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
