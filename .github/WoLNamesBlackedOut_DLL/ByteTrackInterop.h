#pragma once

#include "ByteTrack/BYTETracker.h"
#include "ByteTrack/Detection.h"
#include "ByteTrack/Track.h"

namespace WoLNamesBlackedOut::Core::ByteTrackInterop {

using Tracker = byte_track::BYTETracker<Detection, Track>;
using DetectionPtr = std::shared_ptr<Detection>;

} // namespace WoLNamesBlackedOut::Core::ByteTrackInterop
