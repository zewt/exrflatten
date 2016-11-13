#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <limits.h>

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

// Too fine-grained:
#include <OpenEXR/ImfCRgbaFile.h>
#include <OpenEXR/ImfDeepScanLineInputFile.h>
#include <OpenEXR/ImathBox.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPreviewImage.h>
#include <OpenEXR/ImfAttribute.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/Iex.h>

#include "helpers.h"

using namespace std;
using namespace Imf;
using namespace Imath;
using namespace Iex;

const int NO_OBJECT_ID = 0;

// This currently processes all object IDs at once, which means we need enough memory to hold all
// output buffers at once.  We could make a separate pass for each object ID to reduce memory usage,
// so we only need to hold one at a time.
//
// Not currently supported/tested:
// - data window is untested
// - tiled images
// - volumes (samples with non-zero depth)
// - arbitrary channel mappings, including layers (we assume "R", "G", "B", "A", "Z", "ZBack", "id")
// - separate per-color alpha (RA, GA, BA)
// - (and lots of other stuff, EXR is "too general")


// Given a filename like "abcdef.1234.exr", return "1234".
string getFrameNumberFromFilename(string s)
{
    // abcdef.1234.exr -> abcdef.1234
    s = setExtension(s, "");

    auto pos = s.rfind(".");
    if(pos == string::npos)
        return "";

    string frameString = s.substr(pos+1);
    return frameString;
}


template<typename T>
class FBArray {
public:
    Array2D<T*> data;

    FBArray()
    {
    }

    ~FBArray()
    {
        for(int y = 0; y < data.height(); y++)
        {
            for(int x = 0; x < data.width(); x++)
                delete[] data[y][x];
        }
    }

    void alloc(const Array2D<unsigned int> &sampleCount)
    {
        for(int y = 0; y < data.height(); y++)
        {
            for(int x = 0; x < data.width(); x++)
                data[y][x] = new T[sampleCount[y][x]];
        }
    }

    void AddArrayToFramebuffer(string name, PixelType pt, const Header &header, DeepFrameBuffer &frameBuffer)
    {
        Box2i dataWindow = header.dataWindow();
        int width = dataWindow.max.x - dataWindow.min.x + 1;
        int height = dataWindow.max.y - dataWindow.min.y + 1;
        data.resizeErase(height, width);

        DeepSlice slice(pt,
                    (char *) (&data[0][0] - dataWindow.min.x - dataWindow.min.y * data.width()),
                    sizeof(T *), sizeof(T *) * data.width(), sizeof(T));
        frameBuffer.insert(name, slice);
    }
};

// A simple container for an output EXR containing only RGBA data.
class SimpleImage
{
public:
    struct pixel {
        float rgba[4];
    };
    vector<pixel> data;
    int width, height;
    Header header;

    SimpleImage(int width_, int height_):
        header(width_, height_)
    {
        width = width_;
        height = height_;
        data.resize(width*height);
    }

    void setColor(float r, float g, float b, float a)
    {
        for(int y = 0; y < height; y++)
        {
            for(int x = 0; x < width; x++)
            {
                const int pixelIdx = x + y*width;
                data[pixelIdx].rgba[0] = r;
                data[pixelIdx].rgba[1] = g;
                data[pixelIdx].rgba[2] = b;
                data[pixelIdx].rgba[3] = a;
            }
        }
    }

    void WriteEXR(string filename) const
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
};

struct DeepSample
{
    float rgba[4];
    float zNear, zFar;
    uint32_t objectId;
};

