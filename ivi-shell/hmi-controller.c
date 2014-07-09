/*
 * Copyright (C) 2014 DENSO CORPORATION
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * A reference implementation how to use ivi-layout APIs in order to manage
 * layout of surfaces/layers. Layout change is triggered by ivi-hmi-controller
 * protocol, ivi-hmi-controller.xml. A reference how to use the protocol, see
 * hmi-controller-homescreen.
 *
 * In-Vehicle Infotainment system usually manage properties of surfaces/layers
 * by only a central component which decide where surfaces/layers shall be. This
 * reference show examples to implement the central component as a module of weston.
 *
 * Default Scene graph of UI is defined in hmi_controller_create. It consists of
 * - In the bottom, a base layer to group surfaces of background, panel,
 *   and buttons
 * - Next, a application layer to show application surfaces.
 * - Workspace background layer to show a surface of background image.
 * - Workspace layer to show launcher to launch application with icons. Paths to
 *   binary and icon are defined in weston.ini. The width of this layer is longer
 *   than the size of screen because a workspace has several pages and is controlled
 *   by motion of input.
 *
 * TODO: animation method shall be refined
 * TODO: support fade-in when UI is ready
 */

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <assert.h>
#include <time.h>

#include "ivi-layout-export.h"
#include "ivi-hmi-controller-server-protocol.h"

/*****************************************************************************
 *  structure, globals
 ****************************************************************************/
struct hmi_controller_layer {
    struct ivi_layout_layer  *ivilayer;
    uint32_t id_layer;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct link_layer {
    struct ivi_layout_layer *layout_layer;
    struct wl_list link;
};

struct link_animation {
    struct hmi_controller_animation *animation;
    struct wl_list link;
};

struct hmi_controller_animation;
typedef void (*hmi_controller_animation_frame_func)(void *animation, uint32_t timestamp);
typedef void (*hmi_controller_animation_frame_user_func)(void *animation);
typedef void (*hmi_controller_animation_destroy_func)(struct hmi_controller_animation *animation);

struct move_animation_user_data {
    struct ivi_layout_layer* layer;
    struct animation_set *anima_set;
    struct hmi_controller *hmi_ctrl;
};

struct hmi_controller_animation {
    void *user_data;
    uint32_t  time_start;
    uint32_t  is_done;
    hmi_controller_animation_frame_func frame_func;
    hmi_controller_animation_frame_user_func frame_user_func;
    hmi_controller_animation_destroy_func destroy_func;
};

struct hmi_controller_animation_fade {
    struct hmi_controller_animation base;
    double start;
    double end;
    struct weston_spring spring;
};

struct hmi_controller_animation_move {
    struct hmi_controller_animation base;
    double pos;
    double pos_start;
    double pos_end;
    double v0;
    double a;
    double time_end;
};

struct hmi_controller_fade {
    uint32_t isFadeIn;
    struct hmi_controller_animation_fade *animation;
    struct animation_set *anima_set;
    struct wl_list layer_list;
};

struct animation_set {
    struct wl_event_source  *event_source;
    struct wl_list          animation_list;
};

struct
hmi_server_setting {
    uint32_t    base_layer_id;
    uint32_t    application_layer_id;
    uint32_t    workspace_background_layer_id;
    uint32_t    workspace_layer_id;
    uint32_t    panel_height;
    char       *ivi_homescreen;
};

struct hmi_controller
{
    struct hmi_server_setting          *hmi_setting;
    struct hmi_controller_layer         base_layer;
    struct hmi_controller_layer         application_layer;
    struct hmi_controller_layer         workspace_background_layer;
    struct hmi_controller_layer         workspace_layer;
    enum ivi_hmi_controller_layout_mode layout_mode;

    struct animation_set                    *anima_set;
    struct hmi_controller_fade              workspace_fade;
    struct hmi_controller_animation_move    *workspace_swipe_animation;
    int32_t                                 workspace_count;
    struct wl_array                     ui_widgets;
    int32_t                             is_initialized;

    struct weston_compositor           *compositor;
    struct weston_process               process;
    struct wl_listener                  destroy_listener;
};

struct launcher_info
{
    uint32_t surface_id;
    uint32_t workspace_id;
    uint32_t index;
};

/*****************************************************************************
 *  local functions
 ****************************************************************************/
static void *
fail_on_null(void *p, size_t size, char* file, int32_t line)
{
    if (size && !p) {
        fprintf(stderr, "%s(%d) %zd: out of memory\n", file, line, size);
        exit(EXIT_FAILURE);
    }

    return p;
}

static void *
mem_alloc(size_t size, char* file, int32_t line)
{
    return fail_on_null(calloc(1, size), size, file, line);
}

#define MEM_ALLOC(s) mem_alloc((s),__FILE__,__LINE__)

static int32_t
is_surf_in_uiWidget(struct hmi_controller *hmi_ctrl,
                         struct ivi_layout_surface *ivisurf)
{
    uint32_t id = ivi_layout_getIdOfSurface(ivisurf);

    uint32_t *ui_widget_id = NULL;
    wl_array_for_each (ui_widget_id, &hmi_ctrl->ui_widgets) {
        if (*ui_widget_id == id) {
            return 1;
        }
    }

    return 0;
}

static int
compare_launcher_info(const void *lhs, const void *rhs)
{
    const struct launcher_info *left = (const struct launcher_info *)lhs;
    const struct launcher_info *right = (const struct launcher_info *)rhs;

    if (left->workspace_id < right->workspace_id) {
        return -1;
    }

    if (left->workspace_id > right->workspace_id) {
        return 1;
    }

    if (left->index < right->index) {
        return -1;
    }

    if (left->index > right->index) {
        return 1;
    }

    return 0;
}

/**
 * Internal methods called by mainly ivi_hmi_controller_switch_mode
 * This reference shows 4 examples how to use ivi_layout APIs.
 */
static void
mode_divided_into_tiling(struct hmi_controller *hmi_ctrl,
                        struct ivi_layout_surface **ppSurface,
                        uint32_t surface_length,
                        struct hmi_controller_layer *layer)
{
    const float surface_width  = (float)layer->width * 0.25;
    const float surface_height = (float)layer->height * 0.5;
    int32_t surface_x = 0;
    int32_t surface_y = 0;
    struct ivi_layout_surface *ivisurf  = NULL;
    int32_t ret = 0;

    uint32_t i = 0;
    uint32_t num = 1;
    for (i = 0; i < surface_length; i++) {
        ivisurf = ppSurface[i];

        /* skip ui widgets */
        if (is_surf_in_uiWidget(hmi_ctrl, ivisurf)) {
            continue;
        }

        if (num <= 8) {
            if (num < 5) {
                surface_x = (int32_t)((num - 1) * (surface_width));
                surface_y = 0;
            }
            else {
                surface_x = (int32_t)((num - 5) * (surface_width));
                surface_y = (int32_t)surface_height;
            }
            ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf, surface_x, surface_y,
                                                            surface_width, surface_height);
            assert(!ret);

            ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
            assert(!ret);

            num++;
            continue;
        }

        ret = ivi_layout_surfaceSetVisibility(ivisurf, 0);
        assert(!ret);
    }
}

static void
mode_divided_into_sidebyside(struct hmi_controller *hmi_ctrl,
                      struct ivi_layout_surface **ppSurface,
                      uint32_t surface_length,
                      struct hmi_controller_layer *layer)
{
    uint32_t surface_width  = layer->width / 2;
    uint32_t surface_height = layer->height;
    struct ivi_layout_surface *ivisurf  = NULL;
    int32_t ret = 0;

