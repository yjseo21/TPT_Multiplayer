#pragma once
#include "ExplicitSingleton.h"
#include <stdint.h>
#include <array>

class RNG
{
public:
	using State = std::array<uint64_t, 2>;

private:
	State s;
	static uint64_t rotl(const uint64_t x, int k)
	{
		return (x << k) | (x >> (64 - k));
	}
	// xoroshiro128+ by David Blackman and Sebastiano Vigna. Inline: particles
	// draw from it millions of times a tick.
	uint64_t next()
	{
		const uint64_t s0 = s[0];
		uint64_t s1 = s[1];
		const uint64_t result = s0 + s1;

		s1 ^= s0;
		s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14); // a, b
		s[1] = rotl(s1, 36); // c

		return result;
	}
public:
	unsigned int operator()()
	{
		return next()&0xFFFFFFFF;
	}
	unsigned int gen()
	{
		return next() & 0x7FFFFFFF;
	}
	int between(int lower, int upper)
	{
		unsigned int r = next();
		return static_cast<int>(r % ((unsigned int)(upper) - (unsigned int)(lower) + 1U)) + lower;
	}
	bool chance(int numerator, unsigned int denominator)
	{
		if (numerator < 0)
			return false;
		return next() % denominator < static_cast<unsigned int>(numerator);
	}
	float uniform01()
	{
		return float((next() & UINT32_C(0xFFFFFFFF)) / float(UINT32_C(0xFFFFFFFF)));
	}
	double uniform01Double();

	RNG();
	void seed(unsigned int sd);

	void state(State ns)
	{
		s = ns;
	}

	State state() const
	{
		return s;
	}
};

// Please only use this on the main thread and never for simulation stuff.
// For simulation stuff, use Simulation::rng. For renderer stuff, use Renderer::rng.
// For anything else, prefer a dedicated RNG instance over this one.
extern RNG interfaceRng;
