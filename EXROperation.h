#ifndef EXROperation_h
#define EXROperation_h

#include <string>
#include <vector>
#include <memory>
using namespace std;

class DeepImage;

#include <OpenEXR/ImfDeepFrameBuffer.h>

// Configuration settings shared by multiple EXROperations.  These can be specified
// at any point on the commandline, and aren't order specific.  They can't be different
// for different operations.
struct SharedConfig
{
    string outputPath;
    vector<string> inputFilenames;

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
    virtual bool AddArgument(string opt, string value) { return false; }

    // The last commandline argument for this operation has been received.
    virtual void ArgumentsComplete() { }

    // Add all EXR channels needed by this operation.
    virtual void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const { };

    // Run the operation on the DeepImage.
    virtual void Run(shared_ptr<DeepImage> image) const = 0;
};

#endif