    uint32_t i = 0;
    uint32_t num = 1;
    for (i = 0; i < surface_length; i++) {
        ivisurf = ppSurface[i];

        /* skip ui widgets */
        if (is_surf_in_uiWidget(hmi_ctrl, ivisurf)) {
            continue;
        }

        if (num == 1) {
            ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf, 0, 0,
                                                surface_width, surface_height);
            assert(!ret);

            ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
            assert(!ret);

            num++;
            continue;
        }
        else if (num == 2) {
            ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf, surface_width, 0,
                                                surface_width, surface_height);
            assert(!ret);

            ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
            assert(!ret);

            num++;
            continue;
        }

        ivi_layout_surfaceSetVisibility(ivisurf, 0);
        assert(!ret);
    }
}

static void
mode_fullscreen_someone(struct hmi_controller *hmi_ctrl,
                      struct ivi_layout_surface **ppSurface,
                      uint32_t surface_length,
                      struct hmi_controller_layer *layer)
{
    const uint32_t  surface_width  = layer->width;
    const uint32_t  surface_height = layer->height;
    struct ivi_layout_surface *ivisurf  = NULL;
    int32_t ret = 0;

    uint32_t i = 0;
    for (i = 0; i < surface_length; i++) {
        ivisurf = ppSurface[i];

        /* skip ui widgets */
        if (is_surf_in_uiWidget(hmi_ctrl, ivisurf)) {
            continue;
        }

        ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf, 0, 0,
                                            surface_width, surface_height);
        assert(!ret);

        ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
        assert(!ret);
    }
}

static void
mode_random_replace(struct hmi_controller *hmi_ctrl,
                    struct ivi_layout_surface **ppSurface,
                    uint32_t surface_length,
                    struct hmi_controller_layer *layer)
{
    const uint32_t surface_width  = (uint32_t)(layer->width * 0.25f);
    const uint32_t surface_height = (uint32_t)(layer->height * 0.25f);
    uint32_t surface_x = 0;
    uint32_t surface_y = 0;
    struct ivi_layout_surface *ivisurf  = NULL;
    int32_t ret = 0;

    uint32_t i = 0;
    for (i = 0; i < surface_length; i++) {
        ivisurf = ppSurface[i];

        /* skip ui widgets */
        if (is_surf_in_uiWidget(hmi_ctrl, ivisurf)) {
            continue;
        }

        surface_x = rand() % (layer->width - surface_width);
        surface_y = rand() % (layer->height - surface_height);

        ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf, surface_x, surface_y,
                                                  surface_width, surface_height);
        assert(!ret);

        ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
        assert(!ret);
    }
}

static int32_t
has_applicatipn_surface(struct hmi_controller *hmi_ctrl,
                        struct ivi_layout_surface **ppSurface,
                        uint32_t surface_length)
{
    struct ivi_layout_surface *ivisurf  = NULL;
    uint32_t i = 0;

    for (i = 0; i < surface_length; i++) {
        ivisurf = ppSurface[i];

        /* skip ui widgets */
        if (is_surf_in_uiWidget(hmi_ctrl, ivisurf)) {
            continue;
        }

        return 1;
    }

    return 0;
}

/**
 * Supports 4 example to layout of application surfaces;
 * tiling, side by side, fullscreen, and random.
 */
static void
switch_mode(struct hmi_controller *hmi_ctrl,
            enum ivi_hmi_controller_layout_mode layout_mode)
{
    if (!hmi_ctrl->is_initialized) {
        return;
    }

    struct hmi_controller_layer *layer = &hmi_ctrl->application_layer;
    struct ivi_layout_surface **ppSurface = NULL;
    uint32_t surface_length = 0;
    int32_t ret = 0;

    hmi_ctrl->layout_mode = layout_mode;

    ret = ivi_layout_getSurfaces(&surface_length, &ppSurface);
    assert(!ret);

    if (!has_applicatipn_surface(hmi_ctrl, ppSurface, surface_length)) {
        free(ppSurface);
        ppSurface = NULL;
        return;
    }

    switch (layout_mode) {
    case IVI_HMI_CONTROLLER_LAYOUT_MODE_TILING:
        mode_divided_into_tiling(hmi_ctrl, ppSurface, surface_length, layer);
        break;
    case IVI_HMI_CONTROLLER_LAYOUT_MODE_SIDE_BY_SIDE:
        mode_divided_into_sidebyside(hmi_ctrl, ppSurface, surface_length, layer);
        break;
    case IVI_HMI_CONTROLLER_LAYOUT_MODE_FULL_SCREEN:
        mode_fullscreen_someone(hmi_ctrl, ppSurface, surface_length, layer);
        break;
    case IVI_HMI_CONTROLLER_LAYOUT_MODE_RANDOM:
        mode_random_replace(hmi_ctrl, ppSurface, surface_length, layer);
        break;
    }

    ivi_layout_commitChanges();

    free(ppSurface);
    ppSurface = NULL;

    return;
}

/**
 * Internal method for animation
 */
static void
hmi_controller_animation_frame(
    struct hmi_controller_animation *animation, uint32_t timestamp)
{
    if (0 == animation->time_start) {
        animation->time_start = timestamp;
    }

    animation->frame_func(animation, timestamp);
    animation->frame_user_func(animation);
}

static int
animation_set_do_anima(void* data)
{
    struct animation_set *anima_set = data;
    uint32_t fps = 30;

    if (wl_list_empty(&anima_set->animation_list)) {
        wl_event_source_timer_update(anima_set->event_source, 0);
        return 1;
    }

    wl_event_source_timer_update(anima_set->event_source, 1000 / fps);

    struct timespec timestamp = {0};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    uint32_t msec = (1e+3 * timestamp.tv_sec + 1e-6 * timestamp.tv_nsec);

    struct link_animation *link_animation = NULL;
    struct link_animation *next = NULL;

    wl_list_for_each_safe(link_animation, next, &anima_set->animation_list, link) {
        hmi_controller_animation_frame(link_animation->animation, msec);
    }

    ivi_layout_commitChanges();
    return 1;
}

static struct animation_set *
animation_set_create(struct weston_compositor* ec)
{
    struct animation_set *anima_set = MEM_ALLOC(sizeof(*anima_set));

    wl_list_init(&anima_set->animation_list);

    struct wl_event_loop *loop = wl_display_get_event_loop(ec->wl_display);
    anima_set->event_source = wl_event_loop_add_timer(loop, animation_set_do_anima, anima_set);
    wl_event_source_timer_update(anima_set->event_source, 0);

    return anima_set;
}

static void
animation_set_add_animation(struct animation_set *anima_set,
                            struct hmi_controller_animation *anima)
{
    struct link_animation *link_anima = NULL;

    link_anima = MEM_ALLOC(sizeof(*link_anima));
    if (NULL == link_anima) {
        return;
    }

    link_anima->animation = anima;
    wl_list_insert(&anima_set->animation_list, &link_anima->link);
    wl_event_source_timer_update(anima_set->event_source, 1);
}

static void
animation_set_remove_animation(struct animation_set *anima_set,
                               struct hmi_controller_animation *anima)
{
    struct link_animation *link_animation = NULL;
    struct link_animation *next = NULL;

    wl_list_for_each_safe(link_animation, next, &anima_set->animation_list, link) {
        if (link_animation->animation == anima) {
            wl_list_remove(&link_animation->link);
            free(link_animation);
            break;
        }
    }
}

static void
hmi_controller_animation_spring_frame(
    struct hmi_controller_animation_fade *animation, uint32_t timestamp)
{
    if (0 == animation->spring.timestamp) {
        animation->spring.timestamp = timestamp;
    }

    weston_spring_update(&animation->spring, timestamp);
    animation->base.is_done = weston_spring_done(&animation->spring);
}

static void
hmi_controller_animation_move_frame(
    struct hmi_controller_animation_move *animation, uint32_t timestamp)
{
    double s = animation->pos_start;
    double t = timestamp - animation->base.time_start;
    double v0 = animation->v0;
    double a = animation->a;
    double time_end = animation->time_end;

