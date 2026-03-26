#pragma once

#include "Modules/ModuleManager.h"

class FWishGIEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

