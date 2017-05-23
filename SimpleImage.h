#ifndef SimpleImage_h
#define SimpleImage_h

#include <string>
#include <vector>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfHeader.h>

using namespace std;

// A simple container for an output EXR containing only RGBA data.
class SimpleImage
{
public:
    struct pixel {
	Imath::V4f rgba;
        float mask = 0;
    };
    vector<pixel> data;
    int width, height;
    Imf::Header header;

    SimpleImage(int width, int height);

    const pixel &GetPixel(int x, int y) const { return const_cast<SimpleImage *>(this)->GetPixel(x, y); }

    pixel &GetPixel(int x, int y)
    {
	const int pixelIdx = x + y*width;
	return data[pixelIdx];
    }

    const Imath::V4f &GetRGBA(int x, int y) const { return const_cast<SimpleImage *>(this)->GetRGBA(x, y); }

    Imath::V4f &GetRGBA(int x, int y)
    {
	return GetPixel(x,y).rgba;
    }

    void SetColor(Imath::V4f color);

    void ApplyMask();

    void WriteEXR(string filename) const;

};

#endif