    if (time_end <= t) {
        animation->pos = animation->pos_end;
        animation->base.is_done = 1;
    } else {
        animation->pos = v0 * t + 0.5 * a * t * t + s;
    }
}

static void
hmi_controller_animation_destroy(struct hmi_controller_animation *animation)
{
    if (animation->destroy_func) {
        animation->destroy_func(animation);
    }

    free(animation);
}

static void
hmi_controller_fade_animation_destroy(struct hmi_controller_animation *animation)
{
    struct hmi_controller_fade *fade = animation->user_data;
    animation_set_remove_animation(fade->anima_set, animation);
    fade->animation = NULL;
    animation->user_data = NULL;
}

static struct hmi_controller_animation_fade *
hmi_controller_animation_fade_create(double start, double end, double k,
    hmi_controller_animation_frame_user_func frame_user_func, void* user_data,
    hmi_controller_animation_destroy_func destroy_func)
{
    struct hmi_controller_animation_fade* animation = MEM_ALLOC(sizeof(*animation));

    animation->base.frame_user_func = frame_user_func;
    animation->base.user_data = user_data;
    animation->base.frame_func =
        (hmi_controller_animation_frame_func)hmi_controller_animation_spring_frame;
    animation->base.destroy_func = destroy_func;

    animation->start = start;
    animation->end = end;
    weston_spring_init(&animation->spring, k, start, end);
    animation->spring.friction = 1400;
    animation->spring.previous = -(end - start) * 0.03;

    return animation;
}

static struct hmi_controller_animation_move *
hmi_controller_animation_move_create(
    double pos_start, double pos_end, double v_start, double v_end,
    hmi_controller_animation_frame_user_func frame_user_func, void* user_data,
    hmi_controller_animation_destroy_func destroy_func)
{
    struct hmi_controller_animation_move* animation = MEM_ALLOC(sizeof(*animation));

    animation->base.frame_user_func = frame_user_func;
    animation->base.user_data = user_data;
    animation->base.frame_func =
        (hmi_controller_animation_frame_func)hmi_controller_animation_move_frame;
    animation->base.destroy_func = destroy_func;

    animation->pos_start = pos_start;
    animation->pos_end = pos_end;
    animation->v0 = v_start;
    animation->pos = pos_start;

    double dx = (pos_end - pos_start);

    if (1e-3 < fabs(dx)) {
        animation->a = 0.5 * (v_end * v_end - v_start * v_start) / dx;
        if (1e-6 < fabs(animation->a)) {
            animation->time_end = (v_end - v_start) / animation->a;

        } else {
            animation->a = 0;
            animation->time_end = fabs(dx / animation->v0);
        }

    } else {
        animation->time_end = 0;
    }

    return animation;
}

static double
hmi_controller_animation_fade_alpha_get(struct hmi_controller_animation_fade* animation)
{
    if (animation->spring.current > 0.999) {
        return 1.0;
    } else if (animation->spring.current < 0.001 ) {
        return 0.0;
    } else {
        return animation->spring.current;
    }
}

static uint32_t
hmi_controller_animation_is_done(struct hmi_controller_animation *animation)
{
    return animation->is_done;
}

static void
hmi_controller_fade_update(struct hmi_controller_animation_fade *animation, double end)
{
    animation->spring.target = end;
}

static void
hmi_controller_anima_fade_user_frame(struct hmi_controller_animation_fade *animation)
{
    double alpha = hmi_controller_animation_fade_alpha_get(animation);
    alpha = wl_fixed_from_double(alpha);
    struct hmi_controller_fade *fade = animation->base.user_data;
    struct link_layer *linklayer = NULL;
    int32_t is_done = hmi_controller_animation_is_done(&animation->base);
    int32_t is_visible = !is_done || fade->isFadeIn;

    wl_list_for_each(linklayer, &fade->layer_list, link) {
        ivi_layout_layerSetOpacity(linklayer->layout_layer, alpha);
        ivi_layout_layerSetVisibility(linklayer->layout_layer, is_visible);
    }

    if (is_done) {
        hmi_controller_animation_destroy(&animation->base);
    }
}

static void
hmi_controller_anima_move_user_frame(struct hmi_controller_animation_move *animation)
{
    struct move_animation_user_data* user_data = animation->base.user_data;
    struct ivi_layout_layer *layer = user_data->layer;
    int32_t is_done = hmi_controller_animation_is_done(&animation->base);

    int32_t pos[2] = {0};
    ivi_layout_layerGetPosition(layer, pos);

    pos[0] = (int32_t)animation->pos;
    ivi_layout_layerSetPosition(layer, pos);

    if (is_done) {
        hmi_controller_animation_destroy(&animation->base);
    }
}

static void
hmi_controller_fade_run(uint32_t isFadeIn, struct hmi_controller_fade *fade)
{
    double tint = isFadeIn ? 1.0 : 0.0;
    fade->isFadeIn = isFadeIn;

    if (fade->animation) {
        hmi_controller_fade_update(fade->animation, tint);
    } else {
        fade->animation = hmi_controller_animation_fade_create(
            1.0 - tint, tint, 300.0,
            (hmi_controller_animation_frame_user_func)hmi_controller_anima_fade_user_frame,
            fade, hmi_controller_fade_animation_destroy);

        animation_set_add_animation(fade->anima_set, &fade->animation->base);
    }
}

/**
 * Internal method to create layer with hmi_controller_layer and add to a screen
 */
static void
create_layer(struct ivi_layout_screen  *iviscrn,
             struct hmi_controller_layer *layer)
{
    int32_t ret = 0;

    layer->ivilayer = ivi_layout_layerCreateWithDimension(layer->id_layer,
                                                layer->width, layer->height);
    assert(layer->ivilayer != NULL);

    ret = ivi_layout_screenAddLayer(iviscrn, layer->ivilayer);
    assert(!ret);

    ret = ivi_layout_layerSetDestinationRectangle(layer->ivilayer, layer->x, layer->y,
                                                  layer->width, layer->height);
    assert(!ret);

    ret = ivi_layout_layerSetVisibility(layer->ivilayer, 1);
    assert(!ret);
}

/**
 * Internal set notification
 */
static void
set_notification_create_surface(struct ivi_layout_surface *ivisurf,
                                void *userdata)
{
    struct hmi_controller* hmi_ctrl = userdata;
    struct ivi_layout_layer *application_layer = hmi_ctrl->application_layer.ivilayer;
    int32_t ret = 0;

    /* skip ui widgets */
    if (is_surf_in_uiWidget(hmi_ctrl, ivisurf)) {
        return;
    }

    ret = ivi_layout_layerAddSurface(application_layer, ivisurf);
    assert(!ret);
}

static void
set_notification_remove_surface(struct ivi_layout_surface *ivisurf,
                                void *userdata)
{
    (void)ivisurf;
    struct hmi_controller* hmi_ctrl = userdata;
    switch_mode(hmi_ctrl, hmi_ctrl->layout_mode);
}

static void
set_notification_configure_surface(struct ivi_layout_surface *ivisurf,
                                   void *userdata)
{
    (void)ivisurf;
    struct hmi_controller* hmi_ctrl = userdata;
    switch_mode(hmi_ctrl, hmi_ctrl->layout_mode);
}

/**
 * A hmi_controller used 4 layers to manage surfaces. The IDs of corresponding layer
 * are defined in weston.ini. Default scene graph of layers are initialized in
 * hmi_controller_create
 */
