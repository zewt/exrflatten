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

void SimpleImage::ConvertAdditiveLayersToOver(vector<shared_ptr<SimpleImage>> &layers)
{
    for(int lowerLayerIdx = 0; lowerLayerIdx < layers.size(); ++lowerLayerIdx)
    {
	shared_ptr<SimpleImage> lowerLayerColor = layers[lowerLayerIdx];

	for(int y = 0; y < lowerLayerColor->height; y++)
	{
	    for(int x = 0; x < lowerLayerColor->width; x++)
	    {
		float alphaBeforeCompositing = lowerLayerColor->GetPixel(x,y).rgba[3];
		if(alphaBeforeCompositing < 0.0001f)
		    continue;

		// Figure out how much influence this layer actually has.  This is the alpha value, with all of
		// the layers on top composited over which reduce its actual influence.
		float alphaAfterCompositing = lowerLayerColor->GetPixel(x,y).rgba[3];
		for(int upperLayerIdx = lowerLayerIdx+1; upperLayerIdx < layers.size(); ++upperLayerIdx)
		{
		    shared_ptr<const SimpleImage> upperLayerColor = layers[upperLayerIdx];

		    // We added a layer on top of this one, which reduces its alpha when over compositing
		    // is applied.  We want the alpha to stay the same, so the final contribution stays the
		    // same.  Adjust alpha so the final output has the same value it did before adding this
		    // new layer.
		    float upperAlpha = upperLayerColor->GetPixel(x,y).rgba[3];
		    alphaAfterCompositing *= 1-upperAlpha;
		}

		// If the layer doesn't actually have any visibility after the layers above it, avoid
		// division by zero below.  This shouldn't actually happen, since if a layer had no
		// visibility, totalExpectedVisibility should have been zero above.
		if(alphaAfterCompositing < 0.0001f)
		    continue;

		// If we expect to be 0.5 visible and we're actually 0.25 visible, multiply color by
		// 2.0 to restore the expected visibility.  Note that we don't multiply alpha.
		float adjustment = alphaBeforeCompositing / alphaAfterCompositing;

		V4f &lowerColor = lowerLayerColor->GetPixel(x,y).rgba;
		for(int i = 0; i < 3; ++i)
		    lowerColor[i] *= adjustment;
	    }
	}
    }
}
