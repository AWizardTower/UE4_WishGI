struct FAccumulatedBakeStats
{
	int32 RealSampleQueryAccum = 0;
	int32 RealSampleValidAccum = 0;
	int32 RealSampleFallbackVertexAccum = 0;
	int32 IterationAccum = 0;
	double ResidualAccum = 0.0;
	int32 SolvedChannelCount = 0;
	int32 SolvedMeshCount = 0;
	int32 SkippedMeshCount = 0;
};

static bool SolveAssocAssetIntoProbeMap(const FSettings& Settings, const FTargetContext& TargetContext, UWishGIMeshAssocAsset* AssocAsset, UWishGIProbeMapAsset* ProbeMapAsset, int32& RunningProbeStart, FAccumulatedBakeStats& InOutStats)
{
	if (!AssocAsset)
	{
		return false;
	}

	const int32 MeshProbeCount = FMath::Clamp(AssocAsset->ProbeCount, 1, 256);
	FTargetStats TargetStats;

	FWishGIProbeMeshRange MeshRange;
	MeshRange.SourceMesh = AssocAsset->SourceMesh;
	MeshRange.ProbeStart = RunningProbeStart;
	MeshRange.ProbeCount = MeshProbeCount;
	ProbeMapAsset->MeshRanges.Add(MeshRange);

	if (Settings.TargetSource == ETargetSource::RayTrace)
	{
		FSolvedProbeSHSignals SHSignals;
		if (!SolveProbeSHSignalsFromRayTrace(AssocAsset, MeshProbeCount, Settings.Lambda, TargetContext, Settings.SHOrder, SHSignals, TargetStats))
		{
			ProbeMapAsset->MeshRanges.Pop();
			++InOutStats.SkippedMeshCount;
			UE_LOG(LogWishGIBakeScene, Warning, TEXT("Skipping '%s': failed to build RT SH targets/solve."), *AssocAsset->GetPathName());
			return false;
		}

		InOutStats.RealSampleQueryAccum += TargetStats.QueryCount;
		InOutStats.RealSampleValidAccum += TargetStats.ValidCount;
		InOutStats.RealSampleFallbackVertexAccum += TargetStats.FallbackVertexCount;
		InOutStats.IterationAccum += SHSignals.IterationSum;
		InOutStats.ResidualAccum += SHSignals.ResidualSum;
		InOutStats.SolvedChannelCount += SHSignals.SolveCount;

		for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
		{
			ProbeMapAsset->ProbeRecords.Add(BuildProbeRecordFromSHCoefficients(SHSignals, ProbeIndex));
		}
	}
	else
	{
		FSolvedSignals Signals;
		if (!SolveProbeSignals(AssocAsset, MeshProbeCount, Settings.Lambda, TargetContext, Signals, TargetStats))
		{
			ProbeMapAsset->MeshRanges.Pop();
			++InOutStats.SkippedMeshCount;
			UE_LOG(LogWishGIBakeScene, Warning, TEXT("Skipping '%s': failed to build targets/solve."), *AssocAsset->GetPathName());
			return false;
		}

		InOutStats.RealSampleQueryAccum += TargetStats.QueryCount;
		InOutStats.RealSampleValidAccum += TargetStats.ValidCount;
		InOutStats.RealSampleFallbackVertexAccum += TargetStats.FallbackVertexCount;

		auto AccStats = [&InOutStats](const FSolveStats& Stats)
		{
			if (Stats.Iterations > 0)
			{
				InOutStats.IterationAccum += Stats.Iterations;
				InOutStats.ResidualAccum += Stats.Residual;
				InOutStats.SolvedChannelCount += 1;
			}
		};

		AccStats(Signals.StatsR);
		AccStats(Signals.StatsG);
		AccStats(Signals.StatsB);

		const uint32 MeshHash = GetTypeHash(AssocAsset->GetPathName());
		for (int32 ProbeIndex = 0; ProbeIndex < MeshProbeCount; ++ProbeIndex)
		{
			const float R = Signals.R.IsValidIndex(ProbeIndex) ? Signals.R[ProbeIndex] : 0.0f;
			const float G = Signals.G.IsValidIndex(ProbeIndex) ? Signals.G[ProbeIndex] : 0.0f;
			const float B = Signals.B.IsValidIndex(ProbeIndex) ? Signals.B[ProbeIndex] : 0.0f;
			ProbeMapAsset->ProbeRecords.Add(BuildProbeRecord(R, G, B, Settings.SHOrder, MeshHash, ProbeIndex));
		}
	}

	RunningProbeStart += MeshProbeCount;
	++InOutStats.SolvedMeshCount;
	return true;
}
