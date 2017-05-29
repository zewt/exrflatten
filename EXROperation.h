#ifndef EXROperation_h
#define EXROperation_h

class EXROperation
{
public:
    virtual bool AddArgument(string opt, string value) { return false; }

    // Add all EXR channels needed by this operation.
    virtual void AddChannels(shared_ptr<DeepImage> image, DeepFrameBuffer &frameBuffer) const { };

    // Run the operation on the DeepImage.
    virtual void Run(shared_ptr<DeepImage> image) = 0;
};

#endif