static struct hmi_server_setting *
hmi_server_setting_create(void)
{
    struct hmi_server_setting* setting = MEM_ALLOC(sizeof(*setting));

    struct weston_config *config = NULL;
    config = weston_config_parse("weston.ini");

    struct weston_config_section *shellSection = NULL;
    shellSection = weston_config_get_section(config, "ivi-shell", NULL, NULL);

    weston_config_section_get_uint(
        shellSection, "base-layer-id", &setting->base_layer_id, 1000);

    weston_config_section_get_uint(
        shellSection, "workspace-background-layer-id", &setting->workspace_background_layer_id, 2000);

    weston_config_section_get_uint(
        shellSection, "workspace-layer-id", &setting->workspace_layer_id, 3000);

    weston_config_section_get_uint(
        shellSection, "application-layer-id", &setting->application_layer_id, 4000);

    setting->panel_height = 70;

    weston_config_section_get_string(
            shellSection, "ivi-shell-user-interface", &setting->ivi_homescreen, NULL);

    weston_config_destroy(config);

    return setting;
}

/**
 * This is a starting method called from module_init.
 * This sets up scene graph of layers; base, application, workspace background,
 * and workspace. These layers are created/added to screen in create_layer
 *
 * base: to group surfaces of panel and background
 * application: to group surfaces of ivi_applications
 * workspace background: to group a surface of background in workspace
 * workspace: to group surfaces for launching ivi_applications
 *
 * Layers of workspace background and workspace is set to invisible at first.
 * The properties of it is updated with animation when ivi_hmi_controller_home is
 * requested.
 */
static struct hmi_controller *
hmi_controller_create(struct weston_compositor *ec)
{
    struct ivi_layout_screen **ppScreen = NULL;
    struct ivi_layout_screen *iviscrn  = NULL;
    uint32_t screen_length  = 0;
    uint32_t screen_width   = 0;
    uint32_t screen_height  = 0;
    int32_t ret = 0;
    struct link_layer *tmp_link_layer = NULL;

    struct hmi_controller *hmi_ctrl = MEM_ALLOC(sizeof(*hmi_ctrl));
    wl_array_init(&hmi_ctrl->ui_widgets);
    hmi_ctrl->layout_mode = IVI_HMI_CONTROLLER_LAYOUT_MODE_TILING;
    hmi_ctrl->hmi_setting = hmi_server_setting_create();

    ivi_layout_getScreens(&screen_length, &ppScreen);

    iviscrn = ppScreen[0];

    ivi_layout_getScreenResolution(iviscrn, &screen_width, &screen_height);
    assert(!ret);

    /* init base layer*/
    hmi_ctrl->base_layer.x = 0;
    hmi_ctrl->base_layer.y = 0;
    hmi_ctrl->base_layer.width = screen_width;
    hmi_ctrl->base_layer.height = screen_height;
    hmi_ctrl->base_layer.id_layer = hmi_ctrl->hmi_setting->base_layer_id;

    create_layer(iviscrn, &hmi_ctrl->base_layer);

    uint32_t panel_height = hmi_ctrl->hmi_setting->panel_height;


    /* init application layer */
    hmi_ctrl->application_layer.x = 0;
    hmi_ctrl->application_layer.y = 0;
    hmi_ctrl->application_layer.width = screen_width;
    hmi_ctrl->application_layer.height = screen_height - panel_height;
    hmi_ctrl->application_layer.id_layer = hmi_ctrl->hmi_setting->application_layer_id;

    create_layer(iviscrn, &hmi_ctrl->application_layer);

    /* init workspace background layer */
    hmi_ctrl->workspace_background_layer.x = 0;
    hmi_ctrl->workspace_background_layer.y = 0;
    hmi_ctrl->workspace_background_layer.width = screen_width;
    hmi_ctrl->workspace_background_layer.height = screen_height - panel_height;

    hmi_ctrl->workspace_background_layer.id_layer =
        hmi_ctrl->hmi_setting->workspace_background_layer_id;

    create_layer(iviscrn, &hmi_ctrl->workspace_background_layer);
    ivi_layout_layerSetOpacity(hmi_ctrl->workspace_background_layer.ivilayer, 0);
    ivi_layout_layerSetVisibility(hmi_ctrl->workspace_background_layer.ivilayer, 0);

    /* init workspace layer */
    hmi_ctrl->workspace_layer.x = hmi_ctrl->workspace_background_layer.x;
    hmi_ctrl->workspace_layer.y = hmi_ctrl->workspace_background_layer.y;
    hmi_ctrl->workspace_layer.width = hmi_ctrl->workspace_background_layer.width;
    hmi_ctrl->workspace_layer.height = hmi_ctrl->workspace_background_layer.height;
    hmi_ctrl->workspace_layer.id_layer = hmi_ctrl->hmi_setting->workspace_layer_id;

    create_layer(iviscrn, &hmi_ctrl->workspace_layer);
    ivi_layout_layerSetOpacity(hmi_ctrl->workspace_layer.ivilayer, 0);
    ivi_layout_layerSetVisibility(hmi_ctrl->workspace_layer.ivilayer, 0);

    /* set up animation to workspace background and workspace */
    hmi_ctrl->anima_set = animation_set_create(ec);

    wl_list_init(&hmi_ctrl->workspace_fade.layer_list);
    tmp_link_layer = MEM_ALLOC(sizeof(*tmp_link_layer));
    tmp_link_layer->layout_layer = hmi_ctrl->workspace_layer.ivilayer;
    wl_list_insert(&hmi_ctrl->workspace_fade.layer_list, &tmp_link_layer->link);
    tmp_link_layer = MEM_ALLOC(sizeof(*tmp_link_layer));
    tmp_link_layer->layout_layer = hmi_ctrl->workspace_background_layer.ivilayer;
    wl_list_insert(&hmi_ctrl->workspace_fade.layer_list, &tmp_link_layer->link);
    hmi_ctrl->workspace_fade.anima_set = hmi_ctrl->anima_set;

    ivi_layout_addNotificationCreateSurface(set_notification_create_surface, hmi_ctrl);
    ivi_layout_addNotificationRemoveSurface(set_notification_remove_surface, hmi_ctrl);
    ivi_layout_addNotificationConfigureSurface(set_notification_configure_surface, hmi_ctrl);

    free(ppScreen);
    ppScreen = NULL;

    return hmi_ctrl;
}

/**
 * Implementations of ivi-hmi-controller.xml
 */

/**
 * A surface drawing background is identified by id_surface.
 * Properties of the surface is set by using ivi_layout APIs according to
 * the scene graph of UI defined in hmi_controller_create.
 *
 * UI layer is used to add this surface.
 */
static void
ivi_hmi_controller_set_background(struct wl_resource *resource,
                                  uint32_t id_surface)

{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_surface *ivisurf = NULL;
    struct ivi_layout_layer   *ivilayer = hmi_ctrl->base_layer.ivilayer;
    const uint32_t dstx = hmi_ctrl->application_layer.x;
    const uint32_t dsty = hmi_ctrl->application_layer.y;
    const uint32_t width  = hmi_ctrl->application_layer.width;
    const uint32_t height = hmi_ctrl->application_layer.height;
    uint32_t ret = 0;

    uint32_t *add_surface_id = wl_array_add(&hmi_ctrl->ui_widgets,
                                            sizeof(*add_surface_id));
    *add_surface_id = id_surface;

    ivisurf = ivi_layout_getSurfaceFromId(id_surface);
    assert(ivisurf != NULL);

    ret = ivi_layout_layerAddSurface(ivilayer, ivisurf);
    assert(!ret);

    ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf,
                                    dstx, dsty, width, height);
    assert(!ret);

    ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
    assert(!ret);

    ivi_layout_commitChanges();
}

/**
 * A surface drawing panel is identified by id_surface.
 * Properties of the surface is set by using ivi_layout APIs according to
 * the scene graph of UI defined in hmi_controller_create.
 *
 * UI layer is used to add this surface.
 */
