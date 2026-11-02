// Multiplayer rooms: Newtonian gravity computed identically on every machine.
//
// The game's solver (Fft.cpp) is fast but not reproducible across machines:
// FFTW picks its algorithm by timing, each algorithm rounds differently, and
// the result arrives from a worker thread a frame later depending on timing.
// This solver is synchronous, uses its own fixed radix-2 FFT, builds its tables
// without the C maths library, and keeps to strict IEEE arithmetic (no
// fast-math, no fused multiply-add) whatever the build flags. Given the same
// mass and walls it produces the same forces, bit for bit, everywhere.
//
// The pragmas below turn fast-math off for this file. They are not enough
// against fused multiply-add: clang's -ffp-contract=fast (part of -ffast-math)
// ignores them, and ARM64 fuses. Every product that is then added is therefore
// passed through Round(), which makes the compiler hold the rounded product.
#if defined(__clang__)
# pragma float_control(precise, on)
# pragma clang fp contract(off)
#elif defined(__GNUC__)
# pragma GCC optimize("no-fast-math", "fp-contract=off")
#elif defined(_MSC_VER)
# pragma float_control(precise, on)
# pragma fp_contract(off)
#endif

#include "Gravity.h"
#include "SimulationConfig.h"
#include <cmath>
#include <cstring>
#include <utility>
#include <memory>
#include <vector>

namespace
{
// Twice the cell grid, rounded up to powers of two: a mass's pull reaches the
// whole world without wrapping around (the game's solver wraps slightly).
constexpr int Width = 512, Height = 256;
static_assert(Width >= 2 * CELLS.X - 1 && Height >= 2 * CELLS.Y - 1);
static_assert((Width & (Width - 1)) == 0 && (Height & (Height - 1)) == 0);

struct Complex
{
	float re, im;
};

// The value, rounded to its type now: an empty asm statement that claims to use
// and change it in a floating-point register, so a multiply before it cannot be
// fused with an add after it. No instructions are emitted. (MSVC does not fuse
// on x64 under float_control(precise), so it needs nothing.)
template<class T>
inline T Round(T x)
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

Complex Multiply(Complex a, Complex b)
{
	return { Round(a.re * b.re) - Round(a.im * b.im), Round(a.re * b.im) + Round(a.im * b.re) };
}

// sin and cos of a in [-pi/4, pi/4] by Taylor series in double precision: the
// same operations, so the same bits, on every machine (unlike sin() and cos()).
void SinCos(double a, double &s, double &c)
{
	// Horner form, innermost first: s = a (1 - a2/6 (1 - a2/20 (...))), likewise c.
	double a2 = a * a, ts = 1, tc = 1;
	for (double d : { 210., 156., 110., 72., 42., 20., 6. })
		ts = 1 - Round(a2 / d * ts);
	for (double d : { 182., 132., 90., 56., 30., 12., 2. })
		tc = 1 - Round(a2 / d * tc);
	s = a * ts;
	c = tc;
}

// exp(-2 pi i k / n) for k < n / 2, from octant-sized angles only.
std::vector<Complex> Twiddles(int n)
{
	constexpr double pi = 3.14159265358979323846;
	std::vector<Complex> out(n / 2);
	for (int k = 0; k < n / 2; ++k)
	{
		// k = q * n/4 + r: rotate by quarter turns, and keep the angle within an octant.
		int q = k / (n / 4), r = k % (n / 4);
		double s, c;
		if (r <= n / 8)
			SinCos(2 * pi * r / n, s, c);
		else
		{
			SinCos(2 * pi * (n / 4 - r) / n, c, s);
		}
		// cos and sin of 2 pi k / n
		double cs = q ? -s : c, sn = q ? c : s;
		out[k] = { float(cs), float(-sn) };
	}
	return out;
}

void Transform(Complex *a, int n, const std::vector<Complex> &twiddles, bool inverse)
{
	for (int i = 1, j = 0; i < n; ++i)
	{
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(a[i], a[j]);
	}
	for (int length = 2; length <= n; length <<= 1)
	{
		int half = length / 2, step = n / length;
		for (int i = 0; i < n; i += length)
		{
			for (int j = 0; j < half; ++j)
			{
				auto w = twiddles[j * step];
				if (inverse)
					w.im = -w.im;
				auto v = Multiply(a[i + j + half], w);
				auto u = a[i + j];
				a[i + j] = { u.re + v.re, u.im + v.im };
				a[i + j + half] = { u.re - v.re, u.im - v.im };
			}
		}
	}
}

class Solver
{
	std::vector<Complex> rowTwiddles = Twiddles(Width), columnTwiddles = Twiddles(Height);
	std::vector<Complex> kernel, grid, column;
	GravityInput last;
	bool haveLast = false;

