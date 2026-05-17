$files = @(
    'src/d3d9/d3d9_device.cpp',
    'src/d3d9/d3d9_device.h',
    'src/d3d9/d3d9_war3_shadow.cpp',
    'src/d3d9/d3d9_war3_pipeline.cpp',
    'src/d3d9/war3/hooks/war3_hook_shadow.cpp',
    'src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp',
    'src/d3d9/war3/tools/war3_control_plane.cpp',
    'src/d3d9/war3/core/war3_internal_test_config.h',
    'src/d3d9/war3/model/war3_model_hook.cpp',
    'src/d3d9/war3/render/war3_current_draw_contract.cpp'
)
Write-Host "=== File line counts (top files in shadow/render path) ==="
foreach ($f in $files) {
    if (Test-Path $f) {
        $n = (Get-Content $f).Length
        $kb = [math]::Round((Get-Item $f).Length / 1KB, 1)
        $name = $f
        Write-Host ("  {0,-65} {1,8} lines  {2,7} KB" -f $name, $n, $kb)
    }
}
