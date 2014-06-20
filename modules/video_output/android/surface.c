/*****************************************************************************
 * androidsurface.c: android video output using Surface Flinger
 *****************************************************************************
 * Copyright © 2011 VideoLAN
 *
 * Authors: Ming Hu <tewilove@gmail.com>
 *          Ludovic Fauvet <etix@l0cal.com>
 *          Sébastien Toque <xilasz@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_picture_pool.h>

#include "utils.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define CHROMA_TEXT N_("Chroma used")
#define CHROMA_LONGTEXT N_(\
    "Force use of a specific chroma for output. Default is RGB32.")

#define CFG_PREFIX "androidsurface-"

static int  Open (vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin()
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_shortname("AndroidSurface")
    set_description(N_("Android Surface video output"))
    set_capability("vout display", 155)
    add_shortcut("androidsurface", "android")
    add_string(CFG_PREFIX "chroma", NULL, CHROMA_TEXT, CHROMA_LONGTEXT, true)
    set_callbacks(Open, Close)
vlc_module_end()

/*****************************************************************************
 * JNI prototypes
 *****************************************************************************/

extern JavaVM *myVm;
extern void *jni_LockAndGetAndroidSurface();
extern jobject jni_LockAndGetAndroidJavaSurface();
extern void  jni_UnlockAndroidSurface();
extern void  jni_SetAndroidSurfaceSize(int width, int height, int visible_width, int visible_height,
                                       int sar_num, int sar_den, int orientation);

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

static picture_pool_t   *Pool  (vout_display_t *, unsigned);
static void             Display(vout_display_t *, picture_t *, subpicture_t *);
static int              Control(vout_display_t *, int, va_list);

/* */
struct vout_display_sys_t {
    picture_pool_t *pool;

    jobject jsurf;
    ANativeWindow *window;

    /* density */
    int i_sar_num;
    int i_sar_den;

    video_format_t fmt;
    bool b_changed_crop;
};

struct picture_sys_t {
    void *surf;
    ANativeWindow_Buffer info;
    vout_display_sys_t *sys;
};

static int  AndroidLockSurface(picture_t *);
static void AndroidUnlockSurface(picture_t *);

static vlc_mutex_t single_instance = VLC_STATIC_MUTEX;

static int Open(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;

    /* */
    if (vlc_mutex_trylock(&single_instance) != 0) {
        msg_Err(vd, "Can't start more than one instance at a time");
        return VLC_EGENERIC;
    }

    /* Allocate structure */
    vout_display_sys_t *sys = (struct vout_display_sys_t*) calloc(1, sizeof(*sys));
    if (!sys) {
        vlc_mutex_unlock(&single_instance);
        return VLC_ENOMEM;
    }

    /* Setup chroma */
    video_format_t fmt = vd->fmt;

    if (fmt.i_chroma == VLC_CODEC_ANDROID_OPAQUE)
        return VLC_EGENERIC;

    char *psz_fcc = var_InheritString(vd, CFG_PREFIX "chroma");
    if( psz_fcc ) {
        fmt.i_chroma = vlc_fourcc_GetCodecFromString(VIDEO_ES, psz_fcc);
        free(psz_fcc);
    } else
        fmt.i_chroma = VLC_CODEC_RGB32;

    switch(fmt.i_chroma) {
        case VLC_CODEC_YV12:
            /* avoid swscale usage by asking for I420 instead since the
             * vout already has code to swap the buffers */
            fmt.i_chroma = VLC_CODEC_I420;
        case VLC_CODEC_I420:
            break;

        case VLC_CODEC_RGB16:
            fmt.i_bmask = 0x0000001f;
            fmt.i_gmask = 0x000007e0;
            fmt.i_rmask = 0x0000f800;
            break;

        case VLC_CODEC_RGB32:
            fmt.i_rmask  = 0x000000ff;
            fmt.i_gmask  = 0x0000ff00;
            fmt.i_bmask  = 0x00ff0000;
            break;

        default:
            return VLC_EGENERIC;
    }
    video_format_FixRgb(&fmt);

    msg_Dbg(vd, "Pixel format %4.4s", (char*)&fmt.i_chroma);
    sys->fmt = fmt;

    /* Create the associated picture */
    picture_sys_t *picsys = malloc(sizeof(*picsys));
    if (unlikely(picsys == NULL))
        goto enomem;
    picsys->sys = sys;

    picture_resource_t resource = { .p_sys = picsys };
    picture_t *picture = picture_NewFromResource(&fmt, &resource);
    if (!picture) {
        free(picsys);
        goto enomem;
    }

    /* Wrap it into a picture pool */
    picture_pool_configuration_t pool_cfg;
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.picture_count = 1;
    pool_cfg.picture       = &picture;
    pool_cfg.lock          = AndroidLockSurface;
    pool_cfg.unlock        = AndroidUnlockSurface;

    sys->pool = picture_pool_NewExtended(&pool_cfg);
    if (!sys->pool) {
        picture_Release(picture);
        goto enomem;
    }

    /* Setup vout_display */
    vd->sys     = sys;
    vd->fmt     = fmt;
    vd->pool    = Pool;
    vd->display = Display;
    vd->control = Control;
    vd->prepare = NULL;
    vd->manage  = Manage;

    /* Fix initial state */
    vout_display_SendEventFullscreen(vd, false);

    sys->i_sar_num = vd->source.i_sar_num;
    sys->i_sar_den = vd->source.i_sar_den;

    jni_SetAndroidSurfaceSize(fmt.i_width, fmt.i_height, fmt.i_visible_width, fmt.i_visible_height,
                              sys->i_sar_num, sys->i_sar_den, fmt.orientation);

    return VLC_SUCCESS;

enomem:
    free(sys);
    vlc_mutex_unlock(&single_instance);
    return VLC_ENOMEM;
}