Header ReadEXR(string filename, Array2D<vector<DeepSample>> &samples)
{
    DeepScanLineInputFile file(filename.c_str());

    const Header &header = file.header();
    Box2i dataWindow = header.dataWindow();
    const int width = dataWindow.max.x - dataWindow.min.x + 1;
    const int height = dataWindow.max.y - dataWindow.min.y + 1;

    DeepFrameBuffer frameBuffer;

    Array2D<unsigned int> sampleCount;
    sampleCount.resizeErase(height, width);
    frameBuffer.insertSampleCountSlice(Slice(UINT,
                (char *) (&sampleCount[0][0] - dataWindow.min.x - dataWindow.min.y * width),
                sizeof(unsigned int), sizeof(unsigned int) * width));

    // Set up the channels that we need.  This would be a lot cleaner if we could use a single data
    // structure for all data, but the EXR library doesn't seem to support interleaved data for deep
    // samples.
    FBArray<float> dataR;
    dataR.AddArrayToFramebuffer("R", FLOAT, header, frameBuffer);

    FBArray<float> dataG;
    dataG.AddArrayToFramebuffer("G", FLOAT, header, frameBuffer);

    FBArray<float> dataB;
    dataB.AddArrayToFramebuffer("B", FLOAT, header, frameBuffer);

    FBArray<float> dataA;
    dataA.AddArrayToFramebuffer("A", FLOAT, header, frameBuffer);

    FBArray<uint32_t> dataId;
    dataId.AddArrayToFramebuffer("id", UINT, header, frameBuffer);

    FBArray<float> dataZ;
    dataZ.AddArrayToFramebuffer("Z", FLOAT, header, frameBuffer);

    FBArray<float> dataZB;
    dataZB.AddArrayToFramebuffer("ZBack", FLOAT, header, frameBuffer);

    file.setFrameBuffer(frameBuffer);
    file.readPixelSampleCounts(dataWindow.min.y, dataWindow.max.y);

    // Allocate the channels now that we know the number of samples per pixel.
    dataR.alloc(sampleCount);
    dataG.alloc(sampleCount);
    dataB.alloc(sampleCount);
    dataA.alloc(sampleCount);
    dataId.alloc(sampleCount);
    dataZ.alloc(sampleCount);
    dataZB.alloc(sampleCount);

    // Read the main image data.
    file.readPixels(dataWindow.min.y, dataWindow.max.y);

    samples.resizeErase(height, width);

    for(int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; x++)
        {
            vector<DeepSample> &out = samples[y][x];
            samples[y][x].resize(sampleCount[y][x]);
            for(int s = 0; s < sampleCount[y][x]; ++s)
            {
                out[s].rgba[0] = dataR.data[y][x][s];
                out[s].rgba[1] = dataG.data[y][x][s];
                out[s].rgba[2] = dataB.data[y][x][s];
                out[s].rgba[3] = dataA.data[y][x][s];
                out[s].zNear = dataZ.data[y][x][s];
                out[s].zFar = dataZB.data[y][x][s];
                out[s].objectId = dataId.data[y][x][s];

                // Try to work around bad Arnold default IDs.  If you don't explicitly specify an object ID,
                // Arnold seems to write uninitialized memory or some other random-looking data to it.
                if(out[s].objectId > 1000000)
                    out[s].objectId = NO_OBJECT_ID;
            }
        }
    }
    return file.header();
}

// Given a list of samples on a pixel, return a list of sample indices sorted by depth, furthest
// from the camera first.
void sortSamplesByDepth(vector<DeepSample> &samples)
{
    // Sort samples by depth.
    sort(samples.begin(), samples.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.zNear > rhs.zNear;
    });
}

struct Layer
{
    string name;
    int objectId;
    shared_ptr<SimpleImage> image;

    Layer(string name_, int width, int height)
    {
        name = name_;
        image = make_shared<SimpleImage>(width, height);
    }
};

/*
 * Create a set of images, one per object ID in the input image, which can be combined with
 * additive blending to get the original image.  This is useful because the layers can be
 * edited simply on a per-object basis and composited in a simpler application like Photoshop.
 *
 * Normally, samples are composited with "over" compositing.  The order of samples is important:
 * 
 * r = 0 // current color
 * r = r0*a0 + r*(1-a0); // overlay r0/a0, and reduce everything under it
 * r = r1*a1 + r*(1-a1); // overlay r1/a1, and reduce everything under it
 * r = r2*a2 + r*(1-a2); // overlay r2/a2, and reduce everything under it
 * ...
 * 
 * which iteratively adds each sample on top to the output, and reduces the contribution
 * of everything underneath it in the process.  This is repeated for each RGBA channel.
 * 
 * We want to know the final contribution of each sample at the end, so instead we do:
 * 
 * r[0] = r0*a0         // r*(1-a0) is zero since this is the first layer
 * 
 * r[1]= r1*a1          // overlay r1/a1
 * r[0] *= (1-a1)       // and reduce everything under it
 * 
 * r[2] = r2*a2         // overlay r2/a2
 * r[1] *= (1-a2)       // and reduce everything under it
 * r[0] *= (1-a2)       // and reduce everything under it
 * 
 * This tracks each term separately: instead of applying the r*(1-a0) term accumulatively,
 * we track its effect on each individual sample underneath it.  Adding r[0]+r[1]+r[2] 
 * would be equivalent to the normal compositing formula.
 *
 * Since this is simple addition, the order of samples no longer matters.  We can then
 * combine samples by object ID, and the resulting images can be added together with simple
 * additive blending.
 */