static void
ivi_hmi_controller_set_panel(struct wl_resource *resource,
                             uint32_t id_surface)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_surface *ivisurf  = NULL;
    struct ivi_layout_layer   *ivilayer = hmi_ctrl->base_layer.ivilayer;
    const uint32_t width  = hmi_ctrl->base_layer.width;
    uint32_t ret = 0;

    uint32_t *add_surface_id = wl_array_add(&hmi_ctrl->ui_widgets,
                                            sizeof(*add_surface_id));
    *add_surface_id = id_surface;

    ivisurf = ivi_layout_getSurfaceFromId(id_surface);
    assert(ivisurf != NULL);

    ret = ivi_layout_layerAddSurface(ivilayer, ivisurf);
    assert(!ret);
    uint32_t panel_height = hmi_ctrl->hmi_setting->panel_height;
    const uint32_t dstx = 0;
    const uint32_t dsty = hmi_ctrl->base_layer.height - panel_height;

    ret = ivi_layout_surfaceSetDestinationRectangle(
                        ivisurf, dstx, dsty, width, panel_height);
    assert(!ret);

    ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
    assert(!ret);

    ivi_layout_commitChanges();
}

/**
 * A surface drawing buttons in panel is identified by id_surface. It can set
 * several buttons. Properties of the surface is set by using ivi_layout
 * APIs according to the scene graph of UI defined in hmi_controller_create.
 * Additionally, the position of it is shifted to right when new one is requested.
 *
 * UI layer is used to add these surfaces.
 */
static void
ivi_hmi_controller_set_button(struct wl_resource *resource,
                              uint32_t id_surface, uint32_t number)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_surface *ivisurf  = NULL;
    struct ivi_layout_layer   *ivilayer = hmi_ctrl->base_layer.ivilayer;
    const uint32_t width  = 48;
    const uint32_t height = 48;
    uint32_t ret = 0;

    uint32_t *add_surface_id = wl_array_add(&hmi_ctrl->ui_widgets,
                                            sizeof(*add_surface_id));
    *add_surface_id = id_surface;

    ivisurf = ivi_layout_getSurfaceFromId(id_surface);
    assert(ivisurf != NULL);

    ret = ivi_layout_layerAddSurface(ivilayer, ivisurf);
    assert(!ret);

    uint32_t panel_height = hmi_ctrl->hmi_setting->panel_height;

    const uint32_t dstx = (60 * number) + 15;
    const uint32_t dsty = (hmi_ctrl->base_layer.height - panel_height) + 5;

    ret = ivi_layout_surfaceSetDestinationRectangle(
                            ivisurf,dstx, dsty, width, height);
    assert(!ret);

    ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
    assert(!ret);

    ivi_layout_commitChanges();
}

/**
 * A surface drawing home button in panel is identified by id_surface.
 * Properties of the surface is set by using ivi_layout APIs according to
 * the scene graph of UI defined in hmi_controller_create.
 *
 * UI layer is used to add these surfaces.
 */
static void
ivi_hmi_controller_set_home_button(struct wl_resource *resource,
                                   uint32_t id_surface)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_surface *ivisurf  = NULL;
    struct ivi_layout_layer   *ivilayer = hmi_ctrl->base_layer.ivilayer;
    uint32_t ret = 0;
    uint32_t size = 48;
    uint32_t panel_height = hmi_ctrl->hmi_setting->panel_height;
    const uint32_t dstx = (hmi_ctrl->base_layer.width - size) / 2;
    const uint32_t dsty = (hmi_ctrl->base_layer.height - panel_height) + 5;

    uint32_t *add_surface_id = wl_array_add(&hmi_ctrl->ui_widgets,
                                            sizeof(*add_surface_id));
    *add_surface_id = id_surface;

    ivisurf = ivi_layout_getSurfaceFromId(id_surface);
    assert(ivisurf != NULL);

    ret = ivi_layout_layerAddSurface(ivilayer, ivisurf);
    assert(!ret);

    ret = ivi_layout_surfaceSetDestinationRectangle(
                    ivisurf, dstx, dsty, size, size);
    assert(!ret);

    ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
    assert(!ret);

    ivi_layout_commitChanges();
    hmi_ctrl->is_initialized = 1;
}

/**
 * A surface drawing background of workspace is identified by id_surface.
 * Properties of the surface is set by using ivi_layout APIs according to
 * the scene graph of UI defined in hmi_controller_create.
 *
 * A layer of workspace_background is used to add this surface.
 */
static void
ivi_hmi_controller_set_workspacebackground(struct wl_resource *resource,
                                           uint32_t id_surface)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_surface *ivisurf = NULL;
    struct ivi_layout_layer   *ivilayer = NULL;
    ivilayer = hmi_ctrl->workspace_background_layer.ivilayer;

    uint32_t *add_surface_id = wl_array_add(&hmi_ctrl->ui_widgets,
                                            sizeof(*add_surface_id));
    *add_surface_id = id_surface;

    const uint32_t width  = hmi_ctrl->workspace_background_layer.width;
    const uint32_t height = hmi_ctrl->workspace_background_layer.height;
    uint32_t ret = 0;

    ivisurf = ivi_layout_getSurfaceFromId(id_surface);
    assert(ivisurf != NULL);

    ret = ivi_layout_layerAddSurface(ivilayer, ivisurf);
    assert(!ret);

    ret = ivi_layout_surfaceSetDestinationRectangle(ivisurf,
                                    0, 0, width, height);
    assert(!ret);

    ret = ivi_layout_surfaceSetVisibility(ivisurf, 1);
    assert(!ret);

    ivi_layout_commitChanges();
}

/**
 * A list of surfaces drawing launchers in workspace is identified by id_surfaces.
 * Properties of the surface is set by using ivi_layout APIs according to
 * the scene graph of UI defined in hmi_controller_create.
 *
 * The workspace can have several pages to group surfaces of launcher. Each call
 * of this interface increments a number of page to add a group of surfaces
 */
static void
ivi_hmi_controller_add_launchers(struct wl_resource *resource,
                                 uint32_t icon_size)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_layer *layer = hmi_ctrl->workspace_layer.ivilayer;
    uint32_t minspace_x = 10;
    uint32_t minspace_y = minspace_x;

    uint32_t width  = hmi_ctrl->workspace_layer.width;
    uint32_t height = hmi_ctrl->workspace_layer.height;

    uint32_t x_count = (width - minspace_x) / (minspace_x + icon_size);
    uint32_t space_x = (uint32_t)((width - x_count * icon_size) / (1.0 + x_count));
    float fcell_size_x = icon_size + space_x;

    uint32_t y_count = (height - minspace_y) / (minspace_y + icon_size);
    uint32_t space_y = (uint32_t)((height - y_count * icon_size) / (1.0 + y_count));
    float fcell_size_y = icon_size + space_y;

    if (0 == x_count) {
       x_count = 1;
    }

    if (0 == y_count) {
       y_count  = 1;
    }

    struct weston_config *config = weston_config_parse("weston.ini");
    if (!config) {
        return;
    }

    struct weston_config_section *section = weston_config_get_section(config, "ivi-shell", NULL, NULL);
    if (!section) {
        return;
    }

    const char *name = NULL;
    int launcher_count = 0;
    struct wl_array launchers;
    wl_array_init(&launchers);

    while (weston_config_next_section(config, &section, &name)) {
        uint32_t surfaceid = 0;
        uint32_t workspaceid = 0;
        if (0 != strcmp(name, "ivi-launcher")) {
            continue;
        }

        if (0 != weston_config_section_get_uint(section, "icon-id", &surfaceid, 0)) {
            continue;
        }

        if (0 != weston_config_section_get_uint(section, "workspace-id", &workspaceid, 0)) {
            continue;
        }

        struct launcher_info *info = wl_array_add(&launchers, sizeof(*info));

        if (info) {
            info->surface_id = surfaceid;
            info->workspace_id = workspaceid;
            info->index = launcher_count;
            ++launcher_count;
        }
    }

    qsort(launchers.data, launcher_count, sizeof(struct launcher_info), compare_launcher_info);

    uint32_t nx = 0;
    uint32_t ny = 0;
    int32_t prev = -1;
    struct launcher_info *data = NULL;
    wl_array_for_each(data, &launchers)
    {
        uint32_t *add_surface_id = wl_array_add(&hmi_ctrl->ui_widgets,
                                                sizeof(*add_surface_id));
        *add_surface_id = data->surface_id;

        if (0 > prev || (uint32_t)prev != data->workspace_id) {
            nx = 0;
            ny = 0;
            prev = data->workspace_id;

            if (0 <= prev) {
                hmi_ctrl->workspace_count++;
            }
        }

        if (y_count == ny) {
            ny = 0;
            hmi_ctrl->workspace_count++;
        }

        uint32_t x = (uint32_t)(nx * fcell_size_x + (hmi_ctrl->workspace_count - 1) * width + space_x);
        uint32_t y = (uint32_t)(ny * fcell_size_y  + space_y) ;

        struct ivi_layout_surface* layout_surface = NULL;
        layout_surface = ivi_layout_getSurfaceFromId(data->surface_id);
        assert(layout_surface);

        uint32_t ret = 0;
        ret = ivi_layout_layerAddSurface(layer, layout_surface);
        assert(!ret);

        ret = ivi_layout_surfaceSetDestinationRectangle(
                        layout_surface, x, y, icon_size, icon_size);
        assert(!ret);

        ret = ivi_layout_surfaceSetVisibility(layout_surface, 1);
        assert(!ret);

        nx++;

        if (x_count == nx) {
            ny++;
            nx = 0;
        }
    }

    wl_array_release(&launchers);
    weston_config_destroy(config);
    ivi_layout_commitChanges();
}

