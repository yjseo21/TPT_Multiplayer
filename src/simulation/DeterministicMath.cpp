// See DeterministicMath.h. Strict IEEE arithmetic whatever the build flags, as
// in gravity/DeterministicGravity.cpp: the pragmas turn fast-math off, and
// every product that is then added passes through Round(), so no compiler can
// fuse it into a multiply-add (clang's -ffp-contract=fast ignores pragmas, and
// ARM64 fuses).
#if defined(__clang__)
# pragma float_control(precise, on)
# pragma clang fp contract(off)
#elif defined(__GNUC__)
# pragma GCC optimize("no-fast-math", "fp-contract=off")
#elif defined(_MSC_VER)
# pragma float_control(precise, on)
# pragma fp_contract(off)
#endif

#include "DeterministicMath.h"
#include <cmath>
#include <initializer_list>
#include <limits>

namespace dmath
{
namespace
{
// The value, rounded to double now: an empty asm statement that claims to use
// and change it in a floating-point register, so a multiply before it cannot be
// fused with an add after it. No instructions are emitted.
inline double Round(double x)
{
#if defined(__GNUC__) && (defined(__aarch64__) || defined(__arm__))
	__asm__("" : "+w"(x));
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	__asm__("" : "+x"(x));
#elif defined(__GNUC__)
	__asm__("" : "+g"(x));
#endif
	return x;
}
inline double Mul(double a, double b) { return Round(a * b); }

constexpr double Nan = std::numeric_limits<double>::quiet_NaN();
constexpr double Inf = std::numeric_limits<double>::infinity();
constexpr double Pi = 3.14159265358979311600e+00;
constexpr double HalfPi = 1.57079632679489655800e+00;
constexpr double QuarterPi = 7.85398163397448278999e-01;
// pi/2 split so that k * PiO2Hi is exact for the k used here (fdlibm's split).
constexpr double PiO2Hi = 1.57079632673412561417e+00, PiO2Lo = 6.07710050650619224932e-11;
constexpr double TwoOverPi = 6.36619772367581382433e-01;
constexpr double Ln2Hi = 6.93147180369123816490e-01, Ln2Lo = 1.90821492927058770002e-10;
constexpr double Log2E = 1.44269504088896338700e+00;
constexpr double Sqrt1_2 = 7.07106781186547572737e-01;

// sin and cos of r in [-pi/4, pi/4], by their Taylor series in Horner form,
// innermost term first:
//   sin r = r (1 - z/(2*3) (1 - z/(4*5) (1 - ...))),  z = r*r
//   cos r =    1 - z/(1*2) (1 - z/(3*4) (1 - ...))
double SinCore(double r)
{
	double z = Mul(r, r), s = 0;
	for (double d : { 342., 272., 210., 156., 110., 72., 42., 20., 6. })
		s = 1 - Mul(z / d, s);
	return Mul(r, s);
}
double CosCore(double r)
{
	double z = Mul(r, r), c = 0;
	for (double d : { 380., 306., 240., 182., 132., 90., 56., 30., 12., 2. })
		c = 1 - Mul(z / d, c);
	return c;
}

// Reduces x to r in [-pi/4, pi/4] with x = r + k pi/2; returns k mod 4.
int Reduce(double x, double &r)
{
	double k = std::floor(Mul(x, TwoOverPi) + .5);
	r = Round(x - Mul(k, PiO2Hi)) - Mul(k, PiO2Lo);
	double q = k - 4 * std::floor(k / 4);
	return int(q);
}

// atan of t in [0, tan(pi/8)], by its series.
double AtanCore(double t)
{
	double z = Mul(t, t), s = 0;
	for (int n = 24; n >= 1; --n)
		s = 1. / (2 * n + 1) - Mul(z, s);
	return t - Mul(Mul(t, z), s);
}
double Atan(double x)
{
	bool negative = x < 0;
	if (negative)
		x = -x;
	double result;
	if (x > 1)
	{
		double inverse = 1 / x;
		result = HalfPi - (inverse > 0.41421356237309503 ? QuarterPi + AtanCore((inverse - 1) / (inverse + 1)) : AtanCore(inverse));
	}
	else if (x > 0.41421356237309503) // tan(pi/8)
		result = QuarterPi + AtanCore((x - 1) / (x + 1));
	else
		result = AtanCore(x);
	return negative ? -result : result;
}
}

double sin(double x)
{
	if (!std::isfinite(x))
		return Nan;
	double r;
	switch (Reduce(x, r))
	{
	case 0: return SinCore(r);
	case 1: return CosCore(r);
	case 2: return -SinCore(r);
	default: return -CosCore(r);
	}
}

double cos(double x)
{
	if (!std::isfinite(x))
		return Nan;
	double r;
	switch (Reduce(x, r))
	{
	case 0: return CosCore(r);
	case 1: return -SinCore(r);
	case 2: return -CosCore(r);
	default: return SinCore(r);
	}
}

double atan2(double y, double x)
{
	if (std::isnan(x) || std::isnan(y))
		return Nan;
	if (x == 0)
	{
		if (y == 0)
			return std::signbit(x) ? (std::signbit(y) ? -Pi : Pi) : y;
		return y > 0 ? HalfPi : -HalfPi;
	}
	if (std::isinf(x) || std::isinf(y))
	{
		if (std::isinf(x) && std::isinf(y))
			return x > 0 ? (y > 0 ? QuarterPi : -QuarterPi) : (y > 0 ? 3 * QuarterPi : -3 * QuarterPi);
		if (std::isinf(y))
			return y > 0 ? HalfPi : -HalfPi;
		return x > 0 ? (std::signbit(y) ? -0.0 : 0.0) : (std::signbit(y) ? -Pi : Pi);
	}
	double a = Atan(y / x);
	if (x > 0)
		return a;
	return std::signbit(y) ? a - Pi : a + Pi;
}

double exp(double x)
{
	if (std::isnan(x))
		return Nan;
	if (x > 709.782712893384)
		return Inf;
	if (x < -745.1332191019411)
		return 0;
	double k = std::floor(Mul(x, Log2E) + .5);
	double r = Round(x - Mul(k, Ln2Hi)) - Mul(k, Ln2Lo); // |r| <= 0.35
	double p = 0;
	for (int n = 20; n >= 1; --n)
		p = 1 + Mul(r / n, p);
	return std::ldexp(p, int(k));
}

double log(double x)
{
	if (std::isnan(x) || x < 0)
		return Nan;
	if (x == 0)
		return -Inf;
	if (std::isinf(x))
		return Inf;
	int e;
	double m = std::frexp(x, &e); // [0.5, 1)
	if (m < Sqrt1_2)
	{
		m *= 2;
		--e;
	}
	double s = (m - 1) / (m + 1), z = Mul(s, s), series = 0; // |s| <= 0.172
	for (int n = 15; n >= 0; --n)
		series = 1. / (2 * n + 1) + Mul(z, series);
	double logM = 2 * Mul(s, series);
	return Mul(e, Ln2Hi) + (logM + Mul(e, Ln2Lo));
}

double pow(double x, double y)
{
	if (y == 0)
		return 1;
	if (std::isnan(x) || std::isnan(y))
		return Nan;
	bool integer = std::floor(y) == y;
	// Whole powers by repeated squaring: exact where the result is (x*x for 2).
	if (integer && std::fabs(y) <= 64)
	{
		int n = int(std::fabs(y));
		double result = 1, base = x;
		while (n)
		{
			if (n & 1)
				result = Mul(result, base);
			base = Mul(base, base);
			n >>= 1;
		}
		return y < 0 ? 1 / result : result;
	}
	if (x == 0)
		return y > 0 ? 0 : Inf;
	if (x < 0)
	{
		if (!integer)
			return Nan;
		bool odd = std::fabs(std::fmod(y, 2.0)) == 1;
		double magnitude = exp(Mul(y, log(-x)));
		return odd ? -magnitude : magnitude;
	}
	if (std::isinf(x))
		return y > 0 ? Inf : 0;
	return exp(Mul(y, log(x)));
}

double hypot(double x, double y)
{
	if (std::isinf(x) || std::isinf(y))
		return Inf;
	return std::sqrt(Round(Mul(x, x) + Mul(y, y)));
}
}
