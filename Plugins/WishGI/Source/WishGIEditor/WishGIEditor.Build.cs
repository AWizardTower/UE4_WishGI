// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WishGIEditor : ModuleRules
{
	public WishGIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"RenderCore",
				"RHI",
				"UnrealEd"
			}
		);
	}
}
