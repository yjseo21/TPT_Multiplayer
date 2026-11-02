#include "Gravity.h"

void Gravity::Exchange(GravityOutput &gravOut, GravityInput &gravIn, bool forceRecalc)
{
	if (deterministic)
	{
		ExchangeDeterministic(gravOut, gravIn, forceRecalc);
		return;
	}
}

GravityPtr Gravity::Create()
{
	return GravityPtr(new Gravity());
}

void GravityDeleter::operator ()(Gravity *ptr) const
{
	if (ptr->deterministic)
	{
		Gravity::DestroyDeterministic(ptr);
		return;
	}
	delete ptr;
}
