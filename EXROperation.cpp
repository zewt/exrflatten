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

    return false;
}