void SeparateIntoAdditiveLayers(vector<Layer> &layers, const Array2D<vector<DeepSample>> &samples)
{
/*    layers.push_back(Layer("test", samples.width(), samples.height()));
    layers.back().objectId = 0;
    auto image = layers.back().image;*/

    map<int,shared_ptr<SimpleImage>> imagesPerObjectId;

    for(int y = 0; y < samples.height(); y++)
    {
        for(int x = 0; x < samples.width(); x++)
        {
            const vector<DeepSample> &samplesForPixel = samples[y][x];

            struct AccumulatedSample {
                AccumulatedSample()
                {
                    for(int i = 0; i < 4; ++i)
                        rgba[i] = 0;
                }
                float rgba[4];
                int objectId;
                float zNear;
            };

            vector<AccumulatedSample> sampleLayers;
            for(const DeepSample &sample: samplesForPixel)
            {
                AccumulatedSample new_sample;
                new_sample.objectId = sample.objectId;
                new_sample.zNear = sample.zNear;
                for(int i = 0; i < 4; ++i)
                    new_sample.rgba[i] = sample.rgba[i];

                // Apply the alpha term to each sample underneath this one.
                float a = sample.rgba[3];
                for(AccumulatedSample &sample: sampleLayers)
                {
                    for(int i = 0; i < 4; ++i)
                        sample.rgba[i] *= 1-a;
                }

                // Add the new sample.
                sampleLayers.push_back(new_sample);
            }

            // Combine samples by object ID, creating a layer for each.
            //
            // We could do this in one pass instead of two, but debugging is easier in two passes.
            const int pixelIdx = x + y*samples.width();
/*            for(const AccumulatedSample &sample: sampleLayers)
            {
                for(int i = 0; i < 4; ++i)
                    image->data[pixelIdx].rgba[i] += sample.rgba[i];
            } */

            for(const AccumulatedSample &sample: sampleLayers)
            {
                int objectId = sample.objectId;
                if(imagesPerObjectId.find(objectId) == imagesPerObjectId.end())
                {
                    layers.push_back(Layer("color", samples.width(), samples.height()));
                    Layer &layer = layers.back();
                    layer.objectId = objectId;
                    imagesPerObjectId[objectId] = layer.image;
                }

                auto image = imagesPerObjectId.at(objectId);
                for(int i = 0; i < 4; ++i)
                    image->data[pixelIdx].rgba[i] += sample.rgba[i];
            }
        }
    }
}

void readObjectIdNames(const Header &header, map<int,string> &objectIdNames)
{
    for(auto it = header.begin(); it != header.end(); ++it)
    {
        auto &attr = it.attribute();
        if(strcmp(attr.typeName(), "string"))
            continue;

        string headerName = it.name();
        if(headerName.substr(0, 9) != "ObjectId/")
            continue;

        string idString = headerName.substr(9);
        int id = atoi(idString.c_str());
        const StringAttribute &value = dynamic_cast<const StringAttribute &>(attr);
        objectIdNames[id] = value.value();
    }
}

void CopyLayerAttributes(const Header &input, Header &output)
{
    for(auto it = input.begin(); it != input.end(); ++it)
    {
        auto &attr = it.attribute();
        string headerName = it.name();
        if(headerName == "channels" ||
            headerName == "chunkCount" ||
            headerName == "compression" ||
            headerName == "lineOrder" ||
            headerName == "type" ||
            headerName == "version")
            continue;

        if(headerName.substr(0, 9) == "ObjectId/")
            continue;

        output.insert(headerName, attr);
    }
}

class FlattenFiles
{
public:
    FlattenFiles(string inputFilename);
    bool flatten(string output);

private:
    string MakeOutputFilename(string output, const Layer &layer, string name);

    string inputFilename;
    map<int,OutputFilenames> objectIdNames;
};

FlattenFiles::FlattenFiles(string inputFilename_)
{
    inputFilename = inputFilename_;
}

