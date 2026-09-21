#pragma once

/**
	The two passes.

	1. **capture** -- the host's input, copied into the ring slot the newest
	   frame belongs in. Resolves MaxUV and the half-texel inset once, so the
	   readout pass works on a texture where the picture really does fill the
	   layer.

	2. **readout** -- straight to the host's framebuffer. Every pixel works out
	   which row it is on, when that row was read, what the camera was doing at
	   that moment, and integrates the ring over the row's exposure window. The
	   flash and the mains light are evaluated at the same window. That is the
	   whole plugin; see the comment at the top of `kReadoutShader`.

	Both are complete shaders -- nothing is assembled at run time -- so the
	text `tools/verify.sh` extracts and compiles is the text the plugin runs.
*/
namespace readout::shaders
{

extern const char* const kVertexShader;
extern const char* const kCaptureShader;
extern const char* const kReadoutShader;

} // namespace readout::shaders
