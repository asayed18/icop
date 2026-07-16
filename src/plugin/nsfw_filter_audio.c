/*****************************************************************************
 * nsfw_filter_audio.c: extracted from nsfw_filter.c
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
# include <wchar.h>
#else
# include <pthread.h>
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_input.h>
#include <vlc_aout.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>
#include <vlc_variables.h>

#include "nsfw_filter.h"
#include "nsfw_filter_internal.h"

static input_thread_t *FindInputThread(vlc_object_t *obj)
{
    while (obj != NULL) {
        const struct vlc_common_members *members =
            (const struct vlc_common_members *)obj;

        if (members->object_type != NULL &&
            strncmp(members->object_type, "input", 5) == 0) {
            return (input_thread_t *)obj;
        }
        obj = members->parent;
    }

    return NULL;
}

bool GetInputMediaTimeMs(filter_t *filter, uint64_t *time_ms)
{
    vlc_input_control_fn input_control = NULL;
    input_thread_t *input;
    int64_t input_time = 0;
    float configured_start;
    bool found = false;

    if (filter == NULL || time_ms == NULL)
        return false;
    *time_ms = 0;
    input = FindInputThread((vlc_object_t *)filter);
    LoadVlcPlaybackAccessors(&input_control, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL);
    if (input != NULL && input_control != NULL &&
        input_control(input, INPUT_GET_TIME, &input_time) == VLC_SUCCESS &&
        input_time >= 0) {
        *time_ms = (uint64_t)input_time * 1000 / CLOCK_FREQ;
        found = true;
    }
    configured_start = GetVlcConfigFloat(filter, "start-time", 0.0f);
    if (configured_start > 0.0f) {
        uint64_t configured_start_ms =
            (uint64_t)(configured_start * 1000.0f + 0.5f);
        if (configured_start_ms > *time_ms)
            *time_ms = configured_start_ms;
        found = true;
    }
    return found;
}

static playlist_t *FindPlaylistObject(vlc_object_t *obj)
{
    while (obj != NULL) {
        const struct vlc_common_members *members =
            (const struct vlc_common_members *)obj;

        if (members->object_type != NULL &&
            strncmp(members->object_type, "playlist", 8) == 0) {
            return (playlist_t *)obj;
        }
        obj = members->parent;
    }

    return NULL;
}

void SyncAudioMutedForBlockedFrame(filter_t *filter, bool mute_requested)
{
    filter_sys_t *sys;
    playlist_t *playlist;
    input_thread_t *input;
    audio_output_t *aout;
    int current_mute = 0;
    bool previous_mute = false;
    bool mute_applied = false;
    bool used_aout = false;
    vlc_input_control_fn input_control = NULL;
    vlc_playlist_mute_get_fn playlist_mute_get = NULL;
    vlc_playlist_mute_set_fn playlist_mute_set = NULL;
    vlc_aout_mute_get_fn mute_get = NULL;
    vlc_aout_mute_set_fn mute_set = NULL;
    vlc_var_get_fn var_get = NULL;
    vlc_var_set_fn var_set = NULL;
    vlc_object_release_fn object_release = NULL;
    vlc_value_t mute_value;

    if (filter == NULL || filter->p_sys == NULL)
        return;

    sys = filter->p_sys;
    if (!sys->mute_audio_on_blocked)
        return;

    if (mute_requested && sys->audio_muted_by_filter)
        return;
    if (!mute_requested && !sys->audio_muted_by_filter)
        return;

    if (!LoadVlcPlaybackAccessors(&input_control, &playlist_mute_get,
                                 &playlist_mute_set,
                                 &mute_get, &mute_set,
                                 &var_get, &var_set, &object_release)) {
        if (mute_requested && !sys->audio_mute_warning_logged) {
            fprintf(stderr,
                    "icop: VLC playback accessors are unavailable for mute control\n");
            sys->audio_mute_warning_logged = true;
        }
        return;
    }

    playlist = FindPlaylistObject((vlc_object_t *)filter);
    if (playlist != NULL &&
        playlist_mute_get != NULL &&
        playlist_mute_set != NULL) {
        current_mute = playlist_mute_get(playlist);
        previous_mute = current_mute > 0;
        if (mute_requested) {
            if (current_mute <= 0) {
                if (playlist_mute_set(playlist, true) == VLC_SUCCESS) {
                    mute_applied = true;
                } else if (!sys->audio_mute_warning_logged) {
                    fprintf(stderr,
                            "icop: failed to mute VLC playlist while blocked\n");
                    sys->audio_mute_warning_logged = true;
                }
            } else {
                mute_applied = true;
            }
        } else if (sys->audio_muted_by_filter && current_mute > 0) {
            playlist_mute_set(playlist, false);
            mute_applied = true;
        }
        if (mute_requested) {
            sys->audio_previous_mute = previous_mute;
            sys->audio_muted_by_filter = mute_applied;
        } else {
            sys->audio_muted_by_filter = false;
            sys->audio_previous_mute = false;
        }
        return;
    }

    input = FindInputThread((vlc_object_t *)filter);
    if (input == NULL) {
        if (mute_requested && !sys->audio_mute_warning_logged) {
            fprintf(stderr,
                    "icop: could not locate the current VLC input to mute audio\n");
            sys->audio_mute_warning_logged = true;
        }
        return;
    }

    if (input_control(input, INPUT_GET_AOUT, &aout) == VLC_SUCCESS &&
        aout != NULL && mute_get != NULL && mute_set != NULL) {
        used_aout = true;
        current_mute = mute_get(aout);
        previous_mute = current_mute > 0;
        if (mute_requested) {
            if (current_mute <= 0) {
                if (mute_set(aout, true) == VLC_SUCCESS) {
                    mute_applied = true;
                } else if (!sys->audio_mute_warning_logged) {
                    fprintf(stderr,
                            "icop: failed to mute VLC audio output while blocked\n");
                    sys->audio_mute_warning_logged = true;
                }
            } else {
                mute_applied = true;
            }
        } else if (sys->audio_muted_by_filter && current_mute > 0) {
            mute_set(aout, false);
            mute_applied = true;
        }
    }

    if (var_get != NULL && var_set != NULL) {
        memset(&mute_value, 0, sizeof(mute_value));
        if (var_get((vlc_object_t *)input, "mute", &mute_value) == VLC_SUCCESS) {
            previous_mute = previous_mute || mute_value.b_bool;
        }

        mute_value.b_bool = mute_requested;
        if (mute_requested) {
            if (var_set((vlc_object_t *)input, "mute", mute_value) == VLC_SUCCESS) {
                mute_applied = true;
            } else if (!sys->audio_mute_warning_logged) {
                fprintf(stderr,
                        "icop: failed to mute VLC input via mute variable\n");
                sys->audio_mute_warning_logged = true;
            }
        } else if (sys->audio_muted_by_filter && current_mute > 0) {
            (void)var_set((vlc_object_t *)input, "mute", mute_value);
            mute_applied = true;
        }
    }

    if (mute_requested) {
        sys->audio_previous_mute = previous_mute;
        sys->audio_muted_by_filter = mute_applied;
    } else {
        sys->audio_muted_by_filter = false;
        sys->audio_previous_mute = false;
    }

    if (used_aout)
        object_release((vlc_object_t *)aout);
}
