#include "Ring.h"

#include <algorithm>

namespace readout
{
Ring::~Ring()
{
	//Nothing GL can be done here -- a destructor may well run without a current
	//context. DeInitGL is where the release happens; this is only a reminder
	//that it has to.
}

bool Ring::Ensure( GLsizei requestedWidth, GLsizei requestedHeight, int requestedSlots )
{
	reshaped = false;

	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	requestedSlots = std::clamp( requestedSlots, 2, kMaxSlots );

	if( textureID != 0 && width == requestedWidth && height == requestedHeight && slots == requestedSlots )
		return true;

	Destroy();

	glGenTextures( 1, &textureID );
	glBindTexture( GL_TEXTURE_2D_ARRAY, textureID );
	glTexImage3D( GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, requestedWidth, requestedHeight, requestedSlots,
	              0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );

	//Bilinear, because every fetch the readout pass makes is between texels:
	//the shake moves the sample point by a fraction of a pixel, and a sensor
	//row lands between two frames of the ring far more often than on one.
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	//Clamp rather than repeat, although the shader never relies on it: a
	//fetch outside the picture returns nothing rather than the far edge, and
	//the clamp only decides what the bilinear footprint of the last texel
	//row sees.
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );

	//The driver reports an allocation it could not honour here, or not at all.
	if( glGetError() != GL_NO_ERROR )
	{
		Destroy();
		return false;
	}

	glGenFramebuffers( 1, &fboID );

	width  = requestedWidth;
	height = requestedHeight;
	slots  = requestedSlots;

	//Clear every layer. A buffer whose contents are undefined is not "a bit of
	//noise on the first frame", it is whatever texture memory the driver
	//handed back -- and a slot is shown for as many frames as the window is
	//long.
	GLint previousFBO           = 0;
	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFBO );
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	glBindFramebuffer( GL_FRAMEBUFFER, fboID );
	glViewport( 0, 0, width, height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	bool complete = true;
	for( int layer = 0; layer < slots; ++layer )
	{
		glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textureID, 0, layer );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			complete = false;
			break;
		}
		glClear( GL_COLOR_BUFFER_BIT );
	}

	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFBO ) );
	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );

	if( !complete )
	{
		Destroy();
		return false;
	}

	reshaped = true;
	return true;
}

void Ring::BeginCapture( int layer )
{
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &savedFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, fboID );
	glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textureID, 0,
	                           std::clamp( layer, 0, std::max( 0, slots - 1 ) ) );
	glViewport( 0, 0, width, height );
}

void Ring::EndCapture()
{
	//The framebuffer is put back; the viewport deliberately is not, because
	//nothing here knows what the host's was. The plugin captures the host
	//viewport before the first pass and restores it before the last, the way
	//every effect in the fleet has to.
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( savedFBO ) );
}

void Ring::Destroy()
{
	if( fboID != 0 )
	{
		glDeleteFramebuffers( 1, &fboID );
		fboID = 0;
	}
	if( textureID != 0 )
	{
		glDeleteTextures( 1, &textureID );
		textureID = 0;
	}
	width  = 0;
	height = 0;
	slots  = 0;
}

} // namespace readout
