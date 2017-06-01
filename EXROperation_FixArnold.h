#ifndef EXROperation_FixArnold_h
#define EXROperation_FixArnold_h

#include "EXROperation.h"
#include <memory>
using namespace std;

// Arnold outputs P AOVs with broken data ... sometimes.  The values seem to be multiplied
// by alpha.  However, this doesn't always happen, so we can't just always divide by
// alpha.  Do some ugly logic to figure out whether the alpha multiplication has
// happened: multiply them by the worldToNDC matrix to derive the screen space coordinate,
// and see whether we get correct results as-is or after dividing by alpha.
//
// This operation is inserted automatically at the start of the list.
class EXROperation_FixArnold: public EXROperation
{
public:
    EXROperation_FixArnold() { }
    void AddChannels(shared_ptr<DeepImage> image, Imf::DeepFrameBuffer &frameBuffer) const;
    void Run(shared_ptr<DeepImage> image) const;

private:
    bool IsArnold(shared_ptr<DeepImage> image) const;
};

#endif
