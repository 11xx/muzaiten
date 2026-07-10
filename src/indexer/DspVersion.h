#pragma once

// The active scalar-analysis version, split out of Dsp.h so the app-side
// read path (FeatureStore) can judge feature-row freshness against the
// version this binary would write WITHOUT linking analyzer code. This
// matters because meta.dsp_version in a features.sqlite only says which
// analyzer last opened the store — an untouched all-old store under a newer
// binary still needs its rows recognized as stale.
//
// Bump whenever any extraction algorithm or constant changes; stored per
// row in the features table so stale rows can be recomputed selectively.
namespace Dsp {

inline constexpr const char *kDspVersion = "muzaiten-dsp-v2";

} // namespace Dsp
