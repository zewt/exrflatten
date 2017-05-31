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

class EXROperation
{
public:
    // Add all EXR channels needed by this operation.
    virtual void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const { };

    // Run the operation on the DeepImage.
    virtual void Run(shared_ptr<DeepImage> image) const = 0;
};

#endif
