#include "DeepImageUtil.h"
#include "SimpleImage.h"

#include <OpenEXR/ImathVec.h>

using namespace Imf;
using namespace Imath;

// Flatten the color channels of a deep EXR to a simple flat layer.
shared_ptr<SimpleImage> DeepImageUtil::CollapseEXR(shared_ptr<const DeepImage> image, set<int> objectIds)
{
    shared_ptr<SimpleImage> result = make_shared<SimpleImage>(image->width, image->height);

    auto rgba = image->GetChannel<V4f>("rgba");
    auto Z = image->GetChannel<float>("Z");
    auto id = image->GetChannel<uint32_t>("id");

    for(int y = 0; y < image->height; y++)
    {
	for(int x = 0; x < image->width; x++)
	{
	    V4f &out = result->GetRGBA(x, y);
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