static void
ivi_hmi_controller_UI_ready(struct wl_client *client,
                            struct wl_resource *resource)
{
    struct setting {
        uint32_t background_id;
        uint32_t panel_id;
        uint32_t tiling_id;
        uint32_t sidebyside_id;
        uint32_t fullscreen_id;
        uint32_t random_id;
        uint32_t home_id;
        uint32_t workspace_background_id;
    };

    struct config_command {
        char *key;
        void *dest;
    };

    struct weston_config *config = NULL;
    struct weston_config_section *section = NULL;
    struct setting dest;
    int result = 0;
    int i = 0;

    const struct config_command uint_commands[] = {
        { "background-id", &dest.background_id },
        { "panel-id", &dest.panel_id },
        { "tiling-id", &dest.tiling_id },
        { "sidebyside-id", &dest.sidebyside_id },
        { "fullscreen-id", &dest.fullscreen_id },
        { "random-id", &dest.random_id },
        { "home-id", &dest.home_id },
        { "workspace-background-id", &dest.workspace_background_id },
        { NULL, NULL }
    };

    config = weston_config_parse("weston.ini");
    section = weston_config_get_section(config, "ivi-shell", NULL, NULL);

    for (i = 0; -1 != result; ++i)
    {
        const struct config_command *command = &uint_commands[i];

        if (!command->key)
        {
            break;
        }

        if (weston_config_section_get_uint(
                    section, command->key, (uint32_t *)command->dest, 0) != 0)
        {
            result = -1;
        }
    }

    if (-1 != result)
    {
        ivi_hmi_controller_set_background(resource, dest.background_id);
        ivi_hmi_controller_set_panel(resource, dest.panel_id);
        ivi_hmi_controller_set_button(resource, dest.tiling_id, 0);
        ivi_hmi_controller_set_button(resource, dest.sidebyside_id, 1);
        ivi_hmi_controller_set_button(resource, dest.fullscreen_id, 2);
        ivi_hmi_controller_set_button(resource, dest.random_id, 3);
        ivi_hmi_controller_set_home_button(resource, dest.home_id);
        ivi_hmi_controller_set_workspacebackground(resource, dest.workspace_background_id);
    }

    weston_config_destroy(config);

    ivi_hmi_controller_add_launchers(resource, 256);
}

/**
 * Implementation of request and event of ivi_hmi_controller_workspace_control
 * and controlling workspace.
 *
 * When motion of input is detected in a surface of workspace background,
 * ivi_hmi_controller_workspace_control shall be invoked and to start controlling of
 * workspace. The workspace has several pages to show several groups of applications.
 * The workspace is slid by using ivi-layout to select a a page in layer_set_pos
 * according to motion. When motion finished, e.g. touch up detected, control is
 * terminated and event:ivi_hmi_controller_workspace_control is notified.
 */
struct pointer_grab {
    struct weston_pointer_grab grab;
    struct ivi_layout_layer *layer;
    struct wl_resource *resource;
};

struct touch_grab {
    struct weston_touch_grab grab;
    struct ivi_layout_layer *layer;
    struct wl_resource *resource;
};

struct move_grab {
    wl_fixed_t dst[2];
    wl_fixed_t rgn[2][2];
    double v[2];
    struct timespec start_time;
    struct timespec pre_time;
    wl_fixed_t start_pos[2];
    wl_fixed_t pos[2];
    int32_t is_moved;
};

struct pointer_move_grab {
    struct pointer_grab base;
    struct move_grab move;
};

struct touch_move_grab {
    struct touch_grab base;
    struct move_grab move;
    int32_t is_active;
};

static void
pointer_grab_start(struct pointer_grab *grab,
                   struct ivi_layout_layer *layer,
                   const struct weston_pointer_grab_interface *interface,
                   struct weston_pointer *pointer)
{
    grab->grab.interface = interface;
    grab->layer = layer;
    weston_pointer_start_grab(pointer, &grab->grab);
}

static void
touch_grab_start(struct touch_grab *grab,
                 struct ivi_layout_layer *layer,
                 const struct weston_touch_grab_interface *interface,
                 struct weston_touch* touch)
{
    grab->grab.interface = interface;
    grab->layer = layer;
    weston_touch_start_grab(touch, &grab->grab);
}

static int32_t
range_val(int32_t val, int32_t min, int32_t max)
{
    if (val < min) {
        return min;
    }

    if (max < val) {
        return max;
    }

    return val;
}

static void
hmi_controller_move_animation_destroy(struct hmi_controller_animation *animation)
{
    struct move_animation_user_data *user_data = animation->user_data;
    if (animation == &user_data->hmi_ctrl->workspace_swipe_animation->base) {
        user_data->hmi_ctrl->workspace_swipe_animation = NULL;
    }

    animation_set_remove_animation(user_data->anima_set, animation);
    free(animation->user_data);
    animation->user_data = NULL;
}

