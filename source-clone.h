#pragma once

#include <obs-module.h>
#include "version.h"
#include <util/threading.h>
#include <util/circlebuf.h>

enum clone_type {
	CLONE_SOURCE,
	CLONE_CURRENT_SCENE,
	CLONE_PREVIOUS_SCENE,
};

struct source_clone {
	obs_source_t *source;
	enum clone_type clone_type;
	obs_weak_source_t *clone;
	obs_weak_source_t *current_scene;
	struct audio_wrapper_info *audio_wrapper;
	struct circlebuf audio_data[MAX_AUDIO_CHANNELS];
	struct circlebuf audio_frames;
	struct circlebuf audio_timestamps;
	uint64_t audio_ts;
	size_t num_channels;
	pthread_mutex_t audio_mutex;
	gs_texrender_t *render;
	bool processed_frame;
	bool audio_enabled;
	uint8_t buffer_frame;
	uint32_t cx;
	uint32_t cy;
	uint32_t source_cx;
	uint32_t source_cy;
	enum gs_color_space space;
	bool rendering;
	bool active_clone;
};
