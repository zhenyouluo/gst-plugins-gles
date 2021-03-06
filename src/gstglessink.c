/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2011 Julian Scheel <julian@jusst.de>
 * Copyright (C) 2011 Soeren Grunewald <soeren.grunewald@avionic-design.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-glesplugin
 *
 *
 * <refsect2>
 * <title>OpenGL ES2.0 videosink plugin</title>
 * |[
 * gst-launch -v -m videotestsrc ! glessink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <string.h>

#define GST_USE_UNSTABLE_API
#include <gst/gst.h>

#if GST_CHECK_VERSION(1, 0, 0)
#include <gst/video/videooverlay.h>
#else
#include <gst/interfaces/xoverlay.h>
#endif
#include <gst/video/video.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <X11/Xatom.h>

#include <unistd.h>

#include "gstglessink.h"
#include "shader.h"

GST_DEBUG_CATEGORY (gst_gles_sink_debug);


typedef enum _GstGLESPluginProperties  GstGLESPluginProperties;

enum _GstGLESPluginProperties
{
  PROP_0,
  PROP_SILENT,
  PROP_CROP_TOP,
  PROP_CROP_BOTTOM,
  PROP_CROP_LEFT,
  PROP_CROP_RIGHT,
  PROP_DROP_FIRST
};

#if GST_CHECK_VERSION(1, 0, 0)
static void
gst_gles_video_overlay_init (GstVideoOverlayInterface * iface);

G_DEFINE_TYPE_WITH_CODE (GstGLESSink, gst_gles_sink, GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_OVERLAY,
    gst_gles_video_overlay_init));
#else
GST_BOILERPLATE_WITH_INTERFACE (GstGLESSink, gst_gles_sink, GstVideoSink,
    GST_TYPE_VIDEO_SINK, GstXOverlay, GST_TYPE_X_OVERLAY, gst_gles_xoverlay)
#endif

static void gst_gles_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gles_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_gles_sink_start (GstBaseSink * basesink);
static gboolean gst_gles_sink_stop (GstBaseSink * basesink);
static gboolean gst_gles_sink_set_caps (GstBaseSink * basesink,
                                          GstCaps * caps);
static GstFlowReturn gst_gles_sink_render (GstBaseSink * basesink,
                                             GstBuffer * buf);
static GstFlowReturn gst_gles_sink_preroll (GstBaseSink * basesink,
                                              GstBuffer * buf);
static void gst_gles_sink_finalize (GObject *gobject);
static gint setup_gl_context (GstGLESSink *sink);
static gpointer gl_thread_proc (gpointer data);

#define WxH ", width = (int) [ 16, 4096 ], height = (int) [ 16, 4096 ]"

#if GST_CHECK_VERSION(1, 0, 0)
static GstStaticPadTemplate gles_sink_factory =
        GST_STATIC_PAD_TEMPLATE ("sink",
                                 GST_PAD_SINK,
                                 GST_PAD_ALWAYS,
                                 GST_STATIC_CAPS ( GST_VIDEO_CAPS_MAKE("I420")
                                                   WxH) );
#else
static GstStaticPadTemplate gles_sink_factory =
        GST_STATIC_PAD_TEMPLATE ("sink",
                                 GST_PAD_SINK,
                                 GST_PAD_ALWAYS,
                                 GST_STATIC_CAPS ( GST_VIDEO_CAPS_YUV("I420")
                                                   WxH) );
#endif

/* OpenGL ES 2.0 implementation */
static GLuint
gl_create_texture(GLuint tex_filter)
{
    GLuint tex_id = 0;

    glGenTextures (1, &tex_id);
    glBindTexture (GL_TEXTURE_2D, tex_id);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tex_filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, tex_filter);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return tex_id;
}

static void
gl_gen_framebuffer(GstGLESSink *sink)
{
    GstGLESContext *gles = &sink->gl_thread.gles;
    glGenFramebuffers (1, &gles->framebuffer);

    gles->rgb_tex.id = gl_create_texture(GL_LINEAR);
    if (!gles->rgb_tex.id)
        GST_ERROR_OBJECT (sink, "Could not create RGB texture");

    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, GST_VIDEO_SINK_WIDTH (sink),
                  GST_VIDEO_SINK_HEIGHT (sink), 0, GL_RGB,
                  GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer (GL_FRAMEBUFFER, gles->framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, gles->rgb_tex.id, 0);
}

static void
gl_init_textures (GstGLESSink *sink)
{
    sink->gl_thread.gles.y_tex.id = gl_create_texture(GL_NEAREST);
    sink->gl_thread.gles.u_tex.id = gl_create_texture(GL_NEAREST);
    sink->gl_thread.gles.v_tex.id = gl_create_texture(GL_NEAREST);
}