static void
move_workspace_grab_end(struct move_grab *move, struct wl_resource* resource,
                        wl_fixed_t grab_x, struct ivi_layout_layer *layer)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    int32_t width = (int32_t)hmi_ctrl->workspace_background_layer.width;

    struct timespec time = {0};
    clock_gettime(CLOCK_MONOTONIC, &time);

    double  grab_time = 1e+3 * (time.tv_sec  - move->start_time.tv_sec) +
                        1e-6 * (time.tv_nsec - move->start_time.tv_nsec);

    double  from_motion_time = 1e+3 * (time.tv_sec  - move->pre_time.tv_sec) +
                               1e-6 * (time.tv_nsec - move->pre_time.tv_nsec);

    double pointer_v = move->v[0];

    if (200 < from_motion_time) {
       pointer_v = 0.0;
    }

    int32_t is_flick = grab_time < 400 &&
                       0.4 < fabs(pointer_v);

    int32_t pos[2] = {0};
    ivi_layout_layerGetPosition(layer, pos);

    int page_no = 0;

    if (is_flick) {
        int orgx = wl_fixed_to_int(move->dst[0] + grab_x);
        page_no = (-orgx + width / 2) / width;

        if (pointer_v < 0.0) {
            page_no++;
        }else {
            page_no--;
        }
    }else {
        page_no = (-pos[0] + width / 2) / width;
    }

    page_no = range_val(page_no, 0, hmi_ctrl->workspace_count - 1);
    double end_pos = -page_no * width;

    double dst = fabs(end_pos - pos[0]);
    double max_time = 0.5 * 1e+3;
    double v = dst / max_time;

    double vmin = 1000 * 1e-3;
    if (v < vmin ) {
        v = vmin;
    }

    double v0 = 0.0;
    if (pos[0] < end_pos) {
        v0 = v;
    } else {
        v0 = -v;
    }

    struct move_animation_user_data *animation_user_data = NULL;
    animation_user_data = MEM_ALLOC(sizeof(*animation_user_data));
    animation_user_data->layer = layer;
    animation_user_data->anima_set = hmi_ctrl->anima_set;
    animation_user_data->hmi_ctrl = hmi_ctrl;

    struct hmi_controller_animation_move* animation = NULL;
    animation = hmi_controller_animation_move_create(
        pos[0], end_pos, v0, v0,
        (hmi_controller_animation_frame_user_func)hmi_controller_anima_move_user_frame,
        animation_user_data, hmi_controller_move_animation_destroy);

    hmi_ctrl->workspace_swipe_animation = animation;
    animation_set_add_animation(hmi_ctrl->anima_set, &animation->base);

    ivi_hmi_controller_send_workspace_end_control(resource, move->is_moved);
}

static void
pointer_move_workspace_grab_end(struct pointer_grab *grab)
{
    struct pointer_move_grab *pnt_move_grab = (struct pointer_move_grab *) grab;
    struct ivi_layout_layer *layer = pnt_move_grab->base.layer;

    move_workspace_grab_end(&pnt_move_grab->move, grab->resource,
                            grab->grab.pointer->grab_x, layer);

    weston_pointer_end_grab(grab->grab.pointer);
}

static void
touch_move_workspace_grab_end(struct touch_grab *grab)
{
    struct touch_move_grab *tch_move_grab = (struct touch_move_grab *) grab;
    struct ivi_layout_layer *layer = tch_move_grab->base.layer;

    move_workspace_grab_end(&tch_move_grab->move, grab->resource,
                            grab->grab.touch->grab_x, layer);

    weston_touch_end_grab(grab->grab.touch);
}

static void
pointer_noop_grab_focus(struct weston_pointer_grab *grab)
{
}

static void
move_grab_update(struct move_grab *move, wl_fixed_t pointer[2])
{
    struct timespec timestamp = {0};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);

    double dt = (1e+3 * (timestamp.tv_sec  - move->pre_time.tv_sec) +
                 1e-6 * (timestamp.tv_nsec - move->pre_time.tv_nsec));

    if (dt < 1e-6) {
        dt = 1e-6;
    }

    move->pre_time = timestamp;

    int32_t ii = 0;
    for (ii = 0; ii < 2; ii++) {
        wl_fixed_t prepos = move->pos[ii];
        move->pos[ii] = pointer[ii] + move->dst[ii];

        if (move->pos[ii] < move->rgn[0][ii]) {
            move->pos[ii] = move->rgn[0][ii];
            move->dst[ii] = move->pos[ii] - pointer[ii];
        } else if (move->rgn[1][ii] < move->pos[ii]) {
            move->pos[ii] = move->rgn[1][ii];
            move->dst[ii] = move->pos[ii] - pointer[ii];
        }

        move->v[ii] = wl_fixed_to_double(move->pos[ii] - prepos) / dt;

        if (!move->is_moved &&
            0 < wl_fixed_to_int(move->pos[ii] - move->start_pos[ii])) {
            move->is_moved = 1;
        }
    }
}

static void
layer_set_pos(struct ivi_layout_layer *layer, wl_fixed_t pos[2])
{
    int32_t layout_pos[2] = {0};
    layout_pos[0] = wl_fixed_to_int(pos[0]);
    layout_pos[1] = wl_fixed_to_int(pos[1]);
    ivi_layout_layerSetPosition(layer, layout_pos);
    ivi_layout_commitChanges();
}

static void
pointer_move_grab_motion(struct weston_pointer_grab *grab, uint32_t time,
                         wl_fixed_t x, wl_fixed_t y)
{
    struct pointer_move_grab *pnt_move_grab = (struct pointer_move_grab *) grab;
    wl_fixed_t pointer_pos[2] = {x, y};
    move_grab_update(&pnt_move_grab->move, pointer_pos);
    layer_set_pos(pnt_move_grab->base.layer, pnt_move_grab->move.pos);
    weston_pointer_move(pnt_move_grab->base.grab.pointer, x, y);
}

static void
touch_move_grab_motion(struct weston_touch_grab *grab, uint32_t time,
                       int touch_id, wl_fixed_t x, wl_fixed_t y)
{
    struct touch_move_grab *tch_move_grab = (struct touch_move_grab *) grab;

    if (!tch_move_grab->is_active) {
        return;
    }

    wl_fixed_t pointer_pos[2] = {grab->touch->grab_x, grab->touch->grab_y};
    move_grab_update(&tch_move_grab->move, pointer_pos);
    layer_set_pos(tch_move_grab->base.layer, tch_move_grab->move.pos);
}

static void
pointer_move_workspace_grab_button(struct weston_pointer_grab *grab,
                                   uint32_t time, uint32_t button,
                                   uint32_t state_w)
{
    if (BTN_LEFT == button &&
        WL_POINTER_BUTTON_STATE_RELEASED == state_w) {
        struct pointer_grab *pg = (struct pointer_grab *)grab;
        pointer_move_workspace_grab_end(pg);
        free(grab);
    }
}

static void
touch_nope_grab_down(struct weston_touch_grab *grab, uint32_t time,
                     int touch_id, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
touch_move_workspace_grab_up(struct weston_touch_grab *grab, uint32_t time, int touch_id)
{
    struct touch_move_grab *tch_move_grab = (struct touch_move_grab *)grab;

    if (0 == touch_id) {
        tch_move_grab->is_active = 0;
    }

    if (0 == grab->touch->num_tp) {
        touch_move_workspace_grab_end(&tch_move_grab->base);
        free(grab);
    }
}

static void
pointer_move_workspace_grab_cancel(struct weston_pointer_grab *grab)
{
    struct pointer_grab *pg = (struct pointer_grab *)grab;
    pointer_move_workspace_grab_end(pg);
    free(grab);
}

static void
touch_move_workspace_grab_cancel(struct weston_touch_grab *grab)
{
    struct touch_grab *tg = (struct touch_grab *)grab;
    touch_move_workspace_grab_end(tg);
    free(grab);
}

static const struct weston_pointer_grab_interface pointer_move_grab_workspace_interface = {
    pointer_noop_grab_focus,
    pointer_move_grab_motion,
    pointer_move_workspace_grab_button,
    pointer_move_workspace_grab_cancel
};

static const struct weston_touch_grab_interface touch_move_grab_workspace_interface = {
    touch_nope_grab_down,
    touch_move_workspace_grab_up,
    touch_move_grab_motion,
    touch_move_workspace_grab_cancel
};

enum HMI_GRAB_DEVICE
{
    HMI_GRAB_DEVICE_NONE,
    HMI_GRAB_DEVICE_POINTER,
    HMI_GRAB_DEVICE_TOUCH
};

static enum HMI_GRAB_DEVICE
get_hmi_grab_device(struct weston_seat *seat, uint32_t serial)
{
    if (seat->pointer &&
        seat->pointer->focus &&
        seat->pointer->button_count &&
        seat->pointer->grab_serial == serial) {
        return HMI_GRAB_DEVICE_POINTER;
    }

    if (seat->touch &&
        seat->touch->focus &&
        seat->touch->grab_serial) {
        return HMI_GRAB_DEVICE_TOUCH;
    }

    return HMI_GRAB_DEVICE_NONE;
}

static void
move_grab_init(struct move_grab* move, wl_fixed_t start_pos[2],
               wl_fixed_t grab_pos[2], wl_fixed_t rgn[2][2],
               struct wl_resource* resource)
{
    clock_gettime(CLOCK_MONOTONIC, &move->start_time);
    move->pre_time = move->start_time;
    move->pos[0] = start_pos[0];
    move->pos[1] = start_pos[1];
    move->start_pos[0] = start_pos[0];
    move->start_pos[1] = start_pos[1];
    move->dst[0] = start_pos[0] - grab_pos[0];
    move->dst[1] = start_pos[1] - grab_pos[1];
    memcpy(move->rgn, rgn, sizeof(move->rgn));
}

static void
move_grab_init_workspace(struct move_grab* move,
                         wl_fixed_t grab_x, wl_fixed_t grab_y,
                         struct wl_resource *resource)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    struct ivi_layout_layer *layer = hmi_ctrl->workspace_layer.ivilayer;
    int32_t workspace_count = hmi_ctrl->workspace_count;
    int32_t workspace_width = hmi_ctrl->workspace_background_layer.width;
    int32_t layer_pos[2] = {0};
    ivi_layout_layerGetPosition(layer, layer_pos);

    wl_fixed_t start_pos[2] = {0};
    start_pos[0] = wl_fixed_from_int(layer_pos[0]);
    start_pos[1] = wl_fixed_from_int(layer_pos[1]);

    wl_fixed_t rgn[2][2] = {{0}};
    rgn[0][0] = wl_fixed_from_int(-workspace_width * (workspace_count - 1));

    rgn[0][1] = wl_fixed_from_int(0);
    rgn[1][0] = wl_fixed_from_int(0);
    rgn[1][1] = wl_fixed_from_int(0);

    wl_fixed_t grab_pos[2] = {grab_x, grab_y};

    move_grab_init(move, start_pos, grab_pos, rgn, resource);
}

