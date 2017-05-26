#ifndef DeepImageUtil_H
#define DeepImageUtil_H

#include <memory>
#include <set>
using namespace std;

#include <OpenEXR/ImfHeader.h>

#include "DeepImage.h"
class SimpleImage;

namespace DeepImageUtil {
    const int NO_OBJECT_ID = 0;

    // Flatten the color channels of a deep EXR to a simple flat layer.
    shared_ptr<SimpleImage> CollapseEXR(shared_ptr<const DeepImage> image, set<int> objectIds = {});

    // Change all samples with an object ID of fromObjectId to intoObjectId.
    void CombineObjectId(shared_ptr<DeepImage> image, int fromObjectId, int intoObjectId);

    // Copy all image attributes from one header to another, except for built-in EXR headers that
    // we shouldn't set.
    void CopyLayerAttributes(const Imf::Header &input, Imf::Header &output);

    // Sort samples based on the depth of each pixel, furthest from the camera first.
    void SortSamplesByDepth(shared_ptr<DeepImage> image);

    // Return the final visibility of each sample at the given pixel.
    //
    // If a sample has three pixels with alpha 1.0, 0.5 and 0.5, the first sample is covered by the
    // samples on top of it, and the final visibility is { 0.25, 0.25, 0.5 }.
    vector<float> GetSampleVisibility(shared_ptr<const DeepImage> image, int x, int y);

    // Copy all samples from all channels of images into a single image.
    shared_ptr<DeepImage> CombineImages(vector<shared_ptr<DeepImage>> images);
}

#endif

