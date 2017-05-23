#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#include "getopt.h"
#include "stroke.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include <unordered_map>

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
#include <OpenEXR/ImfMatrixAttribute.h>
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

// A simple container for an output EXR containing only RGBA data.
class SimpleImage
{
public:
    struct pixel {
        V4f rgba;
        float mask = 0;
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

    const V4f &GetPixel(int x, int y) const
    {
	return const_cast<SimpleImage *>(this)->GetPixel(x, y);
    }

    V4f &GetPixel(int x, int y)
    {
	const int pixelIdx = x + y*width;
	return data[pixelIdx].rgba;
    }

    void setColor(V4f color)
    {
        for(int y = 0; y < height; y++)
        {
            for(int x = 0; x < width; x++)
		GetPixel(x, y) = color;
        }
    }

    /*
    void ApplyMask()
    {
        for(int y = 0; y < height; y++)
        {
            for(int x = 0; x < width; x++)
		GetPixel(x, y) *= data[pixelIdx].mask;
        }
    }
    */
    void WriteEXR(string filename) const
    {
        printf("Writing: %s\n", filename.c_str());

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

// A base class for a deep image channel.  Most of the real work is in TypedDeepImageChannel.
class DeepImageChannel
{
public:
    virtual ~DeepImageChannel()
    {
    }

    virtual void AddToFramebuffer(string name, const Header &header, DeepFrameBuffer &frameBuffer, int channel = 0) = 0;
    virtual void Reorder(int x, int y, const vector<int> &order) = 0;
    virtual void AddSample(int x, int y, int count) = 0;
};

template<class T> PixelType GetEXRPixelType();
template<> PixelType GetEXRPixelType<float>() { return FLOAT; }
template<> PixelType GetEXRPixelType<V3f>() { return FLOAT; }
template<> PixelType GetEXRPixelType<V4f>() { return FLOAT; }
template<> PixelType GetEXRPixelType<uint32_t>() { return UINT; }

// Return the number of bytes from one component of a sample to the next.  For
// vector types, this is from one element of the vector to the next.
template<class T> int GetEXRElementSize();
template<> int GetEXRElementSize<float>() { return sizeof(float); }
template<> int GetEXRElementSize<V3f>() { return sizeof(float); }
template<> int GetEXRElementSize<V4f>() { return sizeof(float); }
template<> int GetEXRElementSize<uint32_t>() { return sizeof(uint32_t); }

template<typename T>
class TypedDeepImageChannel: public DeepImageChannel
{
public:
    TypedDeepImageChannel<T>(int width_, int height_, const Array2D<unsigned int> &sampleCount_):
	width(width_), height(height_),
	sampleCount(sampleCount_)
    {
	data.resizeErase(height, width);

	for(int y = 0; y < data.height(); y++)
	{
	    for(int x = 0; x < data.width(); x++)
	    {
		data[y][x] = new T[sampleCount[y][x]];
	    }
	}
    }

    ~TypedDeepImageChannel()
    {
	for(int y = 0; y < data.height(); y++)
	{
	    for(int x = 0; x < data.width(); x++)
		delete[] data[y][x];
	}
    }

    // Each pixel has multiple samples, and each sample can have multiple elements for vector types.
    // The X stride is the number of bytes from one pixel to the next: sizeof(vector<T>).
    // The sample stride is the number of bytes from one sample to the next: sizeof(T).
    void AddToFramebuffer(string name, const Header &header, DeepFrameBuffer &frameBuffer, int channel = 0)
    {
	// When reading deep files, OpenEXR expects a packed array of pointers for each pixel,
	// pointing to an array of sample values, and it takes a stride value from one sample
	// to the next.  However, there's no way to specify an offset from the start of the
	// sample array so you can read vectors.  You can make it read the first component
	// of a vector, but there's no way to make it skip ahead to read the second.  To work
	// around this, we need to allocate a separate buffer for each channel of a vector,
	// and fill it with pointers to each component.
	//
	// The temporary array is stored in readPointer.  It needs to remain allocated until
	// readPixels is called.
	Array2D<T *> *readArray;
	if(channel == 0)
	{
	    readArray = &data;
	}
	else
	{
	    shared_ptr<Array2D<T *>> readPointer = make_shared<Array2D<T *>>();
	    readPointer->resizeErase(data.height(),data.width());
	    for(int y = 0; y < data.height(); y++)
	    {
		for(int x = 0; x < data.width(); x++)
		{
		    char *c = (char *) data[y][x];
		    c += GetEXRElementSize<T>() * channel;
		    (*readPointer)[y][x] = (T *) c;
		}
	    }

	    readPointers.push_back(readPointer);

	    readArray = readPointer.get();
	}

	Box2i dataWindow = header.dataWindow();
	char *base = (char *) &(*readArray)[-dataWindow.min.y][-dataWindow.min.x];

	PixelType pt = GetEXRPixelType<T>();
	int xStride = sizeof(T*);
	int yStride = sizeof(T*) * data.width();
	int sampleStride = sizeof(T);
	DeepSlice slice(pt, base, xStride, yStride, sampleStride);
	frameBuffer.insert(name, slice);
    }

    // Reorder our data to the given order.  If order is { 2, 1, 0 }, we'll reverse the
    // order.
    void Reorder(int x, int y, const vector<int> &order)
    {
	T *newValues = new T[order.size()];

	for(int i = 0; i < order.size(); ++i)
	    newValues[i] = data[y][x][order[i]];

	delete[] data[y][x];
	data[y][x] = newValues;
    }

    // Add an empty sample to the end of the list for the given pixel.
    void AddSample(int x, int y, int count)
    {
	T *newArray = new T[count];
	memcpy(newArray, data[y][x], sizeof(T) * (count-1));
	delete[] data[y][x];
	data[y][x] = newArray;
    }

    // Get all samples for the given pixel.
    const T *GetSamples(int x, int y) const
    {
	return const_cast<TypedDeepImageChannel<T> *>(this)->GetSamples(x, y);
    }

    T *GetSamples(int x, int y)
    {
	return data[y][x];
    }

    // Get a sample for for the given pixel.
    const T &Get(int x, int y, int sample) const
    {
	return const_cast<TypedDeepImageChannel<T> *>(this)->Get(x, y, sample);
    }

    T &Get(int x, int y, int sample)
    {
	return data[y][x][sample];
    }

    // If sample is -1, return defaultValue.  Otherwise, return the actual sample value.
    T GetWithDefault(int x, int y, int sample, T defaultValue) const
    {
	if(sample == -1)
	    return defaultValue;
	return Get(x, y, sample);
    }

    // Get the last sample for a pixel.  This is useful after calling AddSample to get
    // the sample that was just added.
    T &GetLast(int x, int y)
    {
	int last = sampleCount[y][x]-1;
	return data[y][x][last];
    }

    int width, height;
    Array2D<T *> data;
    vector<shared_ptr<Array2D<T *>>> readPointers;

    // This is a reference to DeepImage::sampleCount, which is shared by all channels.
    const Array2D<unsigned int> &sampleCount;
};

class DeepImage
{
public:
    static shared_ptr<DeepImage> ReadEXR(string filename);

    DeepImage(int width_, int height_)
    {
	width = width_;
	height = height_;
	sampleCount.resizeErase(height, width);
    }

    // Add a channel, and add it to the DeepFrameBuffer to be read.
    //
    // channelName is the name to assign this channel.  This usually corresponds with an EXR channel
    // or layer name, but this isn't required.
    //
    // For vector types, exrChannels is a list of the EXR channels for each component, eg. { "P.X", "P.Y", "P.Z" }.
    template<typename T>
    shared_ptr<TypedDeepImageChannel<T>> AddChannelToFramebuffer(string channelName, vector<string> exrChannels, DeepFrameBuffer &frameBuffer)
    {
	shared_ptr<TypedDeepImageChannel<T>> channel = AddChannel<T>(channelName);
	int idx = 0;
	for(string exrChannel: exrChannels)
	    channel->AddToFramebuffer(exrChannel, header, frameBuffer, idx++);
	return channel;
    }

    // Add a channel with the given name.
    template<typename T>
    shared_ptr<TypedDeepImageChannel<T>> AddChannel(string name)
    {
	auto channel = make_shared<TypedDeepImageChannel<T>>(width, height, sampleCount);
	channels[name] = channel;
	return channel;
    }

    template<typename T>
    shared_ptr<const TypedDeepImageChannel<T>> GetChannel(string name) const
    {
	return const_cast<DeepImage *>(this)->GetChannel<T>(name);
    }

    template<typename T>
    shared_ptr<TypedDeepImageChannel<T>> GetChannel(string name)
    {
	auto it = channels.find(name);
	if(it == channels.end())
	    return nullptr;

	shared_ptr<DeepImageChannel> result = it->second;
	auto typedResult = dynamic_pointer_cast<TypedDeepImageChannel<T>>(result);
	return typedResult;
    }

    void AddSample(int x, int y)
    {
	sampleCount[y][x]++;
	for(auto it: channels)
	{
	    shared_ptr<DeepImageChannel> channel = it.second;
	    channel->AddSample(x, y, sampleCount[y][x]);
	}
    }

    // Sort samples based on the depth of each pixel, furthest from the camera first.
    void SortSamplesByDepth()
    {
	const auto Z = GetChannel<float>("Z");

	vector<int> order;
	for(int y = 0; y < height; y++)
	{
	    for(int x = 0; x < width; x++)
	    {
		order.resize(sampleCount[y][x]);
		for(int sample = 0; sample < order.size(); ++sample)
		    order[sample] = sample;

		// Sort samples by depth.
		const float *depth = Z->GetSamples(x, y);
		sort(order.begin(), order.end(), [&](int lhs, int rhs)
		{
		    float lhsZNear = depth[lhs];
		    float rhsZNear = depth[rhs];
		    return lhsZNear > rhsZNear;
		});

		for(auto it: channels)
		{
		    shared_ptr<DeepImageChannel> &channel = it.second;
		    channel->Reorder(x, y, order);
		}
	    }
	}
    }

    int NumSamples(int x, int y) const
    {
	return sampleCount[y][x];
    }


    vector<float> GetSampleVisibility(int x, int y) const
    {
	vector<float> result;

	auto rgba = GetChannel<V4f>("rgba");

	for(int s = 0; s < sampleCount[y][x]; ++s)
	{
	    float alpha = rgba->Get(x, y, s)[3];

	    // Apply the alpha term to each sample underneath this one.
	    for(float &sampleAlpha: result)
		sampleAlpha *= 1-alpha;

	    result.push_back(alpha);
	}
	return result;
    }

    int width = 0, height = 0;
    Header header;
    map<string, shared_ptr<DeepImageChannel>> channels;
    Array2D<unsigned int> sampleCount;
};

shared_ptr<DeepImage> DeepImage::ReadEXR(string filename)
{
    DeepScanLineInputFile file(filename.c_str());

    Box2i dataWindow = file.header().dataWindow();
    const int width = dataWindow.max.x - dataWindow.min.x + 1;
    const int height = dataWindow.max.y - dataWindow.min.y + 1;

    shared_ptr<DeepImage> image = make_shared<DeepImage>(width, height);
    image->header = file.header();

    // Read the sample counts.  This needs to be done before we start adding channels, so we know
    // how much space to allocate.
    DeepFrameBuffer frameBuffer;
    frameBuffer.insertSampleCountSlice(Slice(UINT,
	(char *) (&image->sampleCount[0][0] - dataWindow.min.x - dataWindow.min.y * width),
	sizeof(unsigned int), sizeof(unsigned int) * width));
    file.setFrameBuffer(frameBuffer);
    file.readPixelSampleCounts(dataWindow.min.y, dataWindow.max.y);

    // Set up the channels we're interested in.
    image->AddChannelToFramebuffer<V4f>("rgba", {"R", "G", "B", "A"}, frameBuffer);
    image->AddChannelToFramebuffer<uint32_t>("id", {"id"}, frameBuffer);
    image->AddChannelToFramebuffer<float>("Z", { "Z" }, frameBuffer);
    image->AddChannelToFramebuffer<float>("ZBack", {"ZBack"}, frameBuffer);
    image->AddChannelToFramebuffer<float>("mask", { "mask" }, frameBuffer);
    image->AddChannelToFramebuffer<V3f>("P", { "P.X", "P.Y", "P.Z" }, frameBuffer);
    image->AddChannelToFramebuffer<V3f>("N", { "N.X", "N.Y", "N.Z" }, frameBuffer);

    // Read the main image data.
    file.setFrameBuffer(frameBuffer);
    file.readPixelSampleCounts(dataWindow.min.y, dataWindow.max.y);
    file.readPixels(dataWindow.min.y, dataWindow.max.y);

    // Try to work around bad Arnold default IDs.  If you don't explicitly specify an object ID,
    // Arnold seems to write uninitialized memory or some other random-looking data to it.
    auto id = image->GetChannel<uint32_t>("id");
    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            for(int s = 0; s < image->sampleCount[y][x]; ++s)
            {
                if(id->Get(x,y,s) > 1000000)
		    id->Get(x,y,s) = NO_OBJECT_ID;
            }
        }
    }

