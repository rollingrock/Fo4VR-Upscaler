# Ratio depth resolve regression

This offline harness compiles and executes `src/Upscaler/DepthResolveShader.h` itself on
both the local D3D11 hardware adapter and WARP. It creates the same typeless D24S8 source,
R24 depth SRV, writable D24S8 destination, and ALWAYS/REPLACE state as the production pass.

Run from PowerShell with the game and mod manager closed:

```powershell
& .\scratch\cell-continuity\source\tools\ratio-depth-tests\run.ps1
```

The suite checks a `.57` packed extent, the vendor Quality extent snapped by
`round(output / 1.5)`, per-eye separation,
and zero stencil over the entire output. The third case deliberately applies stale default
dimensions to `.57` pixels and must produce depth mismatches; it proves the regression
test distinguishes the reported stale-geometry failure from a working resolve.
