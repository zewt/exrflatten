#include "DeepImage.h"
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImathBox.h>
#include <OpenEXR/ImfDeepScanLineInputFile.h>
#include <OpenEXR/ImfInputFile.h>

#include <algorithm>
#include <assert.h>

#include "helpers.h"

using namespace std;
using namespace Imf;
using namespace Imath;

// Instantiate TypedDeepImageChannel templates.
template class TypedDeepImageChannel<uint32_t>;  
template class TypedDeepImageChannel<float>;  
template class TypedDeepImageChannel<V3f>;  
template class TypedDeepImageChannel<V4f>;  

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

DeepImageChannel::DeepImageChannel(int width_, int height_, const Array2D<unsigned int> &sampleCount_):
    width(width_), height(height_),
    sampleCount(sampleCount_)
{
}

template<typename T>
TypedDeepImageChannel<T>::TypedDeepImageChannel(int width_, int height_, const Array2D<unsigned int> &sampleCount_):
    DeepImageChannel(width_, height_, sampleCount_)
{
    defaultValue = T();
    data.resizeErase(height, width);

    for(int y = 0; y < data.height(); y++)
    {
	for(int x = 0; x < data.width(); x++)
	    data[y][x] = new T[sampleCount[y][x]];
    }
}

template<typename T>
TypedDeepImageChannel<T> *TypedDeepImageChannel<T>::Clone() const
{
    auto result = new TypedDeepImageChannel<T>(width, height, sampleCount);

    for(int y = 0; y < data.height(); y++)
    {
	for(int x = 0; x < data.width(); x++)
	{
	    for(int s = 0; s < sampleCount[y][x]; ++s)
		result->data[y][x][s] = data[y][x][s];
	}
    }

    return result;
}

template<typename T>
TypedDeepImageChannel<T> *TypedDeepImageChannel<T>::CreateSameType(const Array2D<unsigned int> &sampleCount) const
{
    return new TypedDeepImageChannel<T>(width, height, sampleCount);
}

template<typename T>
void TypedDeepImageChannel<T>::CopySamples(shared_ptr<const DeepImageChannel> OtherChannel, int x, int y, int firstIdx)
{
    shared_ptr<const TypedDeepImageChannel<T>> TypedOtherChannel = dynamic_pointer_cast<const TypedDeepImageChannel<T>>(OtherChannel);
    if(TypedOtherChannel == nullptr)
	return;

    const T *src = TypedOtherChannel->GetSamples(x, y);
    T *dst = this->GetSamples(x, y);
    for(int s = 0; s < TypedOtherChannel->sampleCount[y][x]; ++s)
	dst[s + firstIdx] = src[s];
}

template<typename T>
TypedDeepImageChannel<T>::~TypedDeepImageChannel()
{
    for(int y = 0; y < data.height(); y++)
    {
	for(int x = 0; x < data.width(); x++)
	    delete[] data[y][x];
    }
}

template<typename T>
void TypedDeepImageChannel<T>::Reorder(int x, int y, const vector<pair<int,int>> &swaps)
{
    run_swaps(data[y][x], swaps);
}

template<typename T>
void TypedDeepImageChannel<T>::AddSample(int x, int y, int count)
{
    assert(count > 0);
    T *newArray = new T[count];
    memcpy(newArray, data[y][x], sizeof(T) * (count-1));
    newArray[count-1] = defaultValue;
    delete[] data[y][x];
    data[y][x] = newArray;
}

