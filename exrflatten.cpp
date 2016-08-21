#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <limits.h>

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

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

using namespace std;
using namespace Imf;
using namespace Imath;

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

string vssprintf(const char *fmt, va_list va)
{
    // Work around a gcc bug: passing a va_list to vsnprintf alters it. va_list is supposed
    // to be by value.
    va_list vc;
    va_copy(vc, va);

    int iBytes = vsnprintf(NULL, 0, fmt, vc);
    char *pBuf = (char*) alloca(iBytes + 1);
    vsnprintf(pBuf, iBytes + 1, fmt, va);
    return string(pBuf, iBytes);
}

string ssprintf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    return vssprintf(fmt, va);
}

string subst(string s, string from, string to)
{
    int start = 0;
    while(1)
    {
        auto pos = s.find(from, start);
        if(pos == string::npos)
            break;

        string before = s.substr(0, pos);
        string after = s.substr(pos + from.size());
        s = before + to + after;
        start = pos + to.size();
    }

    return s;
}

/*
 * Return the last named component of dir:
 * a/b/c -> c
 * a/b/c/ -> c
 */
string basename(const string &dir)
{
    size_t end = dir.find_last_not_of("/\\");
    if( end == dir.npos )
        return "";

    size_t start = dir.find_last_of("/\\", end);
    if(start == dir.npos)
        start = 0;
    else
        ++start;

    return dir.substr(start, end-start+1);
}

string setExtension(string path, const string &ext)
{
    auto pos = path.rfind('.');
    if(pos != string::npos)
        path = path.substr(0, pos);

    return path + ext;
}

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

// http://www.openexr.com/TechnicalIntroduction.pdf:
void splitVolumeSample(
        float a, float c, // Opacity and color of original sample
        float zf, float zb, // Front and back of original sample
        float z, // Position of split
        float& af, float& cf, // Opacity and color of part closer than z
        float& ab, float& cb) // Opacity and color of part further away than z
{
    // Given a volume sample whose front and back are at depths zf and zb respectively, split the
    // sample at depth z. Return the opacities and colors of the two parts that result from the split.
    //
    // The code below is written to avoid excessive rounding errors when the opacity of the original
    // sample is very small:
    //
    // The straightforward computation of the opacity of either part requires evaluating an expression
    // of the form
    //
    // 1 - pow (1-a, x).
    //
    // However, if a is very small, then 1-a evaluates to 1.0 exactly, and the entire expression
    // evaluates to 0.0.
    //
    // We can avoid this by rewriting the expression as
    //
    // 1 - exp (x * log (1-a)),
    //
    // and replacing the call to log() with a call to the function log1p(), which computes the logarithm
    // of 1+x without attempting to evaluate the expression 1+x when x is very small.
    //
    // Now we have
    //
    // 1 - exp (x * log1p (-a)).
    //
    // However, if a is very small then the call to exp() returns 1.0, and the overall expression still
    // evaluates to 0.0. We can avoid that by replacing the call to exp() with a call to expm1():
    //
    // -expm1 (x * log1p (-a))
    //
    // expm1(x) computes exp(x) - 1 in such a way that the result is accurate even if x is very small.

    assert (zb > zf && z >= zf && z <= zb);
    a = max (0.0f, min (a, 1.0f));
    if (a == 1)
    {
        af = ab = 1;
        cf = cb = c;
    }
    else
    {
        float xf = (z - zf) / (zb - zf);
        float xb = (zb - z) / (zb - zf);
        if (a > numeric_limits<float>::min())
        {
            af = -expm1 (xf * log1p (-a));
            cf = (af / a) * c;
            ab = -expm1 (xb * log1p (-a));
            cb = (ab / a) * c;
        }
        else
        {
            af = a * xf;
            cf = c * xf;
            ab = a * xb;
            cb = c * xb;
        }
    }
}

