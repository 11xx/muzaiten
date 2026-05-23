# MusicBee Inference Notes

Local reference files:

- `musicbee-wine/MusicBee_Portable/AppData/MusicBee3Settings.ini`
- `musicbee-wine/MusicBee_Portable/Configuration.xml`
- `musicbee-wine/MusicBee_Portable/AppData/ArtworkInfo.dat`
- `musicbee-wine/MusicBee_Portable/AppData/AlbumCoverHashes.dat`
- `musicbee-wine/MusicBee_Portable/MusicBee.exe`

The versionless portable install is treated as the primary reference for the
user's current behavior. The `3.3.7491` backup is only a secondary comparison
point when settings conflict.

Observed settings:

- `TagStoreRatings=true`
- `TagDisableHalfRatings=false`
- `NavigatorArtworkShowTracksRating=true`
- `TagArtworkNamingRules` includes `Cover.jpg`
- `AppData/InternalCache/AlbumCovers` contains thousands of cached images

Executable strings include whole-star and half-star rating commands. That
supports a display and sorting model based on values from `0` to `100` in
increments of `10`, with unset ratings distinct from explicit zero.

muzaiten uses these observations as behavioral guidance only. It does not copy
MusicBee code, assets, or branding.

