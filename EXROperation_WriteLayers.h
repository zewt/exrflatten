#ifndef EXROperation_WriteLayers_h
#define EXROperation_WriteLayers_h

#include <memory>
#include <string>
using namespace std;

#include "EXROperation.h"
#include "DeepImage.h"
#include "SimpleImage.h"

class EXROperation_WriteLayers: public EXROperation
{
public:
    EXROperation_WriteLayers(const SharedConfig &sharedConfig);

    bool AddArgument(string opt, string value);
    void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const;
    void Run(shared_ptr<DeepImage> image) const;

private:
    const SharedConfig &sharedConfig;
    string outputPattern = "<inputname> <ordername> <layer>.exr";

    struct Layer
    {
	string filename;
	string layerName;
	string layerType;
	int order = 0;
	shared_ptr<SimpleImage> image;

	Layer(int width, int height)
	{
	    image = make_shared<SimpleImage>(width, height);
	}
    };

    struct LayerDesc
    {
	string layerName;
	int objectId;
    };
    vector<LayerDesc> layerDescs;

    struct MaskDesc
    {
	void ParseOptionsString(string optionsString);

	enum MaskType
	{
	    // The mask value will be output on the RGB channels.
	    MaskType_Greyscale,

	    // The mask value will be output on the alpha channel.
	    MaskType_Alpha,

	    // The mask will be composited with the color channel and output as a pre-masked
	    // RGBA image.
	    MaskType_Composited,
	};
	MaskType maskType = MaskType_Greyscale;
	string maskChannel;
	string maskName;
    };
    vector<MaskDesc> masks;

    // A list of (dst, src) pairs to combine layers before writing them.
    vector<pair<int,int>> combines;

    string MakeOutputFilename(const Layer &layer) const;
    string GetFrameNumberFromFilename(string s) const;
};

#endif