void mergeOverlappingSamples(
    float a1, float c1, // Opacity and color of first sample
    float a2, float c2, // Opacity and color of second sample
    float &am, float &cm) // Opacity and color of merged sample
{
    // This function merges two perfectly overlapping volume or point samples. Given the color and
    // opacity of two samples, it returns the color and opacity of the merged sample.
    //
    // The code below is written to avoid very large rounding errors when the opacity of one or both
    // samples is very small:
    //
    // * The merged opacity must not be computed as 1 - (1-a1) * (1-a2)./ If a1 and a2 are less than
    // about half a floating-point epsilon, the expressions (1-a1) and (1-a2) evaluate to 1.0 exactly,
    // and the merged opacity becomes 0.0. The error is amplified later in the calculation of the
    // merged color.
    //
    // Changing the calculation of the merged opacity to a1 + a2 - a1*a2 avoids the excessive rounding
    // error.
    //
    // * For small x, the logarithm of 1+x is approximately equal to x, but log(1+x) returns 0 because
    // 1+x evaluates to 1.0 exactly.  This can lead to large errors in the calculation of the merged
    // color if a1 or a2 is very small.
    //
    // The math library function log1p(x) returns the logarithm of 1+x, but without attempting to
    // evaluate the expression 1+x when x is very small.

    a1 = max (0.0f, min (a1, 1.0f));
    a2 = max (0.0f, min (a2, 1.0f));
    am = a1 + a2 - a1 * a2;
    if (a1 == 1 && a2 == 1)
        cm = (c1 + c2) / 2;
    else if (a1 == 1)
        cm = c1;
    else if (a2 == 1)
        cm = c2;
    else
    {
        static const float MAX = numeric_limits<float>::max();
        float u1 = -log1p (-a1);
        float v1 = (u1 < a1 * MAX)? u1 / a1: 1;
        float u2 = -log1p (-a2);
        float v2 = (u2 < a2 * MAX)? u2 / a2: 1;
        float u = u1 + u2;
        float w = (u > 1 || am < u * MAX)? am / u: 1;
        cm = (c1 * v1 + c2 * v2) * w;
    }
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

    void write(string filename) const
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

void ReadEXR(DeepScanLineInputFile &file, Array2D<vector<DeepSample>> &samples)
{
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

void readDeepScanlineFile(string filename, string output)
{
    DeepScanLineInputFile file(filename.c_str());
    const Header &header = file.header();

/*
    const ChannelList &channels = file.header().channels();
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
    Array2D<vector<DeepSample>> samples;
    ReadEXR(file, samples);

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
    map<int,string> objectIdNames;
    readObjectIdNames(header, objectIdNames);

    if(!objectIdNames.empty())
    {
        printf("Object ID names:\n");
        for(auto it: objectIdNames)
            printf("Id %i: %s\n", it.first, it.second.c_str());
        printf("\n");
    }

    // Set the layer with the object ID 0 to "default", unless a name for that ID
    // was specified explicitly.
    if(objectIdNames.find(NO_OBJECT_ID) == objectIdNames.end())
        objectIdNames[NO_OBJECT_ID] = "default";

    // Write the results.
    for(int i = 0; i < layers.size(); ++i)
    {
        auto &layer = layers[i];

        // Copy all image attributes, except for built-in EXR headers that we shouldn't set.
        for(auto it = header.begin(); it != header.end(); ++it)
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

            layer.image->header.insert(headerName, attr);
        }

        // Do simple substitutions on the output filename.
        string outputName = output;
        string objectIdName;
        if(objectIdNames.find(layer.objectId) != objectIdNames.end())
            objectIdName = objectIdNames.at(layer.objectId);
        else
            objectIdName = ssprintf("#%i", layer.objectId);

        // <name>: the name of the object ID that we got from the EXR file, or "#100" if we
        // only have a number.
        outputName = subst(outputName, "<name>", objectIdName);

        // <layer>: the output layer that we generated.  This is currently always "color".
        outputName = subst(outputName, "<layer>", layer.name);

        // <inputname>: the input filename, with the directory and ".exr" removed.
        string inputName = filename;
        inputName = basename(inputName);
        inputName = setExtension(inputName, "");
        outputName = subst(outputName, "<inputname>", inputName);

        // <frame>: the input filename's frame number, given a "abcdef.1234.exr" filename.
        // It would be nice if there was an EXR attribute contained the frame number.
        outputName = subst(outputName, "<frame>", getFrameNumberFromFilename(filename));
            
        printf("Writing: %s\n", outputName.c_str());
        layer.image->write(outputName);
    }
}

int main(int argc, char **argv)
{
    if(argc < 3) {
        fprintf( stderr, "Usage: exrflatten input.exr output\n" );
        return 1;
    }

    readDeepScanlineFile(argv[1], argv[2]);
    return 0;
}

