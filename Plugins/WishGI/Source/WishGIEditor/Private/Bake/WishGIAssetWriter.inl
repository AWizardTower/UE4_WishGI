struct FCreateProbeMapAssetResult
{
	UWishGIProbeMapAsset* ProbeMapAsset = nullptr;
	FString ObjectPath;
	bool bIsNewAsset = false;
};

static bool CreateOrResetProbeMapAsset(const FSettings& Settings, FCreateProbeMapAssetResult& OutResult)
{
	FString TrimmedOutPath = Settings.OutPath;
	TrimmedOutPath.RemoveFromEnd(TEXT("/"));
	const FString PackageName = FString::Printf(TEXT("%s/%s"), *TrimmedOutPath, *Settings.AssetName);
	OutResult.ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Settings.AssetName);

	UWishGIProbeMapAsset* ProbeMapAsset = LoadObject<UWishGIProbeMapAsset>(nullptr, *OutResult.ObjectPath);
	if (!ProbeMapAsset)
	{
		UPackage* Package = CreatePackage(*PackageName);
		ProbeMapAsset = NewObject<UWishGIProbeMapAsset>(Package, *Settings.AssetName, RF_Public | RF_Standalone);
		OutResult.bIsNewAsset = true;
	}
	else if (!Settings.bOverwrite)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Asset '%s' already exists. Use -Overwrite to replace."), *OutResult.ObjectPath);
		return false;
	}

	ProbeMapAsset->SourceMapPath = Settings.MapPath;
	ProbeMapAsset->TargetSource = TargetSourceToString(Settings.TargetSource);
	ProbeMapAsset->SHOrder = Settings.SHOrder;
	ProbeMapAsset->DirectionCount = Settings.Directions;
	ProbeMapAsset->SHCoefficientCount = (Settings.TargetSource == ETargetSource::RayTrace) ? GetSHCoefficientCount(Settings.SHOrder) : 0;
	ProbeMapAsset->Lambda = Settings.Lambda;
	ProbeMapAsset->ProbeMapTexture.Reset();
	ProbeMapAsset->ProbeRecords.Reset();
	ProbeMapAsset->MeshRanges.Reset();
	ProbeMapAsset->SolverIterations = 0;
	ProbeMapAsset->SolverResidual = 0.0f;
	ProbeMapAsset->RealSampleQueryCount = 0;
	ProbeMapAsset->RealSampleValidCount = 0;
	ProbeMapAsset->RealSampleValidRatio = 0.0f;

	OutResult.ProbeMapAsset = ProbeMapAsset;
	return true;
}

static void FinalizeProbeMapAsset(UWishGIProbeMapAsset* ProbeMapAsset, int32 TotalProbes, int32 RealSampleQueryAccum, int32 RealSampleValidAccum, int32 IterationAccum, double ResidualAccum, int32 SolvedChannelCount)
{
	ProbeMapAsset->TotalProbeCount = TotalProbes;
	ProbeMapAsset->SuggestedProbeMapSize = ComputeProbeMapSize(ProbeMapAsset->TotalProbeCount, ProbeMapAsset->SHOrder);
	ProbeMapAsset->RealSampleQueryCount = RealSampleQueryAccum;
	ProbeMapAsset->RealSampleValidCount = RealSampleValidAccum;
	ProbeMapAsset->RealSampleValidRatio = (RealSampleQueryAccum > 0)
		? static_cast<float>(static_cast<double>(RealSampleValidAccum) / static_cast<double>(RealSampleQueryAccum))
		: 0.0f;

	if (SolvedChannelCount > 0)
	{
		ProbeMapAsset->SolverIterations = FMath::RoundToInt(static_cast<float>(IterationAccum) / static_cast<float>(SolvedChannelCount));
		ProbeMapAsset->SolverResidual = static_cast<float>(ResidualAccum / static_cast<double>(SolvedChannelCount));
	}
}

static bool SaveProbeMapAsset(const FCreateProbeMapAssetResult& Result)
{
	if (Result.bIsNewAsset)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.AssetCreated(Result.ProbeMapAsset);
	}

	if (!SaveAssetPackage(Result.ProbeMapAsset->GetOutermost(), Result.ProbeMapAsset))
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("Failed to save '%s'."), *Result.ObjectPath);
		return false;
	}

	return true;
}
