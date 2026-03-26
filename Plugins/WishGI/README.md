# WishGI Offline Tools (MVP)

This plugin provides two commandlets:

1. `WishGIMeshPrep`
2. `WishGIBakeScene`

## MeshPrep

Generates per-vertex probe associations from static mesh render geometry.

Current MVP flow:
- Area-weighted mesh surface sampling
- KMeans/KMedoids-like probe center initialization
- Top-2 inverse-distance probe weights per sample
- Barycentric transfer from samples to triangle vertices
- Top-2 quantized probe indices/weights per vertex

Example:

```bat
UE4Editor-Cmd.exe YourProject.uproject -run=WishGIMeshPrep -MeshPath=/Game -OutPath=/Game/WishGI/MeshAssoc -SampleDensity=100 -ProbeCount=32 -AssocPerVertex=2 -LOD=0 -KMeansIters=8 -Seed=1337 -Overwrite
```

## BakeScene

Builds a scene-level probe map asset from mesh association assets.

Current MVP flow:
- Read mesh association assets
- Aggregate per-probe coverage from vertex associations
- Generate deterministic SH placeholder records per probe
- Build mesh probe ranges and suggested probe map size

Example:

```bat
UE4Editor-Cmd.exe YourProject.uproject -run=WishGIBakeScene -AssocPath=/Game/WishGI/MeshAssoc -OutPath=/Game/WishGI/Bake -AssetName=WishGI_ProbeMap -SHOrder=2 -Directions=192 -Lambda=0.1 -Overwrite
```

## Notes

- This is an MVP scaffold focused on offline pipeline connectivity.
- Probe visibility graph, A*/Dijkstra distance metric, and full light fitting solver are not implemented yet.
- Runtime shader integration is out of scope of this plugin stage.
