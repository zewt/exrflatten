#ifndef DeepImage_h
#define DeepImage_h

#include <string>
#include <vector>
#include <memory>

#include <OpenEXR/ImfDeepFrameBuffer.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfHeader.h>

using namespace std;

// A base class for a deep image channel.  Most of the real work is in TypedDeepImageChannel.
class DeepImageChannel
{
public:
    virtual ~DeepImageChannel()
    {
    }

    virtual void AddToFramebuffer(string name, const Imf::Header &header, Imf::DeepFrameBuffer &frameBuffer, int channel = 0) = 0;
    virtual void Reorder(int x, int y, const vector<pair<int,int>> &order) = 0;
    virtual void AddSample(int x, int y, int count) = 0;
    virtual DeepImageChannel *Clone() const = 0;
    virtual DeepImageChannel *CreateSameType(const Imf::Array2D<unsigned int> &sampleCount) const = 0;
    virtual void CopySamples(shared_ptr<const DeepImageChannel> OtherChannel, int x, int y, int firstIdx) = 0;

    // Arnold multiplies channels by alpha that shouldn't be.  Premultiplying only makes sense for
    // color channels, but Arnold does it with world space positions and other data.  If this is
    // true, this is a channel that we need to divide by alpha to work around this problem.
    bool needsAlphaDivide = false;
    virtual void UnpremultiplyChannel(shared_ptr<const DeepImageChannel> rgba) = 0;
};

template<typename T>
class TypedDeepImageChannel: public DeepImageChannel
{
public:
    TypedDeepImageChannel<T>(int width, int height, const Imf::Array2D<unsigned int> &sampleCount);
    ~TypedDeepImageChannel();

    // Each pixel has multiple samples, and each sample can have multiple elements for vector types.
    // The X stride is the number of bytes from one pixel to the next: sizeof(vector<T>).
    // The sample stride is the number of bytes from one sample to the next: sizeof(T).
    void AddToFramebuffer(string name, const Imf::Header &header, Imf::DeepFrameBuffer &frameBuffer, int channel = 0);

    // Reorder our data to the given order.  swaps is a list of swaps (see make_swaps
    // and run_swaps).
    void Reorder(int x, int y, const vector<pair<int,int>> &swaps);

    // Add an empty sample to the end of the list for the given pixel.
    void AddSample(int x, int y, int count);

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

    // Copy this layer and its data.
    TypedDeepImageChannel<T> *Clone() const;

    // Return a new, empty TypedDeepImageChannel of this type, with a new sampleCount.
    TypedDeepImageChannel<T> *CreateSameType(const Imf::Array2D<unsigned int> &sampleCount) const;

    // Copy all samples from OtherChannel.  The samples will be output starting at firstIdx.
    // OtherChannel must have the same templated type as this object, and there must be enough
    // samples allocated to hold the copied samples.
    void CopySamples(shared_ptr<const DeepImageChannel> OtherChannel, int x, int y, int firstIdx);

    // Unpremultiply this channel.
    void UnpremultiplyChannel(shared_ptr<const DeepImageChannel> rgba);

    int width, height;
    Imf::Array2D<T *> data;
    vector<shared_ptr<Imf::Array2D<T *>>> readPointers;

    // The default value for this channel when adding new samples with AddSample.
    T defaultValue = T();

    // This is a reference to DeepImage::sampleCount, which is shared by all channels.
    const Imf::Array2D<unsigned int> &sampleCount;
};

extern template class TypedDeepImageChannel<uint32_t>;  
extern template class TypedDeepImageChannel<float>;  
extern template class TypedDeepImageChannel<Imath::V3f>;  
extern template class TypedDeepImageChannel<Imath::V4f>;  


class DeepImage
{
public:
    DeepImage(int width, int height);

    // Add a channel, and add it to the DeepFrameBuffer to be read.
    //
    // channelName is the name to assign this channel.  This usually corresponds with an EXR channel
    // or layer name, but this isn't required.
    //
    // For vector types, exrChannels is a list of the EXR channels for each component, eg. { "P.X", "P.Y", "P.Z" }.
    template<typename T>
    shared_ptr<TypedDeepImageChannel<T>> AddChannelToFramebuffer(string channelName, vector<string> exrChannels, Imf::DeepFrameBuffer &frameBuffer, bool needsAlphaDivide);

    // Set sampleCount as the sample count slice in the given framebuffer.
    void AddSampleCountSliceToFramebuffer(Imf::DeepFrameBuffer &frameBuffer);

    // Add a channel with the given name.
    template<typename T>
    shared_ptr<TypedDeepImageChannel<T>> AddChannel(string name);

    template<typename T>
    shared_ptr<TypedDeepImageChannel<T>> GetChannel(string name);

    template<typename T>
    shared_ptr<const TypedDeepImageChannel<T>> GetChannel(string name) const
    {
	return const_cast<DeepImage *>(this)->GetChannel<T>(name);
    }

    // Add a sample to each channel for the given pixel.
    void AddSample(int x, int y);

    // Get the number of samples for the given pixel.  All channels always have the same
    // number of samples for any given pixel.
    int NumSamples(int x, int y) const { return sampleCount[y][x]; }

    int width = 0, height = 0;
    Imf::Header header;
    map<string, shared_ptr<DeepImageChannel>> channels;
    Imf::Array2D<unsigned int> sampleCount;
};

template<typename T>
shared_ptr<TypedDeepImageChannel<T>> DeepImage::AddChannelToFramebuffer(string channelName, vector<string> exrChannels, Imf::DeepFrameBuffer &frameBuffer, bool needsAlphaDivide_)
{
    if(channels.find(channelName) != channels.end())
    {
	// Just return the channel we already created with this name.
	auto result = dynamic_pointer_cast<TypedDeepImageChannel<T>>(channels.at(channelName));
	if(result == nullptr)
	    throw exception("A channel was added twice with different data types");
	return result;
    }

    shared_ptr<TypedDeepImageChannel<T>> channel = AddChannel<T>(channelName);
    channel->needsAlphaDivide = needsAlphaDivide_;

    int idx = 0;
    for(string exrChannel: exrChannels)
	channel->AddToFramebuffer(exrChannel, header, frameBuffer, idx++);
    return channel;
}

template<typename T>
shared_ptr<TypedDeepImageChannel<T>> DeepImage::AddChannel(string name)
{
    auto channel = make_shared<TypedDeepImageChannel<T>>(width, height, sampleCount);
    channels[name] = channel;
    return channel;
}

template<typename T>
shared_ptr<TypedDeepImageChannel<T>> DeepImage::GetChannel(string name)
{
    auto it = channels.find(name);
    if(it == channels.end())
	return nullptr;

    shared_ptr<DeepImageChannel> result = it->second;
    auto typedResult = dynamic_pointer_cast<TypedDeepImageChannel<T>>(result);
    return typedResult;
}

class DeepImageReader
{
public:
    // Open an EXR, and read its header and sample counts.
    shared_ptr<DeepImage> Open(string filename);

    // Read the read of the file opened by a call to Open.  This should be called after setting
    // up channels to read by calling image->AddChannelToFramebuffer.
    void Read(const Imf::DeepFrameBuffer &frameBuffer);

private:
    shared_ptr<Imf::DeepScanLineInputFile> file;
    shared_ptr<DeepImage> image;
};

#endif
