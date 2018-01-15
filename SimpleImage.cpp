#include "SimpleImage.h"
#include "helpers.h"

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
    data.resize(width*height, V4f(0,0,0,0));
}

void SimpleImage::SetColor(V4f color)
{
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
            GetRGBA(x, y) = color;
    }
}

void SimpleImage::WriteEXR(string filename, vector<EXRLayersToWrite> layers)
{
    if(layers.size() == 0)
        throw StringException("Can't write an image with no layers.");

    // Use the first image's headers as a template.
    Header headerCopy(layers[0].image->header);

    FrameBuffer frameBuffer;

    for(const EXRLayersToWrite &layer: layers)
    {
        shared_ptr<const SimpleImage> image = layer.image; 

        // If we have a layer name, output eg. "layerName.R".  Otherwise, output just "R".
        string layerPrefix = "";
        if(!layer.layerName.empty())
        {
            layerPrefix = layer.layerName;
            layerPrefix += ".";
        }

        if(layer.alphaOnly)
        {
            headerCopy.channels().insert(layerPrefix + "Y", Channel(FLOAT));
            frameBuffer.insert(layerPrefix + "Y", Slice(FLOAT, (char *) &(image->data[0].w), sizeof(V4f), sizeof(V4f) * image->width));
        }
        else
        {
            headerCopy.channels().insert(layerPrefix + "R", Channel(FLOAT));
            headerCopy.channels().insert(layerPrefix + "G", Channel(FLOAT));
            headerCopy.channels().insert(layerPrefix + "B", Channel(FLOAT));
            headerCopy.channels().insert(layerPrefix + "A", Channel(FLOAT));

            frameBuffer.insert(layerPrefix + "R", Slice(FLOAT, (char *) &(image->data[0].x), sizeof(V4f), sizeof(V4f) * image->width));
            frameBuffer.insert(layerPrefix + "G", Slice(FLOAT, (char *) &(image->data[0].y), sizeof(V4f), sizeof(V4f) * image->width));
            frameBuffer.insert(layerPrefix + "B", Slice(FLOAT, (char *) &(image->data[0].z), sizeof(V4f), sizeof(V4f) * image->width));
            frameBuffer.insert(layerPrefix + "A", Slice(FLOAT, (char *) &(image->data[0].w), sizeof(V4f), sizeof(V4f) * image->width));
        }
    }

    // Use PIZ.  It's much faster to write than deflate.
    headerCopy.compression() = PIZ_COMPRESSION;

    OutputFile file(filename.c_str(), headerCopy);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(layers[0].image->height);
}

bool SimpleImage::IsEmpty() const
{
    for(int y = 0; y < height; y++)
        for(int x = 0; x < width; x++)
            if(GetRGBA(x, y)[3] > 0.0001)
                return false;

    return true;
}
