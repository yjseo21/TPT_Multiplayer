#pragma once
#include <type_traits>

// Maths functions that give the same bits on every compiler, maths library and
// CPU. Rooms run physics on the server and on every player's computer (Linux,
// Windows, macOS, x86 and ARM), and the system libraries' sin, cos, pow, exp,
// atan2 and hypot differ in their last bits: enough to make copies of a world
// drift apart within a second. These are built only from operations IEEE
// defines exactly (+ - * /, sqrt, floor, frexp, ldexp), with the same steps
// everywhere. Accuracy is within a few units in the last place of double, which
// is far more than the simulation needs.
//
// Overloads follow <cmath>: float arguments give float, otherwise double.
namespace dmath
{
double sin(double x);
double cos(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double pow(double x, double y);
double hypot(double x, double y);

inline float sin(float x) { return float(sin(double(x))); }
inline float cos(float x) { return float(cos(double(x))); }
inline float atan2(float y, float x) { return float(atan2(double(y), double(x))); }
inline float exp(float x) { return float(exp(double(x))); }
inline float log(float x) { return float(log(double(x))); }
inline float pow(float x, float y) { return float(pow(double(x), double(y))); }
inline float hypot(float x, float y) { return float(hypot(double(x), double(y))); }

inline float sinf(float x) { return sin(x); }
inline float cosf(float x) { return cos(x); }
inline float atan2f(float y, float x) { return atan2(y, x); }
inline float expf(float x) { return exp(x); }
inline float logf(float x) { return log(x); }
inline float powf(float x, float y) { return pow(x, y); }
inline float hypotf(float x, float y) { return hypot(x, y); }

// Mixed and integer arguments promote to double, as <cmath> does.
template<class T> requires std::is_integral_v<T> double sin(T x) { return sin(double(x)); }
template<class T> requires std::is_integral_v<T> double cos(T x) { return cos(double(x)); }
template<class T> requires std::is_integral_v<T> double exp(T x) { return exp(double(x)); }
template<class A, class B>
	requires std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && (!std::is_same_v<A, B> || std::is_integral_v<A>)
double pow(A x, B y) { return pow(double(x), double(y)); }
template<class A, class B>
	requires std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && (!std::is_same_v<A, B> || std::is_integral_v<A>)
double atan2(A y, B x) { return atan2(double(y), double(x)); }
template<class A, class B>
	requires std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && (!std::is_same_v<A, B> || std::is_integral_v<A>)
double hypot(A x, B y) { return hypot(double(x), double(y)); }
}