    return image;
}

struct Layer
{
    string name;
    int objectId = -1;
    string layerName;
    shared_ptr<SimpleImage> image;

    Layer(string name_, int width, int height)
    {
        name = name_;
        image = make_shared<SimpleImage>(width, height);
    }
};

struct OutputFilenames
{
    string main;
    string mask;
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
void SeparateIntoAdditiveLayers(vector<Layer> &layers, shared_ptr<const DeepImage> image, const unordered_map<int,OutputFilenames> &objectIdNames)
{
/*    layers.push_back(Layer("test", samples.width(), samples.height()));
    layers.back().objectId = 0;
    auto image = layers.back().image;*/

    auto getLayerName = [objectIdNames](int objectId) {
        auto it = objectIdNames.find(objectId);
        if(it == objectIdNames.end())
            return OutputFilenames();
        return it->second;
    };

    unordered_map<string,shared_ptr<SimpleImage>> imagesPerObjectIdName;

    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");
    auto Z = image->GetChannel<float>("Z");
    auto mask = image->GetChannel<float>("mask");

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
            struct AccumulatedSample {
                AccumulatedSample()
                {
                    for(int i = 0; i < 4; ++i)
                        rgba[i] = 0;
                }
		V4f rgba;
		V4f masked_rgba;
		float mask;
                int objectId;
                float zNear;
            };

            vector<AccumulatedSample> sampleLayers;
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {
                AccumulatedSample new_sample;
                new_sample.objectId = id->Get(x, y, s);
                new_sample.zNear = Z->Get(x, y, s);

		// If we have a mask, we'll multiply rgba by it to get a masked version.  However,
		// the mask will be premultiplied by rgba[3] too.  To get the real mask value, we
		// need to un-premultiply it.
		float alpha = rgba->Get(x, y, s)[3];
		float maskValue = alpha > 0.0001f? (mask->Get(x, y, s) / alpha):0;

		new_sample.rgba = new_sample.masked_rgba = rgba->Get(x,y,s);
		new_sample.masked_rgba = new_sample.rgba * maskValue;

                // new_sample.mask = mask->Get(x, y, s);

                // Apply the alpha term to each sample underneath this one.
                for(AccumulatedSample &sample: sampleLayers)
                {
		    sample.rgba *= 1-alpha;
		    sample.masked_rgba *= 1-alpha;
                    // sample.mask *= 1-alpha;
                }

                // Add the new sample.
                sampleLayers.push_back(new_sample);
            }

            // Combine samples by object ID, creating a layer for each.
            //
            // We could do this in one pass instead of two, but debugging is easier in two passes.
            const int pixelIdx = x + y*image->width;
/*            for(const AccumulatedSample &sample: sampleLayers)
            {
                for(int i = 0; i < 4; ++i)
                    image->data[pixelIdx].rgba[i] += sample.rgba[i];
            } */

            auto getLayer = [&imagesPerObjectIdName, &layers](string layerName, int objectId, int width, int height)
            {
                auto it = imagesPerObjectIdName.find(layerName);
                if(it != imagesPerObjectIdName.end())
                    return it->second;

                layers.push_back(Layer("color", width, height));
                Layer &layer = layers.back();
                layer.objectId = objectId;
                layer.layerName = layerName;
                imagesPerObjectIdName[layerName] = layer.image;
                return layer.image;
            };

            for(const AccumulatedSample &sample: sampleLayers)
            {
                int objectId = sample.objectId;

                string layerName = getLayerName(objectId).main;
                if(layerName != "")
                {
                    auto out = getLayer(layerName, objectId, image->width, image->height);
                    for(int i = 0; i < 4; ++i)
			out->data[pixelIdx].rgba[i] += sample.rgba[i];
                }

                string maskName = getLayerName(objectId).mask;
                if(maskName != "")
                {
                    auto out = getLayer(maskName, objectId, image->width, image->height);
                    for(int i = 0; i < 4; ++i)
			out->data[pixelIdx].rgba[i] += sample.masked_rgba[i];
                }
                //                image->data[pixelIdx].mask += sample.mask;
            }
        }
    }
}

