#pragma once
#include <obs.h>

struct audio_wrapper_info {
	obs_source_t *source;
	DARRAY(struct source_clone *) clones;
	uint32_t channel;
};

extern struct obs_source_info audio_wrapper_source;

struct audio_wrapper_info *audio_wrapper_get(bool create);

void audio_wrapper_remove(struct audio_wrapper_info *audio_wrapper,
			  struct source_clone *clone);

void audio_wrapper_add(struct audio_wrapper_info *audio_wrapper,
		       struct source_clone *clone);
