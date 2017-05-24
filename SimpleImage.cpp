#include "SimpleImage.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfOutputFile.h>

using namespace Imf;
using namespace Imath;

SimpleImage::SimpleImage(int width_, int height_):
    header(width_, height_)
{
    width = width_;
    height = height_;
    data.resize(width*height);
}

void SimpleImage::SetColor(V4f color)
{
    for(int y = 0; y < height; y++)
    {
	for(int x = 0; x < width; x++)
	    GetRGBA(x, y) = color;
    }
}

void SimpleImage::ApplyMask()
{
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
	    GetPixel(x, y).rgba *= GetPixel(x, y).mask;
    }
}

void SimpleImage::WriteEXR(string filename) const
{
    Header headerCopy(header);
    headerCopy.channels().insert("R", Channel(FLOAT));
    headerCopy.channels().insert("G", Channel(FLOAT));
    headerCopy.channels().insert("B", Channel(FLOAT));
    headerCopy.channels().insert("A", Channel(FLOAT));

    FrameBuffer frameBuffer;
    frameBuffer.insert("R", Slice(FLOAT, (char *) &data.data()->rgba[0], sizeof(pixel), sizeof(pixel) * width));
    frameBuffer.insert("G", Slice(FLOAT, (char *) &data.data()->rgba[1], sizeof(pixel), sizeof(pixel) * width));
    frameBuffer.insert("B", Slice(FLOAT, (char *) &data.data()->rgba[2], sizeof(pixel), sizeof(pixel) * width));
    frameBuffer.insert("A", Slice(FLOAT, (char *) &data.data()->rgba[3], sizeof(pixel), sizeof(pixel) * width));

    OutputFile file(filename.c_str(), headerCopy);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(height);
}