// Flatten the color channels of a deep EXR to a simple flat layer.
shared_ptr<SimpleImage> CollapseEXR(shared_ptr<const DeepImage> image, set<int> objectIds = {})
{
    shared_ptr<SimpleImage> result = make_shared<SimpleImage>(image->width, image->height);

    auto rgba = image->GetChannel<V4f>("rgba");
    auto Z = image->GetChannel<float>("Z");
    auto id = image->GetChannel<uint32_t>("id");

    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    V4f &out = result->GetPixel(x, y);
	    out = V4f(0,0,0,0);

	    int samples = image->NumSamples(x,y);
	    for(int s = 0; s < samples; ++s)
	    {
		bool IncludeLayer = objectIds.empty() || objectIds.find(id->Get(x,y,s)) != objectIds.end();

		V4f color = rgba->Get(x,y,s);
		float alpha = color[3];
		for(int channel = 0; channel < 4; ++channel)
		{
		    if(IncludeLayer)
			out[channel] = color[channel] + out[channel]*(1-alpha);
		    else
			out[channel] =                  out[channel]*(1-alpha);
		}
	    }
	}
    }

    return result;
}

struct StrokeConfig
{
    int objectId = 0;
    float radius = 1.0f;
    float pushTowardsCamera = 1.0f;
    V3f strokeColor = {0,0,0};
};