static void
gl_load_texture (GstGLESSink *sink, GstBuffer *buf)
{
#if GST_CHECK_VERSION(1, 0, 0)
    GstMapInfo bufmap;
    guint8 *data;

    if (G_UNLIKELY(!gst_buffer_map (buf, &bufmap, GST_MAP_READ))) {
	GST_WARNING_OBJECT (sink, "%s: Failed to map buffer data", __func__);
	return;
    }

    data = bufmap.data;
#else
    guint8 *data = GST_BUFFER_DATA (buf);
#endif

    GstGLESContext *gles = &sink->gl_thread.gles;
    /* y component */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, gles->y_tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, GST_VIDEO_SINK_WIDTH (sink),
                 GST_VIDEO_SINK_HEIGHT (sink), 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, data);
    glUniform1i (gles->y_tex.loc, 0);

    /* u component */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, gles->u_tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 GST_VIDEO_SINK_WIDTH (sink)/2,
                 GST_VIDEO_SINK_HEIGHT (sink)/2, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, data +
                 GST_VIDEO_SINK_WIDTH (sink) * GST_VIDEO_SINK_HEIGHT (sink));
    glUniform1i (gles->u_tex.loc, 1);

    /* v component */
    glActiveTexture(GL_TEXTURE2);
    glBindTexture (GL_TEXTURE_2D, gles->v_tex.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE,
                 GST_VIDEO_SINK_WIDTH (sink)/2,
                 GST_VIDEO_SINK_HEIGHT (sink)/2, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, data +
                 GST_VIDEO_SINK_WIDTH (sink) * GST_VIDEO_SINK_HEIGHT (sink) +
                 GST_VIDEO_SINK_WIDTH (sink)/2 *
                 GST_VIDEO_SINK_HEIGHT (sink)/2);
    glUniform1i (gles->v_tex.loc, 2);

#if GST_CHECK_VERSION(1, 0, 0)
    gst_buffer_unmap(buf, &bufmap);
#endif
}

