#pragma once

#include "CoreTypes.h"
#include "ByteTrack/Rect.h"

#include <algorithm>

namespace WoLNamesBlackedOut::Core::ByteTrackInterop {

class Detection {
public:
	explicit Detection(const WoLNamesBlackedOut::Core::Detection& detection)
		: detection_(detection) {
	}

	byte_track::TlwhRect rect() const {
		const float left = detection_.x1;
		const float top = detection_.y1;
		const float width = std::max(0.0f, detection_.x2 - detection_.x1);
		const float height = std::max(0.0f, detection_.y2 - detection_.y1);
		return byte_track::TlwhRect(top, left, width, height);
	}

	float score() const {
		return detection_.score;
	}

	const WoLNamesBlackedOut::Core::Detection& getDetection() const {
		return detection_;
	}

private:
	WoLNamesBlackedOut::Core::Detection detection_;
};

} // namespace WoLNamesBlackedOut::Core::ByteTrackInterop
