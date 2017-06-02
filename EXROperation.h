#ifndef EXROperation_h
#define EXROperation_h

#include <string>
#include <vector>
#include <memory>
using namespace std;

class DeepImage;
#include "helpers.h"

#include <OpenEXR/ImfDeepFrameBuffer.h>

// Configuration settings shared by multiple EXROperations.  These can be specified
// at any point on the commandline, and aren't order specific.  They can't be different
// for different operations.
struct SharedConfig
{
    string outputPath;
    vector<string> inputFilenames;

    // Tunable distance values are in cm.  This can be used to adjust all distances for
    // scenes with a different scale.  If you're in meters, this should be 100, to indicate
    // that a unit is 100x bigger than we expect.  For feet, use 30.48.
    float worldSpaceScale = 1.0f;

    bool ParseOption(string opt, string value);

    // Given a filename, return the path to save it.
    string GetFilename(string filename) const
    {
	if(!outputPath.empty())
	    filename = outputPath + "/" + filename;
	return filename;
    }
};

struct EXROperationState
{
    // An operation can modify state->image directly, and it and other operations will
    // see the changes immediately, but this isn't always wanted.  GetOutputImage can be
    // called to get a separate image.  This will have the same dimensions and channels
    // as state->image, with empty channels.  Samples can be added to this image, and they'll
    // be combined into the final image later.
    //
    // This is useful when multiple operations want to add samples to the image, without seeing
    // any of the samples added by previous operations.  The samples will be queued up in the
    // temporary image so all of the operations can do their work, then they'll be combined
    // later.
    //
    // Note that GetOutputImage will always return the same temporary image, and not create
    // a new temporary image each time it's called.  This is only used to store samples, so
    // allocating a new one for each operation would just take longer.
    shared_ptr<DeepImage> GetOutputImage();

    // Combine all images created by GetOutputImage into image.
    void CombineWaitingImages();

    // The image to work with.
    shared_ptr<DeepImage> image;

    // If an operation calls CreateNewImage, this is the image it created.
    shared_ptr<DeepImage> newImage;

    // All newImages that have been created, which are waiting to be merged into image.
    vector<shared_ptr<DeepImage>> waitingImages;
};

class EXROperation
{
public:
    // Add all EXR channels needed by this operation.
    virtual void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const { };

    // Run the operation on the DeepImage.
    virtual void Run(shared_ptr<EXROperationState> state) const = 0;
};

#endif
