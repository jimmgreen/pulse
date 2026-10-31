// tiff_reduce.h - reduced pictures of large TIFFs, decoded in parallel.
// WIC decodes a TIFF on one thread and at full size before anything can be
// scaled (about 2 s for a 100 MP LZW file). Strips and tiles are compressed
// independently, so this decodes them on several threads and box-averages
// each one straight into a picture shrunk by an integer factor; the caller
// then scales that to the exact size.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {

struct TiffReduced {
    uint32_t width = 0, height = 0;                // ceil(source / factor)
    uint32_t source_width = 0, source_height = 0;  // IFD0 image size
    uint32_t factor = 1;
    uint32_t channels = 0;  // 1 = gray, 3 = BGR, 4 = premultiplied BGRA
    uint32_t stride = 0;
    std::vector<uint8_t> pixels;
};

enum class TiffReduceStatus {
    Made,
    Unsupported,  // BigTIFF, Deflate/JPEG, palette/CMYK/YCbCr, planar, float...
    Corrupt,
};

// Reads the bytes [offset, offset + size) of the file; false on a short read.
// Called from several threads at once.
class TiffByteSource {
public:
    virtual ~TiffByteSource() = default;
    virtual uint64_t Size() const = 0;
    virtual bool Read(uint64_t offset, size_t size, uint8_t* out) = 0;
};

// min_edge: the longest side of the result reaches it (factor =
// floor(longest / min_edge)); a factor below 2 is Unsupported, the full
// decode is as cheap then. threads: 0 = hardware threads (at most 8).
TiffReduceStatus ReduceTiff(TiffByteSource& source, uint32_t min_edge, unsigned threads,
                            TiffReduced& out);
TiffReduceStatus ReduceTiffMemory(const uint8_t* data, size_t size, uint32_t min_edge,
                                  unsigned threads, TiffReduced& out);
// False whenever WIC should decode the file instead.
bool EstimateReducedTiffMemory(const std::wstring& path, uint32_t min_edge, uint64_t& bytes);
bool ReduceTiffFile(const std::wstring& path, uint32_t min_edge, TiffReduced& out);

// Exposed for tests: TIFF LZW (MSB-first, early change). Writes at most
// capacity bytes; false on a malformed stream.
bool DecodeTiffLzw(const uint8_t* data, size_t size, uint8_t* out, size_t capacity,
                   size_t& written);

} // namespace pulse::preview
