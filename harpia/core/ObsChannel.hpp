#pragma once

// "Is this source what that output channel shows?" -- asked without leaking.
//
// obs_get_output_source() hands back a NEW reference (obs_view_get_source
// takes one), so comparing its result and dropping it leaks the channel's
// source. Leaked, a capture scene outlived stopCapture() with its screen
// capture still inside it -- one more per capture rebuild -- until
// obs_shutdown() destroyed every leftover source in hash order: the capture
// first, then the scene, whose teardown released the capture it no longer
// had. That was the access violation in obs_source_release from
// scene_destroy -> remove_all_items.
//
// Every question about a channel goes through here; the unit suite checks
// that nothing calls obs_get_output_source() directly.

#include <obs.h>

namespace harpia {

inline bool channelShows(uint32_t channel, const obs_source_t *source)
{
	obs_source_t *current = obs_get_output_source(channel);
	const bool same = current && current == source;
	obs_source_release(current); // the reference the getter took; null is fine
	return same;
}

} // namespace harpia
