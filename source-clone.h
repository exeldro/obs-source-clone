#pragma once

#include <obs-module.h>
#include "version.h"
#include <util/threading.h>
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
#include <util/deque.h>
#define circlebuf_peek_front deque_peek_front
#define circlebuf_peek_back deque_peek_back
#define circlebuf_push_front deque_push_front
#define circlebuf_push_back deque_push_back
#define circlebuf_pop_front deque_pop_front
#define circlebuf_pop_back deque_pop_back
#define circlebuf_init deque_init
#define circlebuf_free deque_free
#define circlebuf_data deque_data
#else
#include <util/circlebuf.h>
#endif

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
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque audio_data[MAX_AUDIO_CHANNELS];
	struct deque audio_frames;
	struct deque audio_timestamps;
#else
	struct circlebuf audio_data[MAX_AUDIO_CHANNELS];
	struct circlebuf audio_frames;
	struct circlebuf audio_timestamps;
#endif
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
	bool no_filter;
};
