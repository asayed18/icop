#ifndef NSFW_FILTER_INTERNAL_H
#define NSFW_FILTER_INTERNAL_H

#include "nsfw_filter.h"
#include <poll.h>
#include <vlc_filter.h>

/*****************************************************************************
 * Backend ops abstraction — hardware backends register these function
 * pointers.  The main filter code calls through the ops table so it never
 * needs to know whether the active backend is D3D11, VAAPI, or something
 * else.
 *****************************************************************************/
struct nsfw_backend_ops_t
{
    void     (*close)(void *backend);
    int      (*set_analysis_size)(void *backend, int width, int height);
    int      (*readback_rgb)(void *backend, picture_t *picture,
                             uint8_t *rgb, size_t rgb_capacity,
                             int *width, int *height);
    unsigned (*decoder_surface_count)(void *backend, picture_t *picture);
    picture_t *(*render_blocked)(filter_t *filter, void *backend,
                                 picture_t *source,
                                 nsfw_block_style_t style);
    picture_t *(*render_debug_overlay)(filter_t *filter, void *backend,
                                        picture_t *source,
                                        float score, float threshold);
    int      (*dump_ppm)(void *backend, picture_t *picture,
                         const char *path);
    const char *(*adapter_name)(const void *backend);
    const char *(*format_name)(const void *backend);
};

typedef struct nsfw_backend_ops_t nsfw_backend_ops_t;

/*****************************************************************************
 * Backend detection & open — called from Open() in nsfw_filter.c.
 * Tries D3D11, VAAPI, and opaque-fallback in priority order, sets
 * sys->backend_ops / sys->backend_data on success.
 *****************************************************************************/
int nsfw_backend_open(filter_t *filter);

/*****************************************************************************
 * Shared typedefs for dynamically-loaded VLC symbols
 *****************************************************************************/
typedef void (*vlc_config_chain_parse_fn)(vlc_object_t *, const char *,
                                          const char *const *,
                                          config_chain_t *);
typedef int (*vlc_var_create_fn)(vlc_object_t *, const char *, int);
typedef int (*vlc_var_get_checked_fn)(vlc_object_t *, const char *, int,
                                      vlc_value_t *);
typedef void (*vlc_config_put_psz_fn)(vlc_object_t *, const char *,
                                      const char *);
typedef void (*vlc_config_put_int_fn)(vlc_object_t *, const char *, int64_t);
typedef void (*vlc_config_put_float_fn)(vlc_object_t *, const char *, float);
typedef int (*vlc_config_save_file_fn)(vlc_object_t *);

typedef int (*vlc_input_control_fn)(input_thread_t *, int, ...);
typedef int (*vlc_playlist_mute_get_fn)(playlist_t *);
typedef int (*vlc_playlist_mute_set_fn)(playlist_t *, bool);
typedef int (*vlc_aout_mute_get_fn)(audio_output_t *);
typedef int (*vlc_aout_mute_set_fn)(audio_output_t *, bool);
typedef int (*vlc_var_get_fn)(vlc_object_t *, const char *, vlc_value_t *);
typedef int (*vlc_var_set_fn)(vlc_object_t *, const char *, vlc_value_t);
typedef void (*vlc_object_release_fn)(vlc_object_t *);

/*****************************************************************************
 * Shared defines / globals
 *****************************************************************************/
#define NSFW_CFG_PREFIX "nsfw-"
#define NSFW_SETTINGS_VERSION_CURRENT 1
#define NSFW_D3D11_MAX_BUFFERED_FRAMES 8
#define NSFW_D3D11_RESERVED_DECODER_SURFACES 3

extern const char *const kNsfwFilterOptions[];

#define NSFW_ICON_REFRESH_FRAMES 120

/*****************************************************************************
 * Core loader (nsfw_filter_core_loader.c)
 *****************************************************************************/
bool LoadCoreModule(filter_sys_t *sys);
void UnloadCoreModule(filter_sys_t *sys);
bool LoadVlcOptionAccessors(vlc_config_chain_parse_fn *chain_parse,
                            vlc_var_create_fn *var_create,
                            vlc_var_get_checked_fn *var_get_checked);
bool LoadVlcConfigWriteAccessors(vlc_config_put_psz_fn *put_psz,
                                 vlc_config_put_int_fn *put_int,
                                 vlc_config_put_float_fn *put_float,
                                 vlc_config_save_file_fn *save_file);
bool LoadVlcPlaybackAccessors(vlc_input_control_fn *input_control,
                              vlc_playlist_mute_get_fn *playlist_mute_get,
                              vlc_playlist_mute_set_fn *playlist_mute_set,
                              vlc_aout_mute_get_fn *mute_get,
                              vlc_aout_mute_set_fn *mute_set,
                              vlc_var_get_fn *var_get,
                              vlc_var_set_fn *var_set,
                              vlc_object_release_fn *object_release);

