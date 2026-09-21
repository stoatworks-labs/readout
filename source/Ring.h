#pragma once

#include <FFGLSDK.h>

namespace readout
{
/// The most frames the ring will hold. Sixteen full pictures is a quarter of
/// a gigabyte at 4K, and it covers a 60 ms readout plus a 40 ms exposure at
/// any frame rate up to about 140 fps; past that the oldest rows simply read
/// the oldest frame there is.
constexpr int kMaxSlots = 16;

/**
	The last N input frames, as the layers of one array texture.

	The readout pass needs every recent frame at once: a single row's exposure
	window can straddle two or three of them, and the whole picture spans up
	to (readout + exposure) worth. Sixteen separate textures would be sixteen
	sampler bindings, and GLSL 4.10 indexes a sampler array only with a
	dynamically uniform expression -- which a per-pixel frame age is not. One
	`GL_TEXTURE_2D_ARRAY` with a layer per slot is a single sampler that any
	integer can index, so the shader can walk the window per pixel.

	The frame of age k is at layer `( head - k + slots ) % slots`, and the pass
	that captures the input draws into layer `head` through an FBO whose
	colour attachment is re-pointed at that layer each frame.

	Three lessons from the fleet's PassBuffer carried over: reallocate only
	when the shape changes (`Ensure` runs every frame and is a no-op in nearly
	all of them), clear on allocation (a slot is shown to the operator for as
	long as it is in the window, so undefined texture memory would sit on the
	programme feed rather than flicker for a frame), and free the texture
	yourself -- there is no SDK object underneath this one to forget to.
*/
class Ring
{
public:
	~Ring();

	/// Allocate at this size and depth, reusing the existing texture if it
	/// already matches. Returns false when the driver would not give us the
	/// memory. A reshape empties the ring: the layer indexing is modulo the
	/// depth, so a different depth does not shuffle the contents, it files
	/// every one of them under the wrong age.
	bool Ensure( GLsizei width, GLsizei height, int slots );

	/// Bind the FBO to draw into the slot the newest frame belongs in, and
	/// set the viewport to the ring's size. Pair with `EndCapture`.
	void BeginCapture( int layer );
	void EndCapture();

	GLuint TextureID() const
	{
		return textureID;
	}

	GLsizei Width() const
	{
		return width;
	}

	GLsizei Height() const
	{
		return height;
	}

	int Slots() const
	{
		return slots;
	}

	/// True when the last `Ensure` reshaped the ring, so the caller can reset
	/// its head and fill count. Cleared by the next `Ensure`.
	bool WasReshaped() const
	{
		return reshaped;
	}

	void Destroy();

private:
	GLuint textureID = 0;
	GLuint fboID     = 0;
	GLsizei width    = 0;
	GLsizei height   = 0;
	int slots        = 0;
	bool reshaped    = false;
	GLint savedFBO   = 0;
};

} // namespace readout