	// Rows below `rows` may be nonzero; the others stay zero and are skipped.
	void Forward2d(int rows)
	{
		for (int y = 0; y < rows; ++y)
			Transform(&grid[y * Width], Width, rowTwiddles, false);
		Columns(false);
	}
	void Columns(bool inverse)
	{
		for (int x = 0; x < Width; ++x)
		{
			for (int y = 0; y < Height; ++y)
				column[y] = grid[y * Width + x];
			Transform(column.data(), Height, columnTwiddles, inverse);
			for (int y = 0; y < Height; ++y)
				grid[y * Width + x] = column[y];
		}
	}

public:
	Solver() : grid(Width * Height), column(Height)
	{
		// The pull of a unit mass at displacement d (target minus source):
		// -G d / |d|^3, as the game's kernel. Real part x, imaginary part y, so
		// one transform carries both (the transform is linear).
		std::fill(grid.begin(), grid.end(), Complex{ 0, 0 });
		for (int dy = -(CELLS.Y - 1); dy < CELLS.Y; ++dy)
		{
			for (int dx = -(CELLS.X - 1); dx < CELLS.X; ++dx)
			{
				if (!dx && !dy)
					continue;
				double r2 = double(dx) * dx + double(dy) * dy;
				double cube = r2 * std::sqrt(r2); // sqrt is exactly rounded everywhere
				auto &k = grid[((dy + Height) % Height) * Width + (dx + Width) % Width];
				k = { float(-double(M_GRAV) * dx / cube), float(-double(M_GRAV) * dy / cube) };
			}
		}
		Forward2d(Height);
		kernel = grid;
	}

	void Solve(GravityOutput &out, const GravityInput &in, bool forceRecalc)
	{
		// The same input gives the same output: skipping is only a shortcut.
		if (!forceRecalc && haveLast &&
		    !std::memcmp(&last.mass[{ 0, 0 }], &in.mass[{ 0, 0 }], NCELL * sizeof(float)) &&
		    !std::memcmp(&last.mask[{ 0, 0 }], &in.mask[{ 0, 0 }], NCELL * sizeof(uint32_t)))
			return;
		last = in;
		haveLast = true;
		std::fill(grid.begin(), grid.end(), Complex{ 0, 0 });
		for (auto p : CELLS.OriginRect())
			grid[p.Y * Width + p.X].re = in.mask[p] ? in.mass[p] : 0.f;
		Forward2d(CELLS.Y);
		// Convolution theorem: multiply the spectra.
		for (int i = 0; i < Width * Height; ++i)
		{
			grid[i] = Multiply(grid[i], kernel[i]);
		}
		Columns(true);
		for (int y = 0; y < CELLS.Y; ++y)
			Transform(&grid[y * Width], Width, rowTwiddles, true);
		constexpr float scale = 1.f / (Width * Height); // a power of two: exact
		for (auto p : CELLS.OriginRect())
		{
			auto force = grid[p.Y * Width + p.X];
			out.forceX[p] = in.mask[p] ? force.re * scale : 0.f;
			out.forceY[p] = in.mask[p] ? force.im * scale : 0.f;
		}
	}
};

struct DeterministicGravityImpl : public Gravity
{
	std::unique_ptr<Solver> solver;
	DeterministicGravityImpl()
	{
		deterministic = true;
	}
};
}

void Gravity::ExchangeDeterministic(GravityOutput &gravOut, GravityInput &gravIn, bool forceRecalc)
{
	auto *self = static_cast<DeterministicGravityImpl *>(this);
	if (!self->solver)
		self->solver = std::make_unique<Solver>(); // builds the kernel spectrum, once
	self->solver->Solve(gravOut, gravIn, forceRecalc);
}

GravityPtr Gravity::CreateDeterministic()
{
	return GravityPtr(new DeterministicGravityImpl());
}

void Gravity::DestroyDeterministic(Gravity *gravity)
{
	delete static_cast<DeterministicGravityImpl *>(gravity);
}
