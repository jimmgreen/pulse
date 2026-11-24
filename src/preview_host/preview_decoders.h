#pragma once

// Content decoding for the preview host: the ordered decoder table
// (image, svg, metafile, pdf, archive, psd, font, shell preview, shell
// thumbnail, text/hex) and the helpers behind it.

#include "preview_router.h"

namespace pulse::preview {

// Runs the decoder table for one Content request; true when a preview was made.
bool DecodeContent(const DecodeRequest& request, DecodeResult& result);

// Releases thread-local COM factories cached by decoders before CoUninitialize.
void ReleasePreviewThreadCaches() noexcept;

} // namespace pulse::preview