void AddOutlines(const StrokeConfig &config, shared_ptr<DeepImage> image)
{
    // Flatten the image.  We'll use this as the mask to create the stroke.
    shared_ptr<SimpleImage> mask = CollapseEXR(image, { config.objectId });

    // Find closest sample (for our object ID) to the camera for each point.
    Array2D<int> NearestSample;
    NearestSample.resizeErase(image->height, image->width);

    auto rgba = image->GetChannel<V4f>("rgba");
    auto id = image->GetChannel<uint32_t>("id");
    auto ZBack = image->GetChannel<float>("ZBack");
    auto Z = image->GetChannel<float>("Z");

    for(int y = 0; y < image->height; y++)
    {
        for(int x = 0; x < image->width; x++)
        {
	    int &nearest = NearestSample[y][x];
	    nearest = -1;

	    for(int s = 0; s < image->NumSamples(x,y); ++s)
            {
		if(id->Get(x,y,s) != config.objectId)
		    continue;

		if(nearest != -1)
		{
		    if(Z->Get(x,y,s) > Z->Get(x,y,nearest))
			continue;
		}

		nearest = s;
	    }
	}
    }

    // Calculate a stroke for the flattened image, and insert the stroke as deep samples, so
    // it'll get composited at the correct depth, allowing it to be obscured.
    Stroke::CalculateDistance(mask->width, mask->height,
    [&](int x, int y) {
	float result = mask->GetPixel(x, y)[3];
	result = max(0.0f, result);
	result = min(1.0f, result);

	// Skip this line for an inner stroke instead of an outer stroke:
	result = 1.0f - result;
	
	return result;
    }, [&](int x, int y, int sx, int sy, float distance) {
	float alpha = Stroke::DistanceAndRadiusToAlpha(distance, config.radius);

	// Don't add an empty sample.
	if(alpha <= 0.00001f)
	    return;

	// sx/sy might be out of bounds.  This normally only happens if the layer is completely
	// empty and alpha will be 0 so we won't get here, but check to be safe.
	if(sx < 0 || sy < 0 || sx >= NearestSample.width() || sy >= NearestSample.height())
	    return;

	// SourceSample is the nearest visible pixel to this stroke, which we treat as the
	// "source" of the stroke.  StrokeSample is the sample underneath the stroke itself,
	// if any.
	int SourceSample = NearestSample[sy][sx];
	int StrokeSample = NearestSample[y][x];

	// For samples that lie outside the mask, StrokeSample.zNear won't be set, and we'll
	// use the Z distance from the source sample.  For samples that lie within the mask,
	// eg. because there's antialiasing, use whichever is nearer, the sample under the stroke
	// or the sample the stroke came from.  In this case, the sample under the stroke might
	// be closer to the camera than the source sample, so if we don't do this the stroke will
	// end up being behind the shape.
	//
	// Note that either of these may not actually have a sample, in which case the index will
	// be -1 and we'll use the default.
	float zDistance = min(Z->GetWithDefault(sx, sy, SourceSample, 10000000),
	                      Z->GetWithDefault(x, y, StrokeSample, 10000000));

	// Bias the distance closer to the camera.  We need to subtract at least a small amount to
	// make sure the stroke is on top of the source shape.  Subtracting more helps avoid aliasing
	// where two stroked objects are overlapping, but too much will cause strokes to be on top
	// of objects they shouldn't.
	zDistance -= config.pushTowardsCamera;
	// zDistance = 0;

	/*
	 * An outer stroke is normally blended underneath the shape, and only antialiased on
	 * the outer edge of the stroke.  The inner edge where the stroke meets the shape isn't
	 * antialiased.  Instead, the antialiasing of the shape on top of it is what gives the
	 * smooth blending from the stroke to the shape.
	 *
	 * However, we want to put the stroke over the shape, not underneath it, so it can go over
	 * other stroked objects.  Deal with this by mixing the existing color over the stroke color.
	 */
	V4f strokeColor = V4f(config.strokeColor[0], config.strokeColor[1], config.strokeColor[2], 1) * alpha;
	V4f topColor = mask->GetPixel(x, y);
	V4f mixedColor = topColor + strokeColor * (1-topColor[3]);

	// Add a sample for the stroke.
	image->AddSample(x, y);

	rgba->GetLast(x,y) = mixedColor;
	Z->GetLast(x,y) = zDistance;
	ZBack->GetLast(x,y) = zDistance;
	id->GetLast(x,y) = config.objectId;
    });
}

