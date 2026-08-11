#pragma once

#include <memory>

#include "ByteTrack/Detection.h"

namespace WoLNamesBlackedOut::Core::ByteTrackInterop {

class Track {
public:
	using DetectionPtr = std::shared_ptr<class Detection>;

	explicit Track(const DetectionPtr& det)
		: track_id_(0) {
		if (det) {
			detection_ = det->getDetection();
		}
	}

	void update(const DetectionPtr& det) {
		if (!det) {
			return;
		}

		detection_ = det->getDetection();
	}

	void set_track_id(size_t id) {
		track_id_ = id;
	}

	size_t track_id() const {
		return track_id_;
	}

	WoLNamesBlackedOut::Core::Detection getDetection() const {
		return detection_;
	}

private:
	WoLNamesBlackedOut::Core::Detection detection_{};
	size_t track_id_;
};

} // namespace WoLNamesBlackedOut::Core::ByteTrackInterop
