#include "tpt-rand.h"
#include <cstdlib>
#include <ctime>

double RNG::uniform01Double()
{
	return double(next() / double(UINT64_C(0xFFFFFFFFFFFFFFFF)));
}

RNG::RNG()
{
	s[0] = time(nullptr);
	s[1] = 614;
}

void RNG::seed(unsigned int sd)
{
	s[0] = sd;
	s[1] = sd;
}

RNG interfaceRng;