template<typename T>
void TypedDeepImageChannel<T>::AddToFramebuffer(string name, const Header &header, DeepFrameBuffer &frameBuffer, int channel)
{
    // Make sure we don't add the same channel multiple times, since the second one
    // will silently replace the first.
    if(frameBuffer.findSlice(name) != NULL)
	throw exception("The same EXR channel was added more than once");

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

template<typename T>
void TypedDeepImageChannel<T>::UnpremultiplyChannel(shared_ptr<DeepImageChannelProxy> A)
{
    for(int y = 0; y < height; y++)
    {
	for(int x = 0; x < width; x++)
	{
	    T *channelSamples = GetSamples(x, y);
	    for(int s = 0; s < sampleCount[y][x]; ++s)
	    {
		float a = A->Get(x,y,s);
		if(a > 0.00001f)
		    channelSamples[s] = T(channelSamples[s] / a);
	    }
	}
    }
}

DeepImage::DeepImage(int width_, int height_)
{
    width = width_;
    height = height_;
    sampleCount.resizeErase(height, width);

    // resizeErase does not erase.
    memset(&sampleCount[0][0], 0, sizeof(sampleCount[0][0]) * width * height);
}

namespace {
    // This is used below to read the given channel from a vector.  If the type
    // is just a float, the value is returned and the channel number is ignored.
    template<typename T>
    float GetValueForChannel(const T &vec, int channel)
    {
	return vec[channel];
    }

    float GetValueForChannel(float value, int channel)
    {
	return value;
    }

    template<typename T>
    class DeepImageChannelProxyImpl: public DeepImageChannelProxy
    {
    public:
	DeepImageChannelProxyImpl<T>(shared_ptr<const TypedDeepImageChannel<T>> source_, int channel_):
	    source(source_),
	    DeepImageChannelProxy(source_, channel_)
	{
	}

	// Get a sample for for the given pixel.
	float Get(int x, int y, int sample) const
	{
	    auto value = source->Get(x, y, sample);
	    return GetValueForChannel(value, channel);
	}

	shared_ptr<const TypedDeepImageChannel<T>> source;
    };
}

shared_ptr<DeepImageChannelProxy> DeepImage::GetAlphaChannel() const
{
    // Return a proxy for the alpha channel of RGBA.
    auto rgba = GetChannel<V4f>("rgba");
    return make_shared<DeepImageChannelProxyImpl<V4f>>(rgba, 3);
}

void DeepImage::AddSample(int x, int y)
{
    sampleCount[y][x]++;
    for(auto it: channels)
    {
	shared_ptr<DeepImageChannel> channel = it.second;
	channel->AddSample(x, y, sampleCount[y][x]);
    }
}

void DeepImage::AddSampleCountSliceToFramebuffer(DeepFrameBuffer &frameBuffer)
{
    Box2i dataWindow = header.dataWindow();

    frameBuffer.insertSampleCountSlice(Slice(UINT,
	(char *) (&sampleCount[0][0] - dataWindow.min.x - dataWindow.min.y * width),
	sizeof(unsigned int), sizeof(unsigned int) * width));
}


shared_ptr<DeepImage> DeepImageReader::Open(string filename)
{
    // First, read just the header to check that this is a deep EXR.
    {
	auto tempFile = make_shared<InputFile>(filename.c_str());
	int fileVersion = tempFile->version();
	if((fileVersion & 0x800) == 0)
	    throw exception("Input file is not a deep EXR.");
    }

    file = make_shared<DeepScanLineInputFile>(filename.c_str());

    Box2i dataWindow = file->header().dataWindow();
    const int width = dataWindow.max.x - dataWindow.min.x + 1;
    const int height = dataWindow.max.y - dataWindow.min.y + 1;

    image = make_shared<DeepImage>(width, height);
    image->header = file->header();

    // Read the sample counts.  This needs to be done before we start adding channels, so we know
    // how much space to allocate.
    DeepFrameBuffer frameBuffer;
    image->AddSampleCountSliceToFramebuffer(frameBuffer);
    file->setFrameBuffer(frameBuffer);
    file->readPixelSampleCounts(dataWindow.min.y, dataWindow.max.y);
    return image;
}

void DeepImageReader::Read(const DeepFrameBuffer &frameBuffer)
{
    // Read the main image data.
    Box2i dataWindow = file->header().dataWindow();
    file->setFrameBuffer(frameBuffer);
    file->readPixelSampleCounts(dataWindow.min.y, dataWindow.max.y);
    file->readPixels(dataWindow.min.y, dataWindow.max.y);

    file.reset();
    image.reset();
}
