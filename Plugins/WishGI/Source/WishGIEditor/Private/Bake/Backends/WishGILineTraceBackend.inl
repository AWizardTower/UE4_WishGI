static void PrepareLineTraceBackend(UWorld* World, FTargetContext& InOutTargetContext)
{
	GatherRayTraceLights(World, InOutTargetContext);
}
