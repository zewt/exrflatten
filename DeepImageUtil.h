#ifndef DeepImageUtil_H
#define DeepImageUtil_H

#include <memory>
#include <set>
using namespace std;

#include "DeepImage.h"
class SimpleImage;

namespace DeepImageUtil {
    // Flatten the color channels of a deep EXR to a simple flat layer.
    shared_ptr<SimpleImage> CollapseEXR(shared_ptr<const DeepImage> image, set<int> objectIds = {});
}

#endif

