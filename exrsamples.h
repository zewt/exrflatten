#ifndef EXRSAMPLES_H
#define EXRSAMPLES_H

// These are helpers to split and merge samples.  This would be needed to implement
// EXR sample tidying.
//
// This code is from the OpenEXR library documentation, in "InterpretingDeepPixels.pdf".
void splitVolumeSample(
    float a, float c, // Opacity and color of original sample
    float zf, float zb, // Front and back of original sample
    float z, // Position of split
    float& af, float& cf, // Opacity and color of part closer than z
    float& ab, float& cb); // Opacity and color of part further away than z

void mergeOverlappingSamples(
    float a1, float c1, // Opacity and color of first sample
    float a2, float c2, // Opacity and color of second sample
    float &am, float &cm); // Opacity and color of merged sample

#endif

/*
 * Copyright (c) 2006, Industrial Light & Magic, a division of Lucasfilm
 * Entertainment Company Ltd.  Portions contributed and copyright held by
 * others as indicated.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 * 
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided with
 *       the distribution.
 * 
 *     * Neither the name of Industrial Light & Magic nor the names of
 *       any other contributors to this software may be used to endorse or
 *       promote products derived from this software without specific prior
 *       written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