/*****************************************************************************
 * Config helpers (nsfw_filter_config.c)
 *****************************************************************************/
void ParseVlcFilterOptions(filter_t *filter);
void SyncVlcOptionsToEnv(filter_t *filter);
void MaybeReplaceLegacyPreset(filter_t *filter);
void PersistModernDefaultSettings(filter_t *filter);
nsfw_block_style_t ParseBlockStyle(const char *text);
const char *BlockStyleName(nsfw_block_style_t style);
bool MarkBackendFailureLogged(filter_sys_t *sys);
void ReleasePicture(picture_t *pic);
image_handler_t *CreateImageHandler(filter_t *filter);
void DestroyImageHandler(image_handler_t *handler);
char *GetVlcConfigString(filter_t *filter, const char *name);
int GetVlcConfigInteger(filter_t *filter, const char *name, int fallback);
float GetVlcConfigFloat(filter_t *filter, const char *name, float fallback);
int EnsureRgbBuffer(filter_sys_t *sys, size_t required);
unsigned ResolveAnalysisStride(const video_format_t *fmt);
unsigned ResolveBlockPaddingFrames(unsigned analysis_stride);
unsigned ResolvePrebufferFrames(const video_format_t *fmt);
unsigned MinimumPrebufferFrames(unsigned analysis_stride, unsigned padding_frames);
unsigned ResolveDecisionReloadStride(void);
unsigned ResolveWorkerCount(void);
vlc_tick_t EstimatedFrameInterval(const video_format_t *fmt);
void ConstrainDecoderQueue(filter_sys_t *sys, unsigned surface_count);
nsfw_model_profile_t ResolveUsableModelProfile(nsfw_model_profile_t preferred);
char *DuplicateString(const char *src);

/*****************************************************************************
 * Audio mute (nsfw_filter_audio.c)
 *****************************************************************************/
void SyncAudioMutedForBlockedFrame(filter_t *filter, bool mute_requested);
bool GetInputMediaTimeMs(filter_t *filter, uint64_t *time_ms);

/*****************************************************************************
 * Decision map (nsfw_filter_decision.c)
 *****************************************************************************/
void RefreshDecisionMap(filter_sys_t *sys, bool force);
bool DecisionMapShouldBlock(filter_t *p_filter, picture_t *p_pic);
picture_t *ApplyDisplayOutput(filter_t *filter, picture_t *pic, bool blocked,
                               const nsfw_result_t *evaluation);
void ClearDecisionMap(filter_sys_t *sys);
void ClearTimeBlockRanges(filter_sys_t *sys);
void ResetOutputMaskState(filter_sys_t *sys);
bool TimelineDiscontinuityDetected(const filter_sys_t *sys,
                                   uint64_t timestamp_ms);
void SetTimelineOrigin(filter_t *filter, uint64_t raw_timestamp_ms);
uint64_t RawPictureTimeMs(const filter_sys_t *sys, const picture_t *pic);
uint64_t PictureTimeMs(const filter_sys_t *sys, const picture_t *pic);
bool EnsureTimeBlockRangeCapacity(filter_sys_t *sys, size_t required);

/*****************************************************************************
 * Worker threads / frame queue (nsfw_filter_worker.c)
 *****************************************************************************/
int StartDetectorWorker(filter_sys_t *sys, const nsfw_config_t *cfg);
void StopDetectorWorker(filter_sys_t *sys);
bool QueuePictureLocked(filter_sys_t *sys, picture_t *pic, bool analyze);
picture_t *TakeReadyOutputLocked(filter_sys_t *sys, bool *blocked,
                                 nsfw_result_t *result, bool *evaluated);
bool HasProcessingFramesLocked(filter_sys_t *sys);
void ReleaseQueuedFramesLocked(filter_sys_t *sys);
bool OldestFrameReadyLocked(filter_sys_t *sys);
bool TimeInBlockedRangeLocked(const filter_sys_t *sys, uint64_t timestamp_ms);
void RegisterPositiveDetection(filter_sys_t *sys,
                               const nsfw_result_t *result,
                               uint64_t timestamp_ms,
                               bool *blocked);

/*****************************************************************************
 * Open / Close / Flush / Filter (in nsfw_filter.c)
 *****************************************************************************/
int  Open(vlc_object_t *);
void Close(vlc_object_t *);
void Flush(filter_t *);
picture_t *Filter(filter_t *, picture_t *);

#endif
