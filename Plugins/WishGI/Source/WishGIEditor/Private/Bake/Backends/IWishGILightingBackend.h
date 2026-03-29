#pragma once

namespace WishGIBakeScene
{
struct FTargetContext;

class IWishGILightingBackend
{
public:
	virtual ~IWishGILightingBackend() = default;
	virtual bool Prepare(FTargetContext& TargetContext) = 0;
};
}
