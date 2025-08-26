#include <obs-module.h>
#include "audio-wrapper.h"
#include "source-clone.h"

struct audio_wrapper_info *audio_wrapper_get(bool create)
{
	for (uint32_t i = MAX_CHANNELS - 1; i > 0; i--) {
		obs_source_t *source = obs_get_output_source(i);
		if (!source)
			continue;
		if (strcmp(obs_source_get_unversioned_id(source),
			   audio_wrapper_source.id) == 0) {
			struct audio_wrapper_info *aw =
				obs_obj_get_data(source);
			aw->channel = i;
			obs_source_release(source);
			return aw;
		}
		obs_source_release(source);
	}
	if (!create)
		return NULL;
	obs_source_t *aws = obs_source_create_private(
		audio_wrapper_source.id, audio_wrapper_source.id, NULL);
	struct audio_wrapper_info *aw = obs_obj_get_data(aws);

	for (uint32_t i = MAX_CHANNELS - 1; i > 0; i--) {
		obs_source_t *source = obs_get_output_source(i);
		if (source) {
			obs_source_release(source);
			continue;
		}
		obs_set_output_source(i, aws);
		aw->channel = i;
		obs_source_release(aws);
		return aw;
	}
	obs_source_release(aws);
	return NULL;
}

void audio_wrapper_remove(struct audio_wrapper_info *audio_wrapper,
			  struct source_clone *clone)
{
	da_erase_item(audio_wrapper->clones, &clone);
	if (audio_wrapper->clones.num)
		return;
	obs_source_t *s = obs_get_output_source(audio_wrapper->channel);
	if (s) {
		if (s == audio_wrapper->source) {
			obs_set_output_source(audio_wrapper->channel, NULL);
			return;
		}
		obs_source_release(s);
	}
	for (uint32_t i = MAX_CHANNELS - 1; i > 0; i--) {
		obs_source_t *source = obs_get_output_source(i);
		if (!source)
			continue;
		if (source == audio_wrapper->source) {
			obs_set_output_source(audio_wrapper->channel, NULL);
			return;
		}
		obs_source_release(source);
	}
}

void audio_wrapper_add(struct audio_wrapper_info *audio_wrapper,
		       struct source_clone *clone)
{
	da_push_back(audio_wrapper->clones, &clone);
}

const char *audio_wrapper_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "source_clone_audio_wrapper";
}

void *audio_wrapper_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct audio_wrapper_info *audio_wrapper =
		bzalloc(sizeof(struct audio_wrapper_info));
	audio_wrapper->source = source;
	return audio_wrapper;
}

void audio_wrapper_destroy(void *data)
{
	struct audio_wrapper_info *aw = (struct audio_wrapper_info *)data;
	for (size_t i = 0; i < aw->clones.num; i++) {
		struct source_clone *clone = aw->clones.array[i];
		if (clone->audio_wrapper == aw)
			clone->audio_wrapper = NULL;
	}
	da_free(aw->clones);
	bfree(data);
}

bool audio_wrapper_render(void *data, uint64_t *ts_out,
			  struct obs_source_audio_mix *audio, uint32_t mixers,
			  size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(ts_out);
	UNUSED_PARAMETER(audio);
	UNUSED_PARAMETER(mixers);
	UNUSED_PARAMETER(sample_rate);
	struct audio_wrapper_info *aw = (struct audio_wrapper_info *)data;
	for (size_t i = 0; i < aw->clones.num; i++) {
		struct source_clone *clone = aw->clones.array[i];
		obs_source_t *source = obs_weak_source_get_source(clone->clone);
		if (!source)
			continue;
		if (obs_source_audio_pending(source)) {
			obs_source_release(source);
			continue;
		}

		struct obs_source_audio_mix child_audio;
		obs_source_get_audio_mix(source, &child_audio);
		uint64_t timestamp = obs_source_get_audio_timestamp(source);
		for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
			if ((mixers & (1 << mix)) == 0)
				continue;
			pthread_mutex_lock(&clone->audio_mutex);
			uint32_t frames = AUDIO_OUTPUT_FRAMES;
			for (size_t j = 0; j < channels; j++) {
				deque_push_back(
					&clone->audio_data[j],
					child_audio.output[mix].data[j],
					frames * sizeof(float));
			}
			deque_push_back(&clone->audio_frames, &frames,
					    sizeof(frames));
			deque_push_back(&clone->audio_timestamps,
					    &timestamp, sizeof(timestamp));
			pthread_mutex_unlock(&clone->audio_mutex);
			break;
		}
		obs_source_release(source);
	}
	return false;
}

static void audio_wrapper_enum_sources(void *data,
				       obs_source_enum_proc_t enum_callback,
				       void *param, bool active)
{
	UNUSED_PARAMETER(active);
	struct audio_wrapper_info *aw = (struct audio_wrapper_info *)data;
	for (size_t i = 0; i < aw->clones.num; i++) {
		struct source_clone *clone = aw->clones.array[i];
		obs_source_t *source = obs_weak_source_get_source(clone->clone);
		if (!source)
			continue;

		enum_callback(aw->source, source, param);

		obs_source_release(source);
	}
}

void audio_wrapper_enum_active_sources(void *data,
				       obs_source_enum_proc_t enum_callback,
				       void *param)
{
	audio_wrapper_enum_sources(data, enum_callback, param, true);
}

void audio_wrapper_enum_all_sources(void *data,
				    obs_source_enum_proc_t enum_callback,
				    void *param)
{
	audio_wrapper_enum_sources(data, enum_callback, param, false);
}

struct obs_source_info audio_wrapper_source = {
	.id = "source_clone_audio_wrapper_source",
	.type = OBS_SOURCE_TYPE_SCENE,
	.output_flags = OBS_SOURCE_COMPOSITE | OBS_SOURCE_CAP_DISABLED,
	.get_name = audio_wrapper_get_name,
	.create = audio_wrapper_create,
	.destroy = audio_wrapper_destroy,
	.audio_render = audio_wrapper_render,
	.enum_active_sources = audio_wrapper_enum_active_sources,
	.enum_all_sources = audio_wrapper_enum_all_sources,
};