static void
gl_draw_fbo (GstGLESSink *sink, GstBuffer *buf)
{
    GLfloat vVertices[] =
    {
        -1.0f, -1.0f,
        0.0f, 1.0f,

        1.0f, -1.0f,
        1.0f, 1.0f,

        1.0f, 1.0f,
        1.0f, 0.0f,

        -1.0f, 1.0f,
        0.0f, 0.0f,
    };
    GLushort indices[] = { 0, 1, 2, 0, 2, 3 };
    GstGLESContext *gles = &sink->gl_thread.gles;

    glBindFramebuffer (GL_FRAMEBUFFER, gles->framebuffer);
    glUseProgram (gles->deinterlace.program);

    glViewport(0, 0, GST_VIDEO_SINK_WIDTH (sink),
               GST_VIDEO_SINK_HEIGHT (sink));

    glClear (GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer (gles->deinterlace.position_loc, 2,
                           GL_FLOAT, GL_FALSE, 4 * sizeof (GLfloat),
                           vVertices);

    glVertexAttribPointer (gles->deinterlace.texcoord_loc, 2,
                           GL_FLOAT, GL_FALSE, 4 * sizeof (GLfloat),
                           &vVertices[2]);

    glEnableVertexAttribArray (gles->deinterlace.position_loc);
    glEnableVertexAttribArray (gles->deinterlace.texcoord_loc);

    gl_load_texture(sink, buf);
    GLint line_height_loc =
            glGetUniformLocation(gles->deinterlace.program,
                                 "line_height");
    glUniform1f(line_height_loc, 1.0/sink->video_height);

    glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
}

void
gl_draw_onscreen (GstGLESSink *sink)
{
    GLfloat vVertices[] =
    {
        -1.0f, -1.0f,
        0.0f, 0.0f,

        1.0f, -1.0f,
        1.0f, 0.0f,

        1.0f, 1.0f,
        1.0f, 1.0f,

        -1.0f, 1.0f,
        0.0f, 1.0f,
    };
    GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

    GstVideoRectangle src;
    GstVideoRectangle dst;
    GstVideoRectangle result;

    GstGLESContext *gles = &sink->gl_thread.gles;

    /* add cropping to texture coordinates */
    float crop_left = (float)sink->crop_left / sink->video_width;
    float crop_right = (float)sink->crop_right / sink->video_width;
    float crop_top = (float)sink->crop_top / sink->video_height;
    float crop_bottom = (float)sink->crop_bottom / sink->video_height;

    vVertices[2] += crop_left;
    vVertices[3] += crop_bottom;
    vVertices[6] -= crop_right;
    vVertices[7] += crop_bottom;
    vVertices[10] -= crop_right;
    vVertices[11] -= crop_top;
    vVertices[14] += crop_left;
    vVertices[15] -= crop_top;

    dst.x = 0;
    dst.y = 0;
    dst.w = sink->x11.width;
    dst.h = sink->x11.height;

    src.x = 0;
    src.y = 0;
    src.w = sink->video_width - sink->crop_left - sink->crop_right;
    src.h = sink->video_height - sink->crop_top - sink->crop_bottom;

    gst_video_sink_center_rect(src, dst, &result, TRUE);

    glUseProgram (gles->scale.program);
    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    glViewport (result.x, result.y, result.w, result.h);

    glClear (GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer (gles->scale.position_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), vVertices);

    glVertexAttribPointer (gles->scale.texcoord_loc, 2, GL_FLOAT,
        GL_FALSE, 4 * sizeof (GLfloat), &vVertices[2]);

    glEnableVertexAttribArray (gles->scale.position_loc);
    glEnableVertexAttribArray (gles->scale.texcoord_loc);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture (GL_TEXTURE_2D, gles->rgb_tex.id);
    glUniform1i (gles->rgb_tex.loc, 3);

    glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    eglSwapBuffers (gles->display, gles->surface);
}

/* EGL implementation */


static gint
egl_init (GstGLESSink *sink)
{
    const EGLint configAttribs[] =
    {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };

    const EGLint contextAttribs[] =
    {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    EGLint major;
    EGLint minor;

    GstGLESContext *gles = &sink->gl_thread.gles;

    GST_DEBUG_OBJECT (sink, "egl get display");
    gles->display = eglGetDisplay((EGLNativeDisplayType)
                                          sink->x11.display);
    if (gles->display == EGL_NO_DISPLAY) {
        GST_ERROR_OBJECT(sink, "Could not get EGL display");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl initialize");
    if (!eglInitialize(gles->display, &major, &minor)) {
        GST_ERROR_OBJECT(sink, "Could not initialize EGL context");
        return -1;
    }
    GST_DEBUG_OBJECT (sink, "Have EGL version: %d.%d", major, minor);

    GST_DEBUG_OBJECT (sink, "choose config");
    if (!eglChooseConfig(gles->display, configAttribs, &config, 1,
                        &num_configs)) {
        GST_ERROR_OBJECT(sink, "Could not choose EGL config");
        return -1;
    }

    if (num_configs != 1) {
        GST_WARNING_OBJECT(sink, "Did not get exactly one config, but %d",
                           num_configs);
    }

    GST_DEBUG_OBJECT (sink, "create window surface");
    gles->surface = eglCreateWindowSurface(gles->display, config,
                                     sink->x11.window, NULL);
    if (gles->surface == EGL_NO_SURFACE) {
        GST_ERROR_OBJECT (sink, "Could not create EGL surface");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl create context");
    gles->context = eglCreateContext(gles->display, config,
                                     EGL_NO_CONTEXT, contextAttribs);
    if (gles->context == EGL_NO_CONTEXT) {
        GST_ERROR_OBJECT(sink, "Could not create EGL context");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl make context current");
    if (!eglMakeCurrent(gles->display, gles->surface,
                        gles->surface, gles->context)) {
        GST_ERROR_OBJECT(sink, "Could not set EGL context to current");
        return -1;
    }

    GST_DEBUG_OBJECT (sink, "egl init done");

    return 0;
}

/*
 * ugly quirk, to workaround nvidia bugs
 * closes left open file handles
 */

static void
egl_close_file (GstGLESSink *sink, const gchar *filename)
{
    const gchar *target_file;
    GError *err = NULL;
    GFileInfo *info;
    GFile *file;

    GST_DEBUG_OBJECT (sink, "Check file handle: %s", filename);

    file = g_file_new_for_path (filename);
    info = g_file_query_info (file, "*",
                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                              NULL, &err);
    if (!info || err) {
        GST_ERROR_OBJECT (sink, "Could get file info: %s",
                         err->message);
        g_error_free(err);
        goto cleanup;
    }

    GST_DEBUG_OBJECT (sink, "File type is: %d",
                     g_file_info_get_file_type(info));

    if (!g_file_info_get_is_symlink (info)) {
        GST_DEBUG_OBJECT (sink, "File is no symlink");
        goto cleanup;
    }

    target_file = g_file_info_get_symlink_target (info);
    GST_DEBUG_OBJECT (sink, "Check file resolves to: '%s'",
                      target_file);

    if (g_str_equal (target_file, "/dev/tegra_sema") ||
        g_str_equal (target_file, "/dev/nvhost-gr2d") ||
        g_str_equal (target_file, "/dev/nvhost-gr3d"))
    {
        gchar *basename = g_file_get_basename (file);
        gint64 fid = g_ascii_strtoll (basename, NULL, 10);

        if (fid > 0) {
            GST_DEBUG_OBJECT (sink, "Close file handle %"
                              G_GINT64_FORMAT, fid);
            if (close (fid) < 0) {
                GST_ERROR_OBJECT (sink,
                                  "Could not close file handle: %d",
                                  errno);
            }
        }

        g_free (basename);
    }

cleanup:
    g_object_unref (info);
    g_object_unref (file);
}

static void
egl_close_handles (GstGLESSink *sink)
{
    GError *err = NULL;
    GDir *directory;
    const gchar *file;
    gchar *path;


    path = g_strdup_printf ("/proc/%u/fd", getpid());
    GST_DEBUG_OBJECT (sink, "Check for dead file handles in %s", path);

    directory = g_dir_open (path, 0, &err);
    if (!directory || err) {
        GST_ERROR_OBJECT(sink, "Could not list files: %s",
                         err->message);
        g_object_unref (err);
        goto cleanup;
    }

    while ((file = g_dir_read_name(directory))) {
        gchar *filename = g_strconcat(path, "/", file, NULL);
        egl_close_file (sink, filename);
        g_free (filename);
    }

cleanup:
    g_free (path);
    g_dir_close (directory);
}


static void
egl_close(GstGLESSink *sink)
{
    GstGLESContext *context = &sink->gl_thread.gles;

    const GLuint framebuffers[] = {
        context->framebuffer
    };

    const GLuint textures[] = {
        context->y_tex.id,
        context->u_tex.id,
        context->v_tex.id,
        context->rgb_tex.id
    };

    if (context->initialized) {
        glDeleteFramebuffers (G_N_ELEMENTS(framebuffers), framebuffers);
        glDeleteTextures (G_N_ELEMENTS(textures), textures);
        gl_delete_shader (&context->scale);
        gl_delete_shader (&context->deinterlace);
    }

    if (context->context) {
        eglDestroyContext (context->display, context->context);
        context->context = NULL;
    }

    if (context->surface) {
        eglDestroySurface (context->display, context->surface) ;
        context->surface = NULL;
    }

    if (context->display) {
        eglTerminate (context->display);
        context->display = NULL;
    }

    egl_close_handles (sink);

    context->initialized = FALSE;
}

static gint
x11_init (GstGLESSink *sink, gint width, gint height)
{
    Window root;
    XSetWindowAttributes swa;
    XWMHints hints;

    sink->x11.display = XOpenDisplay (NULL);
    if(!sink->x11.display) {
        GST_ERROR_OBJECT(sink, "Could not create X display");
        return -1;
    }

    XLockDisplay (sink->x11.display);
    root = DefaultRootWindow (sink->x11.display);
    swa.event_mask =
            StructureNotifyMask | ExposureMask | VisibilityChangeMask;

    if (!sink->x11.window) {
        sink->x11.window = XCreateWindow (
                    sink->x11.display, root,
                    0, 0, width, height, 0,
                    CopyFromParent, InputOutput,
                    CopyFromParent, CWEventMask,
                    &swa);

        XSetWindowBackgroundPixmap (sink->x11.display, sink->x11.window,
                                    None);

        hints.input = True;
        hints.flags = InputHint;
        XSetWMHints(sink->x11.display, sink->x11.window, &hints);

        XMapWindow (sink->x11.display, sink->x11.window);
        XStoreName (sink->x11.display, sink->x11.window, "GLESSink");
    } else {
        guint border, depth;
        int x, y;
        /* change event mask, so we get resize notifications */
        XSelectInput (sink->x11.display, sink->x11.window,
                      ExposureMask | StructureNotifyMask |
                      PointerMotionMask | KeyPressMask |
                      KeyReleaseMask);

        /* retrieve the current window geometry */
        XGetGeometry (sink->x11.display, sink->x11.window, &root,
                      &x, &y, (uint*)&sink->x11.width, (uint*)&sink->x11.height,
                      &border, &depth);
    }

    XUnlockDisplay (sink->x11.display);

    return 0;
}

static void
x11_close (GstGLESSink *sink)
{
    if (sink->x11.display) {
        XLockDisplay (sink->x11.display);

        /* only destroy the window if we created it, windows
          owned by the application stay untouched */
        if (!sink->x11.external_window) {
            XDestroyWindow (sink->x11.display, sink->x11.window);
            sink->x11.window = 0;
        } else
            XSelectInput (sink->x11.display, sink->x11.window, 0);

        XSync (sink->x11.display, FALSE);
        XUnlockDisplay (sink->x11.display);
        XCloseDisplay(sink->x11.display);
        sink->x11.display = NULL;
    }
}

static void
x11_handle_events (gpointer data)
{
    GstGLESSink *sink = GST_GLES_SINK (data);

    XLockDisplay (sink->x11.display);
    while (XPending (sink->x11.display)) {
        XEvent  xev;
        XNextEvent(sink->x11.display, &xev);

        switch (xev.type) {
        case ConfigureRequest:
            g_print("XConfigure* Request\n");
        case ConfigureNotify:
            GST_DEBUG_OBJECT(sink, "XConfigure* Event: wxh: %dx%d",
                             xev.xconfigure.width,
                             xev.xconfigure.height);
            g_print("XConfigure* Event: wxh: %dx%d\n",
                             xev.xconfigure.width,
                             xev.xconfigure.height);
            sink->x11.width = xev.xconfigure.width;
            sink->x11.height = xev.xconfigure.height;

            gl_draw_onscreen (sink);
            break;
        default:
            break;
        }
    }
    XUnlockDisplay (sink->x11.display);

}

static gboolean
gl_thread_init (GstGLESSink *sink)
{
    GstGLESThread *thread = &sink->gl_thread;
    GError *error = NULL;

    thread->handle = g_thread_try_new ("gl_thread", gl_thread_proc, sink, &error);
    if (!thread->handle) {
        GST_ERROR_OBJECT (sink, "Can't create render-thread: %s",
                          error ? error->message : "(unknown)");
        g_clear_error (&error);
        return FALSE;
    }
    return TRUE;
}

static void
gl_thread_stop (GstGLESSink *sink)
{
    if (sink->gl_thread.running) {
        sink->gl_thread.running = FALSE;
        g_mutex_lock (&sink->gl_thread.data_lock);
        sink->gl_thread.buf = NULL;

        g_cond_signal (&sink->gl_thread.data_signal);
        g_mutex_unlock (&sink->gl_thread.data_lock);

        g_thread_join(sink->gl_thread.handle);
    }
}

/* gl thread main function */
static gpointer
gl_thread_proc (gpointer data)
{
    GstGLESSink *sink = GST_GLES_SINK (data);
    GstGLESThread *thread = &sink->gl_thread;

    GST_DEBUG_OBJECT(sink, "Init GL context (no timedwait)");
    thread->running = setup_gl_context (sink) == 0;

    GST_DEBUG_OBJECT(sink, "Init GL context done, send signal");
    /* signal gst_gles_sink_render that we are done */
    g_mutex_lock (&thread->render_lock);
    g_cond_signal (&thread->render_signal);
    g_mutex_unlock (&thread->render_lock);

    while (thread->running) {
        x11_handle_events (sink);

        g_mutex_lock (&thread->data_lock);
        /* wait till gst_gles_sink_render has some data for us */
        while (!thread->buf && thread->running) {
            g_cond_wait (&thread->data_signal, &thread->data_lock);
        }

        if (thread->buf) {
            if (!thread->gles.initialized) {
                /* generate the framebuffer object */
                gl_gen_framebuffer (sink);
                thread->gles.initialized = TRUE;
            }

            XLockDisplay (sink->x11.display);
            gl_draw_fbo (sink, thread->buf);
            gl_draw_onscreen (sink);
            thread->buf = NULL;
            XUnlockDisplay (sink->x11.display);
	    thread->render_done = TRUE;
        }

        g_mutex_unlock (&thread->data_lock);

        /* signal gst_gles_sink_render that we are done */
        g_mutex_lock (&thread->render_lock);
        g_cond_signal (&thread->render_signal);
        g_mutex_unlock (&thread->render_lock);
    }

    egl_close(sink);
    x11_close(sink);
    return 0;
}

static gint
setup_gl_context (GstGLESSink *sink)
{
    GstGLESContext *gles = &sink->gl_thread.gles;
    gint ret;

    sink->x11.width = 720;
    sink->x11.height = 576;
    if (x11_init (sink, sink->x11.width, sink->x11.height) < 0) {
        GST_ERROR_OBJECT (sink, "X11 init failed, abort");
        return -ENOMEM;
    }

    if (egl_init (sink) < 0) {
        GST_ERROR_OBJECT (sink, "EGL init failed, abort");
        x11_close (sink);
        return -ENOMEM;
    }

    ret = gl_init_shader (GST_ELEMENT (sink), &gles->deinterlace,
                          SHADER_DEINT_LINEAR);
    if (ret < 0) {
        GST_ERROR_OBJECT (sink, "Could not initialize shader: %d", ret);
        egl_close (sink);
        x11_close (sink);
        return -ENOMEM;
    }
    gles->y_tex.loc = glGetUniformLocation(gles->deinterlace.program,
                                           "s_ytex");
    gles->u_tex.loc = glGetUniformLocation(gles->deinterlace.program,
                                           "s_utex");
    gles->v_tex.loc = glGetUniformLocation(gles->deinterlace.program,
                                           "s_vtex");

    ret = gl_init_shader (GST_ELEMENT (sink), &gles->scale, SHADER_COPY);
    if (ret < 0) {
        GST_ERROR_OBJECT (sink, "Could not initialize shader: %d", ret);
        egl_close (sink);
        x11_close (sink);
        return -ENOMEM;
    }
    gles->rgb_tex.loc = glGetUniformLocation(gles->scale.program, "s_tex");
    gl_init_textures (sink);

    /* finally announce the window handle to controling app */
    if (!sink->x11.external_window)
#if GST_CHECK_VERSION(1, 0, 0)
        gst_video_overlay_got_window_handle (GST_VIDEO_OVERLAY (sink),
                                         sink->x11.window);
#else
        gst_x_overlay_got_window_handle (GST_X_OVERLAY (sink),
                                         sink->x11.window);
#endif
    return 0;
}


#if !GST_CHECK_VERSION(1, 0, 0)
/* GObject vmethod implementations */

static void
gst_gles_sink_base_init (gpointer gclass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (gclass);

  gst_element_class_set_details_simple(element_class,
    "GLES sink",
    "Sink/Video",
    "Output video using Open GL ES 2.0",
    "Julian Scheel <julian jusst de>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gles_sink_factory));
}
#endif

/* initialize the plugin's class */
static void
gst_gles_sink_class_init (GstGLESSinkClass * klass)
{
  GstBaseSinkClass *basesink_class = GST_BASE_SINK_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gst_gles_sink_finalize;

  gobject_class->set_property = gst_gles_sink_set_property;
  gobject_class->get_property = gst_gles_sink_get_property;

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CROP_TOP,
      g_param_spec_uint ("crop_top", "Crop on top border", "Crop n pixels on top "
          "of the picture.", 0, G_MAXUINT, 0,
	  G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CROP_BOTTOM,
      g_param_spec_uint ("crop_bottom", "Crop on bottom border", "Crop n pixels on "
	"bottom of the picture.", 0, G_MAXUINT, 0,
	  G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CROP_LEFT,
      g_param_spec_uint ("crop_left", "Crop on bottom border", "Crop n pixels on "
	"left of the picture.", 0, G_MAXUINT, 0,
	  G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CROP_RIGHT,
      g_param_spec_uint ("crop_right", "Crop on right border", "Crop n pixels on "
	"right of the picture.", 0, G_MAXUINT, 0,
	  G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_DROP_FIRST,
      g_param_spec_uint ("drop_first", "Drop first n frames", "Before the "
	"first frame is drawn, drop n frames.", 0, G_MAXUINT, 0,
	  G_PARAM_READWRITE));

  /* initialise virtual methods */
  basesink_class->start = GST_DEBUG_FUNCPTR (gst_gles_sink_start);
  basesink_class->stop = GST_DEBUG_FUNCPTR (gst_gles_sink_stop);
  basesink_class->render = GST_DEBUG_FUNCPTR (gst_gles_sink_render);
  basesink_class->preroll = GST_DEBUG_FUNCPTR (gst_gles_sink_preroll);
  basesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_gles_sink_set_caps);

#if GST_CHECK_VERSION(1, 0, 0)
  gst_element_class_set_details_simple(element_class,
    "GLES sink",
    "Sink/Video",
    "Output video using Open GL ES 2.0",
    "Julian Scheel <julian jusst de>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gles_sink_factory));
#endif
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
#if GST_CHECK_VERSION(1, 0, 0)
static void
gst_gles_sink_init (GstGLESSink * sink)
#else
static void
gst_gles_sink_init (GstGLESSink * sink,
    GstGLESSinkClass * gclass)
#endif
{
    GstGLESThread *thread = &sink->gl_thread;
    Status ret;

    sink->silent = FALSE;
    sink->gl_thread.gles.initialized = FALSE;

    g_mutex_init(&thread->data_lock);
    g_mutex_init(&thread->render_lock);
    g_cond_init(&thread->data_signal);
    g_cond_init(&thread->render_signal);

    ret = XInitThreads();
    if (ret == 0) {
        GST_ERROR_OBJECT(sink, "XInitThreads failed");
    }

    gst_base_sink_set_max_lateness (GST_BASE_SINK (sink), 20 * GST_MSECOND);
    gst_base_sink_set_qos_enabled(GST_BASE_SINK (sink), TRUE);
}

static void
gst_gles_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLESSink *filter = GST_GLES_SINK (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    case PROP_CROP_TOP:
      filter->crop_top = g_value_get_uint (value);
      break;
    case PROP_CROP_BOTTOM:
      filter->crop_bottom = g_value_get_uint (value);
      break;
    case PROP_CROP_LEFT:
      filter->crop_left = g_value_get_uint (value);
      break;
    case PROP_CROP_RIGHT:
      filter->crop_right = g_value_get_uint (value);
      break;
    case PROP_DROP_FIRST:
      filter->drop_first = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gles_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLESSink *filter = GST_GLES_SINK (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    case PROP_CROP_TOP:
      g_value_set_uint (value, filter->crop_top);
      break;
    case PROP_CROP_BOTTOM:
      g_value_set_uint (value, filter->crop_bottom);
      break;
    case PROP_CROP_LEFT:
      g_value_set_uint (value, filter->crop_left);
      break;
    case PROP_CROP_RIGHT:
      g_value_set_uint (value, filter->crop_right);
      break;
    case PROP_DROP_FIRST:
      g_value_set_uint (value, filter->drop_first);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

/* initialisation code */
static gboolean
gst_gles_sink_start (GstBaseSink *basesink)
{
    return TRUE;
}

/* deinitialisation code */
static gboolean
gst_gles_sink_stop (GstBaseSink *basesink)
{
    GstGLESSink *sink = GST_GLES_SINK (basesink);

    gl_thread_stop (sink);

    GST_VIDEO_SINK_WIDTH (sink) = 0;
    GST_VIDEO_SINK_HEIGHT (sink)  = 0;

    return TRUE;
}

/* this function handles the link with other elements */
static gboolean
gst_gles_sink_set_caps (GstBaseSink *basesink, GstCaps *caps)
{
  GstGLESSink *sink = GST_GLES_SINK (basesink);
  GstVideoFormat fmt;
  guint display_par_n;
  guint display_par_d;
  gint par_n;
  gint par_d;
  gint w;
  gint h;

#if GST_CHECK_VERSION(1, 0, 0)
  GstVideoInfo info;

  if (!gst_video_info_from_caps (&info, caps)) {
      GST_WARNING_OBJECT (sink, "Failed to read video info from caps");
      return FALSE;
  }

  fmt = GST_VIDEO_INFO_FORMAT(&info);
  w = info.width;
  h = info.height;
  par_n = info.par_n;
  par_d = info.par_d;
#else
  if (!gst_video_format_parse_caps (caps, &fmt, &w, &h)) {
      GST_WARNING_OBJECT (sink, "pase_caps failed");
      return FALSE;
  }

  /* retrieve pixel aspect ratio of encoded video */
  if (!gst_video_parse_caps_pixel_aspect_ratio (caps, &par_n, &par_d)) {
      GST_WARNING_OBJECT (sink, "no pixel aspect ratio");
      return FALSE;
  }
#endif
  g_assert ((fmt == GST_VIDEO_FORMAT_I420));

  sink->video_width = w;
  sink->video_height = h;
  GST_VIDEO_SINK_WIDTH (sink) = w;
  GST_VIDEO_SINK_HEIGHT (sink) = h;

  /* calculate actual rendering pixel aspect ratio based on video pixel
   * aspect ratio and display pixel aspect ratio */
  /* FIXME: add display pixel aspect ratio as property to the plugin */
  display_par_n = 1;
  display_par_d = 1;

  gst_video_calculate_display_ratio ((guint*)&sink->par_n,
                                     (guint*)&sink->par_d,
                                     GST_VIDEO_SINK_WIDTH (sink),
                                     GST_VIDEO_SINK_HEIGHT (sink),
                                     (guint) par_n, (guint) par_d,
                                     display_par_n, display_par_d);

  sink->video_width = sink->video_width * par_n / par_d;

  return TRUE;
}

static GstFlowReturn
gst_gles_sink_preroll (GstBaseSink * basesink, GstBuffer * buf)
{
    GstGLESSink *sink = GST_GLES_SINK (basesink);
    GstGLESThread *thread = &sink->gl_thread;

    if (!thread->running) {
        /* give the application the opportunity to head in a
           xwindow id to use as render target */
#if GST_CHECK_VERSION(1, 0, 0)
        gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (sink));
#else
        gst_x_overlay_prepare_xwindow_id (GST_X_OVERLAY (sink));
#endif

        g_mutex_lock (&thread->render_lock);
        if (!gl_thread_init (sink)) {
            g_mutex_unlock (&thread->render_lock);
            goto fail;
        }
        GST_DEBUG_OBJECT(sink, "Wait for init GL context");
	if (!thread->running) {
            g_cond_wait (&thread->render_signal, &thread->render_lock);
            g_mutex_unlock (&thread->render_lock);
	}
        GST_DEBUG_OBJECT(sink, "Init completed");
    }

    if (sink->dropped < sink->drop_first) {
        sink->dropped++;
        goto done;
    }

    g_mutex_lock (&thread->data_lock);
    thread->render_done = FALSE;
    thread->buf = buf;
    g_cond_signal (&thread->data_signal);
    g_mutex_unlock (&thread->data_lock);

    if (!thread->render_done) {
        g_mutex_lock (&thread->render_lock);
        g_cond_wait (&thread->render_signal, &thread->render_lock);
        g_mutex_unlock (&thread->render_lock);
    }

done:
    return GST_FLOW_OK;
fail:
    GST_ELEMENT_ERROR (sink, LIBRARY, INIT, ("Can't create render-thread"),
                       GST_ERROR_SYSTEM);
    return GST_FLOW_ERROR;
}

static GstFlowReturn
gst_gles_sink_render (GstBaseSink *basesink, GstBuffer *buf)
{
    GstGLESSink *sink = GST_GLES_SINK (basesink);
    GstGLESThread *thread = &sink->gl_thread;

    GstClockTime start, stop;

    start = gst_util_get_timestamp();

    if (sink->dropped < sink->drop_first) {
        sink->dropped++;
        goto done;
    }

    g_mutex_lock (&thread->data_lock);
    thread->render_done = FALSE;
    thread->buf = buf;
    g_cond_signal (&thread->data_signal);
    g_mutex_unlock (&thread->data_lock);

    if (!thread->render_done) {
        g_mutex_lock (&thread->render_lock);
        g_cond_wait (&thread->render_signal, &thread->render_lock);
        g_mutex_unlock (&thread->render_lock);
    }

done:
    stop = gst_util_get_timestamp();
    GST_DEBUG_OBJECT (basesink, "Render took %llu ms",
                        stop/GST_MSECOND - start/GST_MSECOND);

    return GST_FLOW_OK;
}

static void
gst_gles_sink_finalize (GObject *gobject)
{
    GstGLESSink *plugin = (GstGLESSink *)gobject;

    gl_thread_stop (plugin);
}

/* Overlay Interface implementation */
#if GST_CHECK_VERSION(1, 0, 0)
static void
gst_gles_video_overlay_set_handle (GstVideoOverlay *overlay, guintptr handle)
#else
static void
gst_gles_xoverlay_set_window_handle (GstXOverlay *overlay, guintptr handle)
#endif
{
    GstGLESSink *sink = GST_GLES_SINK (overlay);

    /* if we have not created a window yet, we'll use the application
      provided one. runtime switching is not yet supported */
    GST_DEBUG_OBJECT (sink, "Setting window handle");
    if (sink->x11.window == 0) {
        GST_DEBUG_OBJECT (sink, "register new window id: %d", handle);
        sink->x11.window = handle;
        sink->x11.external_window = TRUE;
    } else {
        GST_ERROR_OBJECT (sink, "Changing window handle is not yet supported.");
    }
}

#if GST_CHECK_VERSION(1, 0, 0)
static void
gst_gles_video_overlay_init (GstVideoOverlayInterface * iface)
{
    iface->set_window_handle = gst_gles_video_overlay_set_handle;
}
#else
static void
gst_gles_xoverlay_interface_init (GstXOverlayClass *overlay_klass)
{
    overlay_klass->set_window_handle = gst_gles_xoverlay_set_window_handle;
}
#endif

#if !GST_CHECK_VERSION(1, 0, 0)
static gboolean
gst_gles_xoverlay_supported (GstGLESSink *sink,
                             GType iface_type)
{
    GST_DEBUG_OBJECT(sink, "Interface XOverlay supprted");
    g_return_val_if_fail (iface_type == GST_TYPE_X_OVERLAY, FALSE);

    return TRUE;
}
#endif

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
plugin_init (GstPlugin * plugin)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template plugin' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_gles_sink_debug, "glesplugin",
      0, "OpenGL ES 2.0 plugin");

  return gst_element_register (plugin, "glessink", GST_RANK_NONE,
      GST_TYPE_GLES_SINK);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "glesplugin"
#endif

/* gstreamer looks for this structure to register plugins
 *
 * exchange the string 'Template plugin' with your plugin description
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
#if GST_CHECK_VERSION(1, 0, 0)
    glesplugin,
#else
    "glesplugin",
#endif
    "Open GL ES 2.0 plugin",
    plugin_init,
    VERSION,
    "LGPL",
    "Avionic Design",
    "http://avionic-design.de/"
)