// Do simple substitutions on the output filename.
string FlattenFiles::MakeOutputFilename(string output, const Layer &layer, string name)
{
    string outputName = output;

    // <name>: the name of the object ID that we got from the EXR file, or "#100" if we
    // only have a number.
    outputName = subst(outputName, "<name>", name);

    // <layer>: the output layer that we generated.  This is currently always "color".
    outputName = subst(outputName, "<layer>", layer.name);

    // <inputname>: the input filename, with the directory and ".exr" removed.
    string inputName = inputFilename;
    inputName = basename(inputName);
    inputName = setExtension(inputName, "");
    outputName = subst(outputName, "<inputname>", inputName);

    // <frame>: the input filename's frame number, given a "abcdef.1234.exr" filename.
    // It would be nice if there was an EXR attribute contained the frame number.
    outputName = subst(outputName, "<frame>", getFrameNumberFromFilename(inputFilename));

    static bool warned = false;
    if(!warned && outputName == output)
    {
        // If the output filename hasn't changed, there are no substitutions in it, which
        // means we'll write a single file over and over.  That's probably not what was
        // wanted.
        fprintf(stderr, "Warning: output path \"%s\" doesn't contain any substitutions, so only one file will be written.\n", outputName.c_str());
        fprintf(stderr, "Try \"%s\" instead.\n", (outputName + "_<name>.exr").c_str());
        warned = true;
    }

    return outputName;
}

bool FlattenFiles::flatten(string output)
{
    Array2D<vector<DeepSample>> samples;
    Header header;
    try {
        header = ReadEXR(inputFilename, samples);
    }
    catch(const BaseExc &e)
    {
        // We don't include the filename here because OpenEXR's exceptions include the filename.
        // (Unfortunately, the errors are also formatted awkwardly...)
        fprintf(stderr, "%s\n", e.what());
        return false;
    }

/*
    const ChannelList &channels = header.channels();
    for(auto i = channels.begin(); i != channels.end(); ++i)
    {
        const Channel &channel = i.channel();
        printf("... %s: %i\n", i.name(), channel.type);
    }

    set<string> layerNames;
    channels.layers(layerNames);
    for(const string &layerName: layerNames)
    {
        printf("layer: %s", layerName.c_str());
        ChannelList::ConstIterator layerBegin, layerEnd;
        channels.channelsInLayer(layerName, layerBegin, layerEnd);
        for(auto j = layerBegin; j != layerEnd; ++j)
        {
            cout << "\tchannel " << j.name() << endl;
        }
    }
*/

    // Sort all samples by depth.  If we want to support volumes, this is where we'd do the rest
    // of "tidying", splitting samples where they overlap using splitVolumeSample.
    for(int y = 0; y < samples.height(); y++)
    {
        for(int x = 0; x < samples.width(); x++)
        {
            vector<DeepSample> &samplesForPixel = samples[y][x];
            sortSamplesByDepth(samplesForPixel);
        }
    }

    // Separate the image into layers.
    vector<Layer> layers;

    SeparateIntoAdditiveLayers(layers, samples);

    // If this file has names for object IDs, read them.
    readObjectIdNames(header, objectIdNames);

/*    if(!objectIdNames.empty())
    {
        printf("Object ID names:\n");
        for(auto it: objectIdNames)
            printf("Id %i: %s\n", it.first, it.second.c_str());
        printf("\n");
    } */

    // Set the layer with the object ID 0 to "default", unless a name for that ID
    // was specified explicitly.
    if(objectIdNames.find(NO_OBJECT_ID) == objectIdNames.end())
        objectIdNames[NO_OBJECT_ID] = "default";

    // Write the results.
    for(int i = 0; i < layers.size(); ++i)
    {
        auto &layer = layers[i];

        // Copy all image attributes, except for built-in EXR headers that we shouldn't set.
        CopyLayerAttributes(header, layer.image->header);

        string objectIdName;
        if(objectIdNames.find(layer.objectId) != objectIdNames.end())
            objectIdName = objectIdNames.at(layer.objectId);
        else
            objectIdName = ssprintf("#%i", layer.objectId);

        string outputName = MakeOutputFilename(output, layer, objectIdName);

        printf("Writing: %s\n", outputName.c_str());
        try {
            layer.image->WriteEXR(outputName);
        }
        catch(const BaseExc &e)
        {
            fprintf(stderr, "%s\n", e.what());
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    if(argc < 3) {
        fprintf(stderr, "Usage: exrflatten input.exr output\n");
        return 1;
    }

    FlattenFiles flatten(argv[1]);
    if(!flatten.flatten(argv[2]))
        return 1;

    return 0;
}

