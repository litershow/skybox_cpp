# SkyboxChangerCpp

Metamod:Source plugin for CS2 that changes the server skybox from console or chat commands.

This is a native C++ baseline project wired for AMBuild and GitHub Actions. It focuses on:

- simple Metamod/CS2 project structure;
- stable global skybox switching through `sv_skyname`;
- support for passing either a raw sky name or a full material path.

## Commands

From server console or chat:

```txt
!skyset materials/skybox/sky_hr_aztec_02.vmat
!skyset sky_hr_aztec_02
!skyreset
!skystatus
!skyreload
```

Console aliases:

```txt
sky_set <skyname-or-path>
sky_reset
sky_status
sky_reload
```

## Notes

- If you pass a full path like `materials/skybox/fps.vmat`, the plugin converts it to `fps` for `sv_skyname`.
- CS2 may require a map reload for the visual sky to fully refresh. `!skyreload` forces `changelevel` to the current map.
- This first native version is global, not per-player.

## Build

The repository is prepared for the same AMBuild workflow style as your reference project in `132132`.

Required externals in CI:

- `metamod-source`
- `hl2sdk-cs2`
- `AMBuild 2.2+`

## Output

Packaging places files into:

```txt
addons/SkyboxChangerCpp/SkyboxChangerCpp.so
addons/metamod/SkyboxChangerCpp.vdf
```
