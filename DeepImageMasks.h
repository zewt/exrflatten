#ifndef DeepImageMasks_h
#define DeepImageMasks_h

#include <string>
#include <memory>
using namespace std;

#include <OpenEXR/ImathVec.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>

#include "DeepImage.h"

// This creates simple monochrome masks from various things in a deep EXR file.
struct CreateMask
{
    enum Mode
    {
        CreateMaskMode_FacingAngle,
        CreateMaskMode_Depth,
        CreateMaskMode_Distance,
    };
    Mode mode = CreateMaskMode_FacingAngle;

    // The name of the channel to output the mask to.
    string outputChannelName;

    // The source layer to read.
    string srcLayer;

    // CreateMaskMode_FacingAngle: The reference angle.  If zero (default), use the angle
    // away from the camera.
    Imath::V3f angle = Imath::V3f(0,0,0);

    // CreateMaskMode_Distance: The position to measure distance from.
    Imath::V3f pos = Imath::V3f(0,0,0);

    // CreateMaskMode_Depth: minValue is mapped to 0, and maxValue is mapped to 1.
    float minValue = 0, maxValue = 1000;

    // If true, normalize the output to the 0-1 range.
    bool normalize = false;

    // If true, clamp the mask to the 0-1 range.
    bool clamp = true;

    // If true, invert from 0-1 to 1-0.
    bool invert = false;

    string GetSrcLayer() const;

    // Add all layers to frameBuffer that this mask creation will need to read.
    void AddLayers(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const;

    // Create the mask, adding it to the DeepImage.
    shared_ptr<TypedDeepImageChannel<float>> Create(shared_ptr<DeepImage> image) const;

private:
    shared_ptr<TypedDeepImageChannel<float>> CreateFacingAngle(shared_ptr<DeepImage> image) const;
    shared_ptr<TypedDeepImageChannel<float>> CreateDepth(shared_ptr<DeepImage> image) const;
    shared_ptr<TypedDeepImageChannel<float>> CreateDistance(shared_ptr<DeepImage> image) const;
};

// Use CreateMask to create a mask and add it as an EXR channel.
#include "EXROperation.h"
class EXROperation_CreateMask: public EXROperation
{
public:
    EXROperation_CreateMask(const SharedConfig &sharedConfig, string opt, vector<pair<string,string>> arguments);
    void Run(shared_ptr<EXROperationState> state) const;
    void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const;

private:
    CreateMask createMask;
};


#endif
