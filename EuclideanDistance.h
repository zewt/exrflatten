#ifndef EuclideanDistance_h
#define EuclideanDistance_h

#include <memory>
#include <OpenEXR/ImfArray.h>

using namespace std;

namespace EuclideanDistance {
    // Calculate euclidean distance from each pixel to the nearest pixel where the mask is 0.
    struct DistanceResult {
	int sx, sy;
	float distance;
    };

    shared_ptr<Imf::Array2D<DistanceResult>> Calculate(int width, int height, const Imf::Array2D<float> &mask);
}

#endif
