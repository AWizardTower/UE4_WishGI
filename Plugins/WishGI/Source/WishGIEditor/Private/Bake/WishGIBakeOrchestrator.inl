static int32 ExecuteBake(const FSettings& Settings)
{
	FTargetContext TargetContext;
	TargetContext.Source = Settings.TargetSource;
	TargetContext.PrecomputedSource = Settings.PrecomputedSource;
	BuildDirectionSamples(Settings.Directions, TargetContext.DirectionSamples);

	if (Settings.TargetSource == ETargetSource::PrecomputedVolume || Settings.TargetSource == ETargetSource::RayTrace)
	{
		if (Settings.MapPath.IsEmpty())
		{
			UE_LOG(LogWishGIBakeScene, Error, TEXT("-Map is required when -TargetSource=%s."), *TargetSourceToString(Settings.TargetSource));
			return 2;
		}

		TargetContext.World = LoadWorldForSampling(Settings.MapPath);
		if (!TargetContext.World)
		{
			UE_LOG(LogWishGIBakeScene, Error, TEXT("Failed to load map '%s' for %s sampling."), *Settings.MapPath, *TargetSourceToString(Settings.TargetSource));
			return 2;
		}

		if (Settings.TargetSource == ETargetSource::PrecomputedVolume)
		{
			LogPrecomputedDataSummary(TargetContext.World);
		}
		else if (Settings.TargetSource == ETargetSource::RayTrace)
		{
			PrepareLineTraceBackend(TargetContext.World, TargetContext);
		}
	}

	TArray<UWishGIMeshAssocAsset*> AssocAssets;
	GatherAssocAssets(Settings.AssocPath, AssocAssets);
	if (AssocAssets.Num() == 0)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("No mesh association assets found at '%s'."), *Settings.AssocPath);
		return 3;
	}

	FCreateProbeMapAssetResult ProbeMapAssetResult;
	if (!CreateOrResetProbeMapAsset(Settings, ProbeMapAssetResult))
	{
		return 4;
	}

	int32 RunningProbeStart = 0;
	FAccumulatedBakeStats BakeStats;

	for (UWishGIMeshAssocAsset* AssocAsset : AssocAssets)
	{
		SolveAssocAssetIntoProbeMap(Settings, TargetContext, AssocAsset, ProbeMapAssetResult.ProbeMapAsset, RunningProbeStart, BakeStats);
	}

	if (BakeStats.SolvedMeshCount <= 0 || RunningProbeStart <= 0)
	{
		UE_LOG(LogWishGIBakeScene, Error, TEXT("No mesh was solved. Check TargetSource and map lighting data."));
		return 5;
	}

	FinalizeProbeMapAsset(ProbeMapAssetResult.ProbeMapAsset, RunningProbeStart, BakeStats.RealSampleQueryAccum, BakeStats.RealSampleValidAccum, BakeStats.IterationAccum, BakeStats.ResidualAccum, BakeStats.SolvedChannelCount);

	if (!SaveProbeMapAsset(ProbeMapAssetResult))
	{
		return 6;
	}

	UE_LOG(LogWishGIBakeScene, Display, TEXT("Bake complete: '%s'"), *ProbeMapAssetResult.ObjectPath);
	UE_LOG(LogWishGIBakeScene, Display, TEXT("TargetSource=%s, AssocAssets=%d, SolvedMeshes=%d, SkippedMeshes=%d, TotalProbes=%d, SHOrder=%d, SuggestedProbeMap=%dx%d, AvgSolverIter=%d, AvgResidual=%.6f"),
		*TargetSourceToString(Settings.TargetSource),
		AssocAssets.Num(),
		BakeStats.SolvedMeshCount,
		BakeStats.SkippedMeshCount,
		ProbeMapAssetResult.ProbeMapAsset->TotalProbeCount,
		ProbeMapAssetResult.ProbeMapAsset->SHOrder,
		ProbeMapAssetResult.ProbeMapAsset->SuggestedProbeMapSize.X,
		ProbeMapAssetResult.ProbeMapAsset->SuggestedProbeMapSize.Y,
		ProbeMapAssetResult.ProbeMapAsset->SolverIterations,
		ProbeMapAssetResult.ProbeMapAsset->SolverResidual);

	if (Settings.TargetSource == ETargetSource::PrecomputedVolume)
	{
		UE_LOG(LogWishGIBakeScene, Display, TEXT("PrecomputedSource=%s"), *PrecomputedSourceToString(Settings.PrecomputedSource));
	}

	if (Settings.TargetSource == ETargetSource::PrecomputedVolume || Settings.TargetSource == ETargetSource::RayTrace)
	{
		UE_LOG(LogWishGIBakeScene, Display, TEXT("Real sampling stats: Query=%d, Valid=%d, ValidRatio=%.3f, FallbackVertices=%d"),
			BakeStats.RealSampleQueryAccum,
			BakeStats.RealSampleValidAccum,
			ProbeMapAssetResult.ProbeMapAsset->RealSampleValidRatio,
			BakeStats.RealSampleFallbackVertexAccum);
	}

	return 0;
}
