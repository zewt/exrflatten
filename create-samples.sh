#!/bin/bash
mkdir -p output

# sample1 is a render with a lot of transparency, which is useful to show how layering
# can be used with transparent objects.
#
# sample2 is a render with various overlapping opaque objects.  This is useful for
# showing strokes, since strokes don't work well with transparent objects.
#

# Layering

# sample2 has four objects, each with their own ID.  Let's separate it into four EXRs.  These
# can be blended on top of each other to get the original scene.  We're limited in which layers
# we can hide in the result, however.  We can hide YellowCone, since it's both in the back
# of the output layers and in the background of the scene.  If we try to hide TwistedCylinder,
# the cone will appear correctly, but the sphere will be cut out.  This is because the render
# simply doesn't have any data there.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --save-layers \
        --filename-pattern="01 - layer sample - <inputname> <ordername> <layer>.exr" \
        --layer=1=YellowCone \
        --layer=2=TwistedCylinder \
        --layer=3=Sphere \
        --layer=4=GreenCone

# sample3a and sample3b are the same scene as sample2, but with the twisted cylinder rendered
# into sample3a and everything else into sample3b.  If these files are merged in a deep
# compositor, they give exactly the same result as sample2.
#
# Let's do the same thing we did with sample2, this time merging the two separate files.
# This way we really do have all of the pixel data for TwistedCylinder, so if its layer
# is hidden we'll see everything underneath it.  However, we can't hide GreenCone, or we'll
# see a hole in TwistedCylinder.  A hole is cut out of it to show GreenGone, because although
# TwistedCylinder is on top in the layers, it's not actually in the front of the scene.
#
# This all happens automatically, so you only need to worry about it if you're trying to hide
# or mask layers.
exrflatten \
    --input=sample/sample3a.exr \
    --input=sample/sample3b.exr \
    --output=output \
    --save-layers \
        --filename-pattern="02 - layer sample - <inputname> <ordername> <layer>.exr" \
        --layer=1=YellowCone \
        --layer=3=Sphere \
        --layer=4=GreenCone \
        --layer=2=TwistedCylinder

# Layering with transparency
#
# sample1.exr has three object IDs.  Create a layer for each one.  This is the same as the
# above, but shows how transparency works properly even after separating layers.
#
# This is the command used to create the layers for sample.psd.
exrflatten \
    --input=sample/sample1.exr \
    --output=output \
    --save-layers \
        --filename-pattern="03 - layer sample - <inputname> <ordername> <layer>.exr" \
        --layer=1=Cylinder \
        --layer="2=Green helix" \
        --layer="3=Red helix"

# Strokes

# Going back to the solid object scene, let's add a stroke around a single layer, the
# TwistedCylinder.  The stroke will correctly be covered by the GreenCone covering the
# cylinder.
#
# We're not specifying any --layers this time, so we'll just get a single layer, "default".
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --stroke=2 \
    --save-layers \
        --filename-pattern="04 - stroke sample - <inputname> <ordername> <layer>.exr"

# Do the same thing, this time also adding intersection lines.  This uses edge detection
# to add lines where the object crosses itself, which a simple stroke can't do.  All we
# need to do this is a world space position channel, which is "P" in Arnold.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --stroke=2 --intersections \
    --save-layers \
        --filename-pattern="05 - stroke with intersections - <inputname> <ordername> <layer>.exr"

# Do it again, but this time output the object to its own layer.  This is simply using --stroke
# and --layer together.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --stroke=2 --intersections \
    --save-layers \
        --layer=2=TwistedCylinder \
        --filename-pattern="06 - stroke layer sample - <inputname> <ordername> <layer>.exr"

# A more advanced usage: put the stroke in a dedicated layer, which allows the stroke to be
# masked or hidden.  Specifying output-id=1000 tells it to assign the stroke samples the object
# ID 1000, and we can then output it as a layer.
#
# Note that due to the way strokes are composited, some of the color from the object will bleed
# into the stroke layer.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --stroke=2 --intersections --output-id=1000 \
    --save-layers \
        --layer=2=Bend \
        --layer=1000=BendStroke \
        --filename-pattern="07 - stroke layer sample - <inputname> <ordername> <layer>.exr"

# Masks
#
# You can render a mask, or generate them from other channels.  --layer-mask=outputs a mask, and --create-mask
# creates a mask (to be used with --layer-mask).

# Create a depth mask.  The "normalize" option automatically normalizes the mask, so the nearest objects
# to the camera are black and the furthest are white.  Save the mask as "Mask".  The mask will be greyscale
# by default.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --create-mask=depth --name=MaskLayer --normalize \
    --save-layers \
        --filename-pattern="08 - depth mask (grey) - <inputname> <ordername> <layer>.exr" \
        --layer-mask="channel=MaskLayer;name=Mask"

# Output the same mask as an alpha mask.  This has the same data, but in the alpha channel instead of
# greyscale.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --create-mask=depth --name=MaskLayer --normalize \
    --save-layers \
        --filename-pattern="09 - depth mask (a) - <inputname> <ordername> <layer>.exr" \
        --layer-mask="channel=MaskLayer;name=Mask;alpha" 

# Output the same mask as a composited RGB image.  This pre-composites the mask onto color, giving an
# image that can be used directly.  This doesn't composite as generally on other things, but the mask
# will be correctly depth composited.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --create-mask=depth --name=MaskLayer --normalize \
    --save-layers \
        --filename-pattern="10 - depth mask (rgb) - <inputname> <ordername> <layer>.exr" \
        --layer-mask="channel=MaskLayer;name=Mask;rgb" 

# Create a facing angle mask from normals.  The mask will be white where objects are facing perpendicular to
# the camera, and black where they're facing the camera.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --create-mask=facing --name=MaskLayer --normalize \
    --save-layers \
        --filename-pattern="11 - facing mask - <inputname> <ordername> <layer>.exr" \
        --layer-mask="channel=MaskLayer;name=Mask" 

# Create a mask from the distance to a given world space position.  The position happens to be the
# tip of the yellow cone.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --create-mask=distance --name=MaskLayer --normalize --pos=1.115,5.771,3.643 \
    --save-layers \
        --layer-mask="channel=MaskLayer;name=Mask" \
        --filename-pattern="12 - distance mask - <inputname> <ordername> <layer>.exr" \


# Using masks with strokes

# We can create a mask, and then use it on a stroke.  This creates a mask around a point
# on the TwistedCylinder, and then uses it to mask where we apply a stroke on the object.
# It's important that --create-mask be before --stroke, since operations are run in the
# order they're given and we need to make the mask first.  Also note that strokes and other
# operations can't update masks created earlier, so the mask won't have useful values after
# the stroke is created and shouldn't be written with --layer-mask.  If you want to create
# a mask that includes a stroke, create the stroke first.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --create-mask=distance --name=MaskLayer --min=0 --max=1 --pos=-0.462,5.308,-6.169 --invert \
    --stroke=2 --stroke-mask=MaskLayer \
    --save-layers \
        --filename-pattern="13 - depth mask (a) - <inputname> <ordername> <layer>.exr" \
    
# You can save a simple flattened EXR with --save-flattened.  This will write the current image
# to a file.  Here, we apply a stroke to one object, save it, then apply a stroke to a second
# image and save that (including the stroke we added first).  This is useful for troubleshootnig,
# or if you just want to apply filters and don't want layered output.
#
# This will output into the most recent --output=directory.
exrflatten \
    --input=sample/sample2.exr \
    --output=output \
    --stroke=1 \
    --save-flattened="14 - flat 1.exr" \
    --stroke=2 \
    --save-flattened="14 - flat 2.exr"