static struct pointer_move_grab *
create_workspace_pointer_move(struct weston_pointer *pointer, struct wl_resource* resource)
{
    struct pointer_move_grab *pnt_move_grab = MEM_ALLOC(sizeof(*pnt_move_grab));
    pnt_move_grab->base.resource = resource;
    move_grab_init_workspace(&pnt_move_grab->move, pointer->grab_x, pointer->grab_y, resource);
    return pnt_move_grab;
}

static struct touch_move_grab *
create_workspace_touch_move(struct weston_touch *touch, struct wl_resource* resource)
{
    struct touch_move_grab *tch_move_grab = MEM_ALLOC(sizeof(*tch_move_grab));
    tch_move_grab->base.resource = resource;
    tch_move_grab->is_active = 1;
    move_grab_init_workspace(&tch_move_grab->move, touch->grab_x,touch->grab_y, resource);
    return tch_move_grab;
}

static void
ivi_hmi_controller_workspace_control(struct wl_client *client,
                                     struct wl_resource *resource,
                                     struct wl_resource *seat_resource,
                                     uint32_t serial)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);

    if (hmi_ctrl->workspace_count < 2) {
        return;
    }

    struct weston_seat* seat = wl_resource_get_user_data(seat_resource);
    enum HMI_GRAB_DEVICE device = get_hmi_grab_device(seat, serial);

    if (HMI_GRAB_DEVICE_POINTER != device &&
        HMI_GRAB_DEVICE_TOUCH != device) {
        return;
    }

    if (hmi_ctrl->workspace_swipe_animation) {
        hmi_controller_animation_destroy(&hmi_ctrl->workspace_swipe_animation->base);
    }

    struct ivi_layout_layer *layer = hmi_ctrl->workspace_layer.ivilayer;
    struct pointer_move_grab *pnt_move_grab = NULL;
    struct touch_move_grab *tch_move_grab = NULL;

    switch (device) {
    case HMI_GRAB_DEVICE_POINTER:
        pnt_move_grab = create_workspace_pointer_move(seat->pointer, resource);

        pointer_grab_start(
            &pnt_move_grab->base, layer, &pointer_move_grab_workspace_interface,
            seat->pointer);
        break;

    case HMI_GRAB_DEVICE_TOUCH:
        tch_move_grab = create_workspace_touch_move(seat->touch, resource);

        touch_grab_start(
            &tch_move_grab->base, layer, &touch_move_grab_workspace_interface,
            seat->touch);
        break;

    default:
        break;
    }
}

/**
 * Implementation of switch_mode
 */
static void
ivi_hmi_controller_switch_mode(struct wl_client *client,
                               struct wl_resource *resource,
                               uint32_t  layout_mode)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);
    switch_mode(hmi_ctrl, layout_mode);
}

/**
 * Implementation of on/off displaying workspace and workspace background layers.
 */
static void
ivi_hmi_controller_home(struct wl_client *client,
                        struct wl_resource *resource,
                        uint32_t home)
{
    struct hmi_controller *hmi_ctrl = wl_resource_get_user_data(resource);

    if ((IVI_HMI_CONTROLLER_HOME_ON  == home && !hmi_ctrl->workspace_fade.isFadeIn) ||
        (IVI_HMI_CONTROLLER_HOME_OFF == home && hmi_ctrl->workspace_fade.isFadeIn)) {

        uint32_t isFadeIn = !hmi_ctrl->workspace_fade.isFadeIn;
        hmi_controller_fade_run(isFadeIn, &hmi_ctrl->workspace_fade);
    }
}

/**
 * binding ivi-hmi-controller implementation
 */
static const struct ivi_hmi_controller_interface ivi_hmi_controller_implementation = {
    ivi_hmi_controller_UI_ready,
    ivi_hmi_controller_workspace_control,
    ivi_hmi_controller_switch_mode,
    ivi_hmi_controller_home
};

static void
unbind_hmi_controller(struct wl_resource *resource)
{
}

static void
bind_hmi_controller(struct wl_client *client,
           void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = NULL;

    resource = wl_resource_create(
            client, &ivi_hmi_controller_interface, 1, id);

    wl_resource_set_implementation(
            resource, &ivi_hmi_controller_implementation,
            data, unbind_hmi_controller);
}

static void
handle_hmi_client_process_sigchld(struct weston_process *proc, int status)
{
    proc->pid = 0;
}

static void
hmi_client_destroy(struct wl_listener *listener, void *data)
{
    struct hmi_controller *hmi_ctrl =
        container_of(listener, struct hmi_controller, destroy_listener);

    kill(hmi_ctrl->process.pid, SIGTERM);
    hmi_ctrl->process.pid = 0;
}

static void
launch_hmi_client_process(void *data)
{
    struct hmi_controller *hmi_ctrl =
        (struct hmi_controller *)data;

    weston_client_launch(hmi_ctrl->compositor,
                         &hmi_ctrl->process,
                         hmi_ctrl->hmi_setting->ivi_homescreen,
                         handle_hmi_client_process_sigchld);

    hmi_ctrl->destroy_listener.notify = hmi_client_destroy;
    wl_signal_add(&hmi_ctrl->compositor->destroy_signal, &hmi_ctrl->destroy_listener);

    free(hmi_ctrl->hmi_setting->ivi_homescreen);
}

/*****************************************************************************
 *  exported functions
 ****************************************************************************/

WL_EXPORT int
module_init(struct weston_compositor *ec,
            int *argc, char *argv[])
{
    struct hmi_controller *hmi_ctrl = hmi_controller_create(ec);
    hmi_ctrl->compositor = ec;

    if (wl_global_create(ec->wl_display,
                 &ivi_hmi_controller_interface, 1,
                 hmi_ctrl, bind_hmi_controller) == NULL) {
        return -1;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(ec->wl_display);
    wl_event_loop_add_idle(loop, launch_hmi_client_process, hmi_ctrl);

    return 0;
}
