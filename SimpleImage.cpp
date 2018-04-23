#include "SimpleImage.h"
#include "helpers.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfOutputFile.h>

#include <png.h>
#include <zlib.h>

#include <algorithm>
using namespace std;

using namespace Imf;
using namespace Imath;

SimpleImage::SimpleImage(int width_, int height_):
    header(width_, height_)
{
    width = width_;
    height = height_;
    data.resize(width*height, V4f(0,0,0,0));
}

SimpleImage::SimpleImage(const SimpleImage &cpy)
{
    width = cpy.width;
    height = cpy.height;
    data = cpy.data;
    header = cpy.header;
}

void SimpleImage::SetColor(V4f color)
{
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
            GetRGBA(x, y) = color;
    }
}

void SimpleImage::LinearToSRGB()
{
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; ++x)
        {
            int offset = y*width + x;
            float alpha = data[offset].w;
            for(int c = 0; c < 4; ++c)
            {
                float value = data[offset][c];

                if(c != 3)
                {
                    // Unpremultiply:
                    if(alpha > 0.0001f)
                        value /= alpha;

                    value = ::LinearToSRGB(value);
                }
                else
                    value = 1;

                data[offset][c] = value;
            }
        }
    }
}

void SimpleImage::SRGBToLinear()
{
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; ++x)
        {
            int offset = y*width + x;
            float alpha = data[offset].w;
            for(int c = 0; c < 4; ++c)
            {
                float value = data[offset][c];

                if(c != 3)
                {
                    // Premultiply:
                    value *= alpha;

                    value = ::SRGBToLinear(value);
                }

                data[offset][c] = value;
            }
        }
    }
}

void SimpleImage::TransformNormalMap(M44f matrix)
{
    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            // This is a 3-channel vector map encoded in a 4-channel RGBA image.
            // The alpha channel is unused and should be left unchanged.
            V4f &value = GetRGBA(x, y);

            V3f vec;
            vec.x = value.x;
            vec.y = value.y;
            vec.z = value.z;

            // We're working with normal maps, and Arnold doesn't always output normalized
            // normals due to a bug, so normalize now.
            vec.normalize();

            V3f result;
            matrix.multDirMatrix(vec, result);
            value.x = vec.x;
            value.y = vec.y;
            value.z = vec.z;
        }
    }
}

namespace {
    void WritePNG(string filename, SimpleImage::EXRLayersToWrite layer)
    {
        shared_ptr<const SimpleImage> image = layer.image;

        FILE *f = fopen(filename.c_str(), "wb");
        if(!f)
            throw StringException("Error opening output file.");

        png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        if (!png)
            throw StringException("Error writing output file.");

        png_infop info = png_create_info_struct(png);
        if (!info)
            throw StringException("Error writing output file.");

        if (setjmp(png_jmpbuf(png)))
            throw StringException("Error writing output file.");

        png_init_io(png, f);

        // Output is 8bit depth, RGBA format.
        png_set_IHDR(png, info, image->width, image->height, 8,
            PNG_COLOR_TYPE_RGBA,
            PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT,
            PNG_FILTER_TYPE_DEFAULT
        );
        // XXX compression level for both png and exr
        png_set_compression_level(png, Z_NO_COMPRESSION);
        png_write_info(png, info);

        // Interleave the channels and output the data.
        vector<uint8_t> row(image->width*4, 1);
        for(int y = 0; y < image->height; y++)
        {
            for(int x = 0; x < image->width; ++x)
            {
                int offset = y*image->width + x;
                float alpha = image->data[offset].w;
                for(int c = 0; c < 4; ++c)
                {
                    float value = image->data[offset][c];

                    if(c != 3)
                    {
                        // Unpremultiply:
                        if(alpha > 0.0001f)
                            value /= alpha;

                        value = LinearToSRGB(value);
                    }

                    // 32-bit -> 8-bit:
                    uint8_t output = (uint8_t) min(max(lrintf(value * 255.0f), 0L), 255L);
                    row[x*4+c] = output;
                }
            }
            png_write_row(png, row.data());
        }

        png_write_end(png, NULL);

        fclose(f);
    }
}

void SimpleImage::WriteImages(string filename, vector<EXRLayersToWrite> layers)
{
    if(layers.size() == 0)
        throw StringException("Can't write an image with no layers.");

    if(!stricmp(getExtension(filename).c_str(), "png"))
    {
        if(layers.size() > 1)
            throw StringException("Can't write a PNG with multiple layers");

        WritePNG(filename, layers[0]);
        return;
    }

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