void readObjectIdNames(const Header &header, unordered_map<int,OutputFilenames> &objectIdNames)
{
    for(auto it = header.begin(); it != header.end(); ++it)
    {
        auto &attr = it.attribute();
        if(strcmp(attr.typeName(), "string"))
            continue;

        string headerName = it.name();
        if(headerName.substr(0, 9) == "ObjectId/")
        {
            string idString = headerName.substr(9);
            int id = atoi(idString.c_str());
            const StringAttribute &value = dynamic_cast<const StringAttribute &>(attr);
            objectIdNames[id].main = value.value();
        }
        else if(headerName.substr(0, 5) == "Mask/")
        {
            string idString = headerName.substr(5);
            int id = atoi(idString.c_str());

            const StringAttribute &value = dynamic_cast<const StringAttribute &>(attr);
            objectIdNames[id].mask = value.value();
        }
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

struct Config
{
    string inputFilename;
    string outputPattern;

    vector<StrokeConfig> strokes;

    // A list of (dst, src) pairs to combine layers before writing them.
    vector<pair<int,int>> combines;
};

namespace FlattenFiles
{
    bool flatten(const Config &config);
    string GetFrameNumberFromFilename(string s);
    string MakeOutputFilename(const Config &config, string output, const Layer &layer, string name);
};


// Given a filename like "abcdef.1234.exr", return "1234".
string FlattenFiles::GetFrameNumberFromFilename(string s)
{
    // abcdef.1234.exr -> abcdef.1234
    s = setExtension(s, "");

    auto pos = s.rfind(".");
    if(pos == string::npos)
	return "";

    string frameString = s.substr(pos+1);
    return frameString;
}

// Do simple substitutions on the output filename.
string FlattenFiles::MakeOutputFilename(const Config &config, string output, const Layer &layer, string name)
{
    string outputName = output;

    // <name>: the name of the object ID that we got from the EXR file, or "#100" if we
    // only have a number.
    outputName = subst(outputName, "<name>", name);

    // <layer>: the output layer that we generated.  This is currently always "color".
    outputName = subst(outputName, "<layer>", layer.name);

    // <inputname>: the input filename, with the directory and ".exr" removed.
    string inputName = config.inputFilename;
    inputName = basename(inputName);
    inputName = setExtension(inputName, "");
    outputName = subst(outputName, "<inputname>", inputName);

    // <frame>: the input filename's frame number, given a "abcdef.1234.exr" filename.
    // It would be nice if there was an EXR attribute contained the frame number.
    outputName = subst(outputName, "<frame>", GetFrameNumberFromFilename(config.inputFilename));

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

// Change all samples with an object ID of fromObjectId to intoObjectId.
void CombineLayer(shared_ptr<DeepImage> image, int fromObjectId, int intoObjectId)
{
    auto id = image->GetChannel<uint32_t>("id");
    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    for(int s = 0; s < image->NumSamples(x, y); ++s)
	    {
		uint32_t &thisId = id->Get(x,y,s);
		if(thisId == fromObjectId)
		    thisId = intoObjectId;
	    }
	}
    }
}

bool FlattenFiles::flatten(const Config &config)
{
    unordered_map<int,OutputFilenames> objectIdNames;

    shared_ptr<DeepImage> image;
    try {
	image = DeepImage::ReadEXR(config.inputFilename);
    }
    catch(const BaseExc &e)
    {
        // We don't include the filename here because OpenEXR's exceptions include the filename.
        // (Unfortunately, the errors are also formatted awkwardly...)
        fprintf(stderr, "%s\n", e.what());
        return false;
    }
    Header header = image->header;

    //const ChannelList &channels = header.channels();
    //for(auto i = channels.begin(); i != channels.end(); ++i)
    //    printf("Channel %s has type %i\n", i.name(), i.channel().type);

#if 0
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
#endif

    // Sort all samples by depth.  If we want to support volumes, this is where we'd do the rest
    // of "tidying", splitting samples where they overlap using splitVolumeSample.
    image->SortSamplesByDepth();

    // If this file has names for object IDs, read them.
    readObjectIdNames(header, objectIdNames);

    // Set the layer with the object ID 0 to "default", unless a name for that ID
    // was specified explicitly.
    if(objectIdNames.find(NO_OBJECT_ID) == objectIdNames.end())
	objectIdNames[NO_OBJECT_ID].main = "default";

    
/*    if(!objectIdNames.empty())
    {
        printf("Object ID names:\n");
        for(auto it: objectIdNames)
            printf("Id %i: %s\n", it.first, it.second.c_str());
        printf("\n");
    } */

    // Apply strokes to layers.
    for(auto stroke: config.strokes)
	AddOutlines(stroke, image);

    // If we stroked any objects, re-sort samples, since new samples may have been added.
    if(!config.strokes.empty())
	image->SortSamplesByDepth();

    // Combine layers.  This just changes the object IDs of samples, so we don't need to re-sort.
    for(auto combine: config.combines)
	CombineLayer(image, combine.second, combine.first);

    // Separate the image into layers.
    vector<Layer> layers;
    SeparateIntoAdditiveLayers(layers, image, objectIdNames);

    shared_ptr<SimpleImage> flat = CollapseEXR(image);
    layers.push_back(Layer("color", image->width, image->height));
    layers.back().layerName = "main";
    layers.back().image = flat;

    // Write the results.
    for(int i = 0; i < layers.size(); ++i)
    {
        auto &layer = layers[i];

        // Copy all image attributes, except for built-in EXR headers that we shouldn't set.
        CopyLayerAttributes(header, layer.image->header);

        string outputName = MakeOutputFilename(config, config.outputPattern, layer, layer.layerName);

        try {
            layer.image->WriteEXR(outputName);
        }
        catch(const BaseExc &e)
        {
            fprintf(stderr, "%s\n", e.what());
            return false;
        }
        /*
        if(filenames.mask != "")
        {
            // layer.image->ApplyMask();

            string outputName = MakeOutputFilename(config.outputPattern, layer, filenames.mask);
            try {
                layer.image->WriteEXR(outputName);
            }
            catch(const BaseExc &e)
            {
                fprintf(stderr, "%s\n", e.what());
                return false;
            }
        }
        */
    }
    return true;
}

int main(int argc, char **argv)
{
    option opts[] = {
	{"stroke-radius", required_argument, NULL, 0},
	{"stroke", required_argument, NULL, 0},
	{"combine", required_argument, NULL, 0},
	{0},
    };

    Config config;
    float strokeRadius = 1.0f;
    V3f strokeColor(0,0,0);
    while(1)
    {
	    int index = -1;
	    int c = getopt_long(argc, argv, "", opts, &index);
	    if( c == -1 )
		    break;
	    switch( c )
	    {
	    case 0:
	    {
		    string opt = opts[index].name;
		    if(opt == "stroke")
		    {
			config.strokes.emplace_back();
			config.strokes.back().objectId = atoi(optarg);
			config.strokes.back().radius = strokeRadius;
			config.strokes.back().strokeColor = strokeColor;
		    }
		    else if(opt == "stroke-radius")
		    {
			strokeRadius = (float) atof(optarg);
		    }
		    else if(opt == "combine")
		    {
			const char *split = strchr(optarg, ',');
			if(split == NULL)
			{
			    printf("Invalid --combine (ignored)\n");
			    break;
			}

			int dst = atoi(optarg);
			int src = atoi(split+1);
			config.combines.push_back(make_pair(dst, src));
		    }
	    }
	    }
    }

    if(argc < optind + 1) {
	    fprintf(stderr, "Usage: exrflatten [--combine src,dst ...] input.exr output\n");
	    return 1;
    }

    config.inputFilename = argv[optind];
    config.outputPattern = argv[optind+1];

    if(!FlattenFiles::flatten(config))
        return 1;

//    char buf[1024];
//    fgets(buf, 1000, stdin);
    return 0;
}