static void Close(vlc_object_t *p_this)
{
    vout_display_t *vd = (vout_display_t *)p_this;
    vout_display_sys_t *sys = vd->sys;

    picture_pool_Delete(sys->pool);
    if (sys->window)
        ANativeWindow_release(sys->window);
    free(sys);
    vlc_mutex_unlock(&single_instance);
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned count)
{
    VLC_UNUSED(count);

    return vd->sys->pool;
}

#define ALIGN_16_PIXELS( x ) ( ( ( x ) + 15 ) / 16 * 16 )
static void SetupPictureYV12( ANativeWindow_Buffer* p_surfaceInfo, picture_t *p_picture )
{
    /* according to document of android.graphics.ImageFormat.YV12 */
    int i_stride = ALIGN_16_PIXELS( p_surfaceInfo->stride );
    int i_c_stride = ALIGN_16_PIXELS( i_stride / 2 );

    p_picture->p->i_pitch = i_stride;

    /* Fill chroma planes for planar YUV */
    for( int n = 1; n < p_picture->i_planes; n++ )
    {
        const plane_t *o = &p_picture->p[n-1];
        plane_t *p = &p_picture->p[n];

        p->p_pixels = o->p_pixels + o->i_lines * o->i_pitch;
        p->i_pitch  = i_c_stride;
        p->i_lines  = p_picture->format.i_height / 2;
        /*
          Explicitly set the padding lines of the picture to black (127 for YUV)
          since they might be used by Android during rescaling.
        */
        int visible_lines = p_picture->format.i_visible_height / 2;
        if (visible_lines < p->i_lines)
            memset(&p->p_pixels[visible_lines * p->i_pitch], 127, (p->i_lines - visible_lines) * p->i_pitch);
    }

    if( vlc_fourcc_AreUVPlanesSwapped( p_picture->format.i_chroma,
                                       VLC_CODEC_YV12 ) ) {
        uint8_t *p_tmp = p_picture->p[1].p_pixels;
        p_picture->p[1].p_pixels = p_picture->p[2].p_pixels;
        p_picture->p[2].p_pixels = p_tmp;
    }
}

