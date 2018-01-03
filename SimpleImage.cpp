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

void SimpleImage::WriteEXR(string filename, shared_ptr<const SimpleImage> image)
{
    Header headerCopy(image->header);
    headerCopy.channels().insert("R", Channel(FLOAT));
    headerCopy.channels().insert("G", Channel(FLOAT));
    headerCopy.channels().insert("B", Channel(FLOAT));
    headerCopy.channels().insert("A", Channel(FLOAT));

    FrameBuffer frameBuffer;
    frameBuffer.insert("R", Slice(FLOAT, (char *) &(image->data[0].x), sizeof(V4f), sizeof(V4f) * image->width));
    frameBuffer.insert("G", Slice(FLOAT, (char *) &(image->data[0].y), sizeof(V4f), sizeof(V4f) * image->width));
    frameBuffer.insert("B", Slice(FLOAT, (char *) &(image->data[0].z), sizeof(V4f), sizeof(V4f) * image->width));
    frameBuffer.insert("A", Slice(FLOAT, (char *) &(image->data[0].w), sizeof(V4f), sizeof(V4f) * image->width));

    OutputFile file(filename.c_str(), headerCopy);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(image->height);
}

bool SimpleImage::IsEmpty() const
{
    for(int y = 0; y < height; y++)
	for(int x = 0; x < width; x++)
	    if(GetRGBA(x, y)[3] > 0.0001)
		return false;

    return true;
}