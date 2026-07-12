/*****************************************************************************
 * vlc_plugin.h: VLC plugin macros (minimal stub for build)
 *****************************************************************************/
#ifndef VLC_PLUGIN_H
#define VLC_PLUGIN_H 1

#include <vlc_common.h>

/* Category constants */
#define CAT_AUDIO     1
#define CAT_VIDEO     2
#define CAT_INPUT     4
#define CAT_SOUT      6
#define CAT_ADVANCED  7
#define CAT_PLAYLIST  8
#define CAT_INTERFACE 9

/* Subcategory constants */
#define SUBCAT_AUDIO_AFILTER    301
#define SUBCAT_AUDIO_MISC       302
#define SUBCAT_VIDEO_VFILTER    402
#define SUBCAT_VIDEO_SPLITTER   404
#define SUBCAT_INPUT_DEMUX      501
#define SUBCAT_INPUT_ACCESS    502

/* Module descriptor macros – expanded to nothing for stub build.
 * In a real VLC build these expand to a descriptor struct. */
#define vlc_module_begin() \
    static void vlc_entry__dummy(void) __attribute__((unused)); \
    static void vlc_entry__dummy(void) {

#define vlc_module_end() }

#define set_description(d)      ((void)0);
#define set_shortname(n)        ((void)0);
#define set_category(c)         ((void)0);
#define set_subcategory(c)      ((void)0);
#define set_capability(c, s)    ((void)0);
#define set_callbacks(o, c)     ((void)0);
#define set_callback(f)         ((void)0);
#define add_shortcut(s)         ((void)0);
#define add_string(n, v, t, l, a)       ((void)0);
#define add_integer(n, v, t, l, a)      ((void)0);
#define add_float(n, v, t, l, a)        ((void)0);
#define add_bool(n, v, t, l, a)         ((void)0);
#define set_section(c, h)               ((void)0);
#define change_string_list(v, t)        ((void)0);
#define change_float_range(min, max)    ((void)0);
#define change_integer_range(min, max)  ((void)0);
#define change_private()                ((void)0);

/* Module export symbol */
#define VLC_MODULE_EXPORT __attribute__((visibility("default")))

#endif /* VLC_PLUGIN_H */