static int  AndroidLockSurface(picture_t *picture)
{
    picture_sys_t *picsys = picture->p_sys;
    vout_display_sys_t *sys = picsys->sys;
    ANativeWindow_Buffer *info;
    int sw, sh;

    sw = sys->fmt.i_width;
    sh = sys->fmt.i_height;

    jobject jsurf = jni_LockAndGetAndroidJavaSurface();
    if (unlikely(!jsurf)) {
        jni_UnlockAndroidSurface();
        return VLC_EGENERIC;
    }
    if (sys->window && jsurf != sys->jsurf) {
        ANativeWindow_release(sys->window);
        sys->window = NULL;
    }
    sys->jsurf = jsurf;
    if (!sys->window) {
        JNIEnv *p_env;
        (*myVm)->AttachCurrentThread(myVm, &p_env, NULL);
        sys->window = ANativeWindow_fromSurface(p_env, jsurf);
        (*myVm)->DetachCurrentThread(myVm);
    }
    picsys->surf = sys->window;
    info = &picsys->info;

    ANativeWindow_lock(sys->window, info, NULL);

    // For RGB (32 or 16) we need to align on 8 or 4 pixels, 16 pixels for YUV
    int align_pixels = (16 / picture->p[0].i_pixel_pitch) - 1;
    int aligned_width = (sw + align_pixels) & ~align_pixels;

    if (sys->b_changed_crop) {
        ANativeWindow_unlockAndPost(sys->window);
        jni_UnlockAndroidSurface();
        jni_SetAndroidSurfaceSize(sys->fmt.i_width, sys->fmt.i_height, sys->fmt.i_visible_width, sys->fmt.i_visible_height,
                                  sys->i_sar_num, sys->i_sar_den, sys->fmt.orientation);
        sys->b_changed_crop = false;
        return VLC_EGENERIC;
    }
    else if (info->width != aligned_width || info->height != sh) {
        ANativeWindow_unlockAndPost(sys->window);
        ANativeWindow_setBuffersGeometry(sys->window, aligned_width, sh, 0);

        ANativeWindow_lock(sys->window, info, NULL);
        if (info->width != aligned_width || info->height != sh) {
            /* Failed to resize the buffer, give up. */
            ANativeWindow_unlockAndPost(sys->window);
            jni_UnlockAndroidSurface();
            return VLC_EGENERIC;
        }
    }

    picture->p[0].p_pixels = (uint8_t*)info->bits;
    picture->p[0].i_lines = info->height;
    picture->p[0].i_pitch = picture->p[0].i_pixel_pitch * info->stride;

    if (info->format == 0x32315659 /*ANDROID_IMAGE_FORMAT_YV12*/)
        SetupPictureYV12(info, picture);

    return VLC_SUCCESS;
}

static void AndroidUnlockSurface(picture_t *picture)
{
    picture_sys_t *picsys = picture->p_sys;
    vout_display_sys_t *sys = picsys->sys;

    if (likely(picsys->surf))
        ANativeWindow_unlockAndPost(picsys->surf);
    jni_UnlockAndroidSurface();
}

static void Display(vout_display_t *vd, picture_t *picture, subpicture_t *subpicture)
{
    VLC_UNUSED(vd);
    VLC_UNUSED(subpicture);

    /* refcount lowers to 0, and pool_cfg.unlock is called */

    picture_Release(picture);
}

static int Control(vout_display_t *vd, int query, va_list args)
{
    VLC_UNUSED(args);

    switch (query) {
    case VOUT_DISPLAY_HIDE_MOUSE:
        return VLC_SUCCESS;

    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
    {
        if (!vd->sys)
            return VLC_EGENERIC;
        vout_display_sys_t *sys = vd->sys;

        const video_format_t *source = (const video_format_t *)va_arg(args, const video_format_t *);
        sys->fmt = *source;
        sys->b_changed_crop = true;
        return VLC_SUCCESS;
    }

    default:
        msg_Err(vd, "Unknown request in android vout display");

    case VOUT_DISPLAY_CHANGE_FULLSCREEN:
    case VOUT_DISPLAY_CHANGE_WINDOW_STATE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM:
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_GET_OPENGL:
        return VLC_EGENERIC;
    }
}
