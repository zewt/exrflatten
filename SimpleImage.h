#ifndef SimpleImage_h
#define SimpleImage_h

#include <string>
#include <vector>
#include <memory>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfHeader.h>

using namespace std;

// A simple container for an output EXR containing only RGBA data.
class SimpleImage
{
public:
    vector<Imath::V4f> data;
    int width, height;
    Imf::Header header;

    SimpleImage(int width, int height);

    const Imath::V4f &GetRGBA(int x, int y) const { return const_cast<SimpleImage *>(this)->GetRGBA(x, y); }

    Imath::V4f &GetRGBA(int x, int y)
    {
	const int pixelIdx = x + y*width;
	return data[pixelIdx];
    }

    void SetColor(Imath::V4f color);

    void WriteEXR(string filename) const;

    // Return true if this image is completely transparent.
    bool IsEmpty() const;
};

#endif
