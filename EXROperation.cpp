#include "EXROperation.h"

bool SharedConfig::ParseOption(string opt, string value)
{
    if(opt == "input")
    {
	inputFilenames.push_back(value);
	return true;
    }
    else if(opt == "output")
    {
	outputPath = value;
	return true;
    }
    else if(opt == "units")
    {
	if(value == "cm")
	    worldSpaceScale = 100;
	else if(value == "meters")
	    worldSpaceScale = 100; // cm per meter
	else if(value == "feet")
	    worldSpaceScale = 30.48f; // cm per foot
	else
	    worldSpaceScale = (float) atof(value.c_str());

	if(worldSpaceScale < 0.0001f)
	    throw StringException("Invalid world space scale: " + value);
    }

    return false;
}

