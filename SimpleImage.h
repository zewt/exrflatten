#ifndef SimpleImage_h
#define SimpleImage_h

#include <string>
#include <vector>
#include <memory>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImathMatrix.h>

using namespace std;

// A simple container for an output EXR containing only RGBA data.
//
// This can also be used to hold a mask, in which case the data will be
// in A, and R, G, and B will be 1.
class SimpleImage
{
public:
    vector<Imath::V4f> data;
    int width, height;
    Imf::Header header;

    SimpleImage(int width, int height);
    SimpleImage(const SimpleImage &cpy);

    const Imath::V4f &GetRGBA(int x, int y) const { return const_cast<SimpleImage *>(this)->GetRGBA(x, y); }

    Imath::V4f &GetRGBA(int x, int y)
    {
	const int pixelIdx = x + y*width;
	return data[pixelIdx];
    }

    void SetColor(Imath::V4f color);

    // Convert between linear color and sRGB in-place.
    void LinearToSRGB();
    void SRGBToLinear();

    void Premultiply();
    void Unpremultiply();

    // Transform a normal map by a matrix.  The 4th channel (w) will be left unchanged.
    void TransformNormalMap(Imath::M44f matrix);

    class EXRLayersToWrite
    {
    public:
        EXRLayersToWrite(shared_ptr<const SimpleImage> image_): image(image_) { }

        // The source image.
        shared_ptr<const SimpleImage> image; 

        // The layer name to write this as, or blank for no layer.
        string layerName;

        // If false, write RGBA.  Otherwise, write only alpha as a luminance channel (Y).
        bool alphaOnly = false;
    };
    static void WriteImages(string filename, vector<EXRLayersToWrite> layers);

    // Return true if this image is completely transparent.
    bool IsEmpty() const;
};

#endif
