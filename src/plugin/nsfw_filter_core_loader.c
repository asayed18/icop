/*****************************************************************************
 * nsfw_filter_core_loader.c: extracted from nsfw_filter.c
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
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
#include <vlc_picture.h>

#include "nsfw_filter.h"
#include "platform_abstraction.h"
#include "nsfw_filter_internal.h"

bool LoadCoreModule(filter_sys_t *sys)
{
    char base_path[1024];
    char candidate[1024];
    void *module = NULL;
    static const char *const kCoreLibraryNames[] = {
        "libicop_core.so",
        "libicop_core.dylib",
        "icop_core.so",
        "icop_core.dylib",
#ifdef _WIN32
        "icop_core.dll",
#endif
        NULL,
    };

    if (!sys || sys->core_module != NULL)
        return sys && sys->core_module != NULL;

    if (nsfw_plat_get_plugin_dir(base_path, sizeof(base_path))) {
        for (const char *const *name = kCoreLibraryNames; *name != NULL; ++name) {
            int rc = snprintf(candidate, sizeof(candidate), "%s%s",
                              base_path, *name);
            if (rc < 0 || (size_t)rc >= sizeof(candidate))
                continue;
            module = nsfw_plat_dlopen(candidate);
            if (module != NULL)
                break;
        }
    }

    if (module == NULL) {
        for (const char *const *name = kCoreLibraryNames; *name != NULL; ++name) {
            module = nsfw_plat_dlopen(*name);
            if (module != NULL)
                break;
        }
    }

    if (module == NULL)
        return false;

    sys->core_module = module;

    sys->config_default_fn = (nsfw_config_t (*)(void))
        nsfw_plat_dlsym(sys->core_module, "nsfw_config_default");
    sys->model_profile_name_fn = (const char *(*)(nsfw_model_profile_t))
        nsfw_plat_dlsym(sys->core_module, "nsfw_model_profile_name");
    sys->model_profile_parse_fn = (int (*)(const char *,
                                           nsfw_model_profile_t *))
        nsfw_plat_dlsym(sys->core_module, "nsfw_model_profile_parse");
    sys->config_set_model_profile_fn = (void (*)(nsfw_config_t *,
                                                 nsfw_model_profile_t))
        nsfw_plat_dlsym(sys->core_module, "nsfw_config_set_model_profile");
    sys->detector_create_fn = (nsfw_detector_t *(*)(const nsfw_config_t *))
        nsfw_plat_dlsym(sys->core_module, "nsfw_detector_create");
    sys->detector_destroy_fn = (void (*)(nsfw_detector_t *))
        nsfw_plat_dlsym(sys->core_module, "nsfw_detector_destroy");
    sys->detector_classify_fn = (nsfw_result_t (*)(nsfw_detector_t *,
                                                   const uint8_t *,
                                                   int, int, int))
        nsfw_plat_dlsym(sys->core_module, "nsfw_detector_classify");
    sys->core_has_provider_fn = (int (*)(const char *))
        nsfw_plat_dlsym(sys->core_module, "nsfw_core_has_provider");

    if (!sys->config_default_fn || !sys->model_profile_name_fn ||
        !sys->model_profile_parse_fn || !sys->config_set_model_profile_fn ||
        !sys->detector_create_fn ||
        !sys->detector_destroy_fn || !sys->detector_classify_fn) {
        nsfw_plat_dlclose(sys->core_module);
        sys->core_module = NULL;
        sys->config_default_fn = NULL;
        sys->model_profile_name_fn = NULL;
        sys->model_profile_parse_fn = NULL;
        sys->config_set_model_profile_fn = NULL;
        sys->detector_create_fn = NULL;
        sys->detector_destroy_fn = NULL;
        sys->detector_classify_fn = NULL;
        sys->core_has_provider_fn = NULL;
        return false;
    }

    return true;
}


void UnloadCoreModule(filter_sys_t *sys)
{
    if (!sys)
        return;

    if (sys->core_module != NULL)
        nsfw_plat_dlclose(sys->core_module);

    sys->core_module = NULL;
    sys->config_default_fn = NULL;
    sys->model_profile_name_fn = NULL;
    sys->model_profile_parse_fn = NULL;
    sys->config_set_model_profile_fn = NULL;
    sys->detector_create_fn = NULL;
    sys->detector_destroy_fn = NULL;
    sys->detector_classify_fn = NULL;
    sys->core_has_provider_fn = NULL;
}

bool LoadVlcOptionAccessors(vlc_config_chain_parse_fn *chain_parse,
                                   vlc_var_create_fn *var_create,
                                   vlc_var_get_checked_fn *var_get_checked)
{
    static bool loaded = false;
    static vlc_config_chain_parse_fn cached_chain_parse = NULL;
    static vlc_var_create_fn cached_var_create = NULL;
    static vlc_var_get_checked_fn cached_var_get_checked = NULL;
    if (!loaded) {
        cached_chain_parse = (vlc_config_chain_parse_fn)
            nsfw_plat_lookup_vlc_sym("config_ChainParse");
        cached_var_create = (vlc_var_create_fn)
            nsfw_plat_lookup_vlc_sym("var_Create");
        cached_var_get_checked = (vlc_var_get_checked_fn)
            nsfw_plat_lookup_vlc_sym("var_GetChecked");
        loaded = true;
    }

    if (chain_parse != NULL)
        *chain_parse = cached_chain_parse;
    if (var_create != NULL)
        *var_create = cached_var_create;
    if (var_get_checked != NULL)
        *var_get_checked = cached_var_get_checked;

    return cached_chain_parse != NULL &&
           cached_var_create != NULL &&
           cached_var_get_checked != NULL;
}

bool LoadVlcConfigWriteAccessors(vlc_config_put_psz_fn *put_psz,
                                        vlc_config_put_int_fn *put_int,
                                        vlc_config_put_float_fn *put_float,
                                        vlc_config_save_file_fn *save_file)
{
    static bool loaded = false;
    static vlc_config_put_psz_fn cached_put_psz = NULL;
    static vlc_config_put_int_fn cached_put_int = NULL;
    static vlc_config_put_float_fn cached_put_float = NULL;
    static vlc_config_save_file_fn cached_save_file = NULL;
    if (!loaded) {
        cached_put_psz = (vlc_config_put_psz_fn)
            nsfw_plat_lookup_vlc_sym("config_PutPsz");
        cached_put_int = (vlc_config_put_int_fn)
            nsfw_plat_lookup_vlc_sym("config_PutInt");
        cached_put_float = (vlc_config_put_float_fn)
            nsfw_plat_lookup_vlc_sym("config_PutFloat");
        cached_save_file = (vlc_config_save_file_fn)
            nsfw_plat_lookup_vlc_sym("config_SaveConfigFile");
        loaded = true;
    }

    if (put_psz != NULL)
        *put_psz = cached_put_psz;
    if (put_int != NULL)
        *put_int = cached_put_int;
    if (put_float != NULL)
        *put_float = cached_put_float;
    if (save_file != NULL)
        *save_file = cached_save_file;

    return cached_put_psz != NULL &&
           cached_put_int != NULL &&
           cached_put_float != NULL &&
           cached_save_file != NULL;
}

bool LoadVlcPlaybackAccessors(vlc_input_control_fn *input_control,
                                     vlc_playlist_mute_get_fn *playlist_mute_get,
                                     vlc_playlist_mute_set_fn *playlist_mute_set,
                                     vlc_aout_mute_get_fn *mute_get,
                                     vlc_aout_mute_set_fn *mute_set,
                                     vlc_var_get_fn *var_get,
                                     vlc_var_set_fn *var_set,
                                     vlc_object_release_fn *object_release)
{
    static bool loaded = false;
    static vlc_input_control_fn cached_input_control = NULL;
    static vlc_playlist_mute_get_fn cached_playlist_mute_get = NULL;
    static vlc_playlist_mute_set_fn cached_playlist_mute_set = NULL;
    static vlc_aout_mute_get_fn cached_mute_get = NULL;
    static vlc_aout_mute_set_fn cached_mute_set = NULL;
    static vlc_var_get_fn cached_var_get = NULL;
    static vlc_var_set_fn cached_var_set = NULL;
    static vlc_object_release_fn cached_object_release = NULL;
    if (!loaded) {
        cached_input_control = (vlc_input_control_fn)
            nsfw_plat_lookup_vlc_sym("input_Control");
        cached_playlist_mute_get = (vlc_playlist_mute_get_fn)
            nsfw_plat_lookup_vlc_sym("playlist_MuteGet");
        cached_playlist_mute_set = (vlc_playlist_mute_set_fn)
            nsfw_plat_lookup_vlc_sym("playlist_MuteSet");
        cached_mute_get = (vlc_aout_mute_get_fn)
            nsfw_plat_lookup_vlc_sym("aout_MuteGet");
        cached_mute_set = (vlc_aout_mute_set_fn)
            nsfw_plat_lookup_vlc_sym("aout_MuteSet");
        cached_var_get = (vlc_var_get_fn)
            nsfw_plat_lookup_vlc_sym("var_Get");
        cached_var_set = (vlc_var_set_fn)
            nsfw_plat_lookup_vlc_sym("var_Set");
        cached_object_release = (vlc_object_release_fn)
            nsfw_plat_lookup_vlc_sym("vlc_object_release");
        loaded = true;
    }

    if (input_control)
        *input_control = cached_input_control;
    if (playlist_mute_get)
        *playlist_mute_get = cached_playlist_mute_get;
    if (playlist_mute_set)
        *playlist_mute_set = cached_playlist_mute_set;
    if (mute_get)
        *mute_get = cached_mute_get;
    if (mute_set)
        *mute_set = cached_mute_set;
    if (var_get)
        *var_get = cached_var_get;
    if (var_set)
        *var_set = cached_var_set;
    if (object_release)
        *object_release = cached_object_release;

    return cached_input_control != NULL &&
           cached_object_release != NULL &&
           ((cached_playlist_mute_get != NULL && cached_playlist_mute_set != NULL) ||
            (cached_mute_get != NULL && cached_mute_set != NULL) ||
            (cached_var_get != NULL && cached_var_set != NULL));
}
