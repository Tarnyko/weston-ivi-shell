/*
 * Copyright (C) 2013 DENSO CORPORATION
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
 * ivi-shell supports a type of shell for In-Vehicle Infotainment system.
 * In-Vehicle Infotainment system traditionally manages surfaces with global
 * identification. A protocol, ivi_application, supports such a feature
 * by implementing a request, ivi_application::surface_creation defined in
 * ivi_application.xml.
 *
 *  The ivi-shell explicitly loads a module to add business logic like how to
 *  layout surfaces by using internal ivi-layout APIs.
 */
#include "config.h"

#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <dlfcn.h>
#include <limits.h>

#include "ivi-shell.h"
#include "ivi-application-server-protocol.h"
#include "ivi-layout.h"

#include "../shared/os-compatibility.h"

struct ivi_shell_surface
{
    struct ivi_shell *shell;
    struct ivi_layout_surface *layout_surface;

    struct weston_surface *surface;
    uint32_t id_surface;

    int32_t width;
    int32_t height;

    struct wl_list link;
};

struct ivi_shell_setting
{
    char *ivi_module;
};

static struct ivi_layout_interface *ivi_layout;

/* ------------------------------------------------------------------------- */

    /* common functions                                                          */
/* ------------------------------------------------------------------------- */

/**
 * Implementation of ivi_surface
 */

static void
ivi_shell_surface_configure(struct weston_surface *, int32_t, int32_t);

static struct ivi_shell_surface *
get_ivi_shell_surface(struct weston_surface *surface)
{
    if (surface->configure == ivi_shell_surface_configure) {
        return surface->configure_private;
    } else {
        return NULL;
    }
}

static void
ivi_shell_surface_configure(struct weston_surface *surface,
                        int32_t sx, int32_t sy)
{
    struct ivi_shell_surface *ivisurf = get_ivi_shell_surface(surface);
    struct weston_view *view = NULL;
    float from_x = 0.0f;
    float from_y = 0.0f;
    float to_x = 0.0f;
    float to_y = 0.0f;

    if ((surface->width == 0) || (surface->height == 0) || (ivisurf == NULL)) {
        return;
    }

    view = ivi_layout->get_weston_view(ivisurf->layout_surface);
    if (view == NULL) {
        return;
    }

    if (ivisurf->width != surface->width || ivisurf->height != surface->height) {

        ivisurf->width  = surface->width;
        ivisurf->height = surface->height;

        weston_view_to_global_float(view, 0, 0, &from_x, &from_y);
        weston_view_to_global_float(view, sx, sy, &to_x, &to_y);

        weston_view_set_position(view,
                  view->geometry.x + to_x - from_x,
                  view->geometry.y + to_y - from_y);
        weston_view_update_transform(view);

        ivi_layout->surfaceConfigure(ivisurf->layout_surface, surface->width, surface->height);
    }
}

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
    struct ivi_shell_surface *ivisurf = wl_resource_get_user_data(resource);
    if (ivisurf != NULL) {
        ivisurf->surface->configure = NULL;
        ivisurf->surface->configure_private = NULL;
        ivisurf->surface = NULL;
        ivi_layout->surfaceSetNativeContent(NULL, 0, 0, ivisurf->id_surface);
    }

    wl_resource_destroy(resource);
}

static const struct ivi_surface_interface surface_implementation = {
    surface_destroy,
};

static struct ivi_shell_surface *
is_surf_in_surfaces(struct wl_list *list_surf, uint32_t id_surface)
{
    struct ivi_shell_surface *ivisurf;

    wl_list_for_each(ivisurf, list_surf, link) {
        if (ivisurf->id_surface == id_surface) {
            return ivisurf;
        }
    }

    return NULL;
}

static const struct {
    uint32_t warning_code; /* enum ivi_surface_warning_code */
    const char *msg;
} warning_strings[] = {
    {IVI_SURFACE_WARNING_CODE_INVALID_WL_SURFACE, "wl_surface is invalid"},
    {IVI_SURFACE_WARNING_CODE_IVI_ID_IN_USE, "surface_id is already assigned by another app"}
};

/**
 * Implementation of ivi_application::surface_create.
 * Creating new ivi_shell_surface with identification to identify the surface
 * in the system.
 */
static void
application_surface_create(struct wl_client *client,
                           struct wl_resource *resource,
                           uint32_t id_surface,
                           struct wl_resource *surface_resource,
                           uint32_t id)
{
    struct ivi_shell *shell = wl_resource_get_user_data(resource);
    struct ivi_shell_surface *ivisurf = NULL;
    struct ivi_layout_surface *layout_surface = NULL;
    struct weston_surface *weston_surface = wl_resource_get_user_data(surface_resource);
    struct wl_resource *res;
    int32_t warn_idx = -1;

    if (weston_surface != NULL) {

	/* check if a surface already has another role*/
	if (weston_surface->configure) {
		wl_resource_post_error(weston_surface->resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already "
				       "set");
		return;
	}

        layout_surface = ivi_layout->surfaceCreate(weston_surface, id_surface);

        if (layout_surface == NULL)
            warn_idx = 1;
    } else {
        warn_idx = 0;
    }

    res = wl_resource_create(client, &ivi_surface_interface, 1, id);
    if (res == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    if (warn_idx >= 0) {
        wl_resource_set_implementation(res, &surface_implementation,
                                       NULL, NULL);
        ivi_surface_send_warning(res,
                                 warning_strings[warn_idx].warning_code,
                                 warning_strings[warn_idx].msg);
        return;
    }

    ivisurf = is_surf_in_surfaces(&shell->ivi_surface_list, id_surface);
    if (ivisurf == NULL) {
        ivisurf = zalloc(sizeof *ivisurf);
        if (ivisurf == NULL) {
            wl_resource_post_no_memory(res);
            return;
        }

        wl_list_init(&ivisurf->link);
        wl_list_insert(&shell->ivi_surface_list, &ivisurf->link);

        ivisurf->shell = shell;
        ivisurf->id_surface = id_surface;
    }

    ivisurf->width = 0;
    ivisurf->height = 0;
    ivisurf->layout_surface = layout_surface;
    ivisurf->surface = weston_surface;

    weston_surface->configure = ivi_shell_surface_configure;
    weston_surface->configure_private = ivisurf;

    wl_resource_set_implementation(res, &surface_implementation,
                                   ivisurf, NULL);
}

static const struct ivi_application_interface application_implementation = {
    application_surface_create
};

static void
bind_ivi_application(struct wl_client *client,
                void *data, uint32_t version, uint32_t id)
{
    struct ivi_shell *shell = data;
    struct wl_resource *resource = NULL;

    resource = wl_resource_create(client, &ivi_application_interface, 1, id);

    wl_resource_set_implementation(resource,
                                   &application_implementation,
                                   shell, NULL);
}

/**
 * Initialization/destruction method of ivi-shell
 */
static void
shell_destroy(struct wl_listener *listener, void *data)
{
    struct ivi_shell *shell =
        container_of(listener, struct ivi_shell, destroy_listener);
    struct ivi_shell_surface *ivisurf, *next;

    wl_list_for_each_safe(ivisurf, next, &shell->ivi_surface_list, link) {
        wl_list_remove(&ivisurf->link);
        free(ivisurf);
    }

    free(shell);
}

static void
init_ivi_shell(struct weston_compositor *compositor, struct ivi_shell *shell)
{
    shell->compositor = compositor;

    wl_list_init(&shell->ivi_surface_list);
}

static int
ivi_shell_setting_create(struct ivi_shell_setting *dest)
{
    int result = 0;
    struct weston_config *config = NULL;
    struct weston_config_section *section = NULL;

    if (NULL == dest) {
        return -1;
    }

    config = weston_config_parse("weston.ini");
    section = weston_config_get_section(config, "ivi-shell", NULL, NULL);

    if (weston_config_section_get_string(
        section, "ivi-module", (char **)&dest->ivi_module, NULL) != 0)
    {
        result = -1;
    }

    weston_config_destroy(config);
    return result;
}

/**
 * Initialization of ivi-shell.
 */
static int
ivi_load_modules(struct weston_compositor *compositor, const char *modules,
	     int *argc, char *argv[])
{
	const char *p, *end;
	char buffer[256];
	int (*module_init)(struct weston_compositor *compositor,
			   int *argc, char *argv[]);

	if (modules == NULL)
		return 0;

	p = modules;
	while (*p) {
		end = strchrnul(p, ',');
		snprintf(buffer, sizeof buffer, "%.*s", (int) (end - p), p);
		module_init = weston_load_module(buffer, "module_init");
		if (module_init)
			module_init(compositor, argc, argv);
		p = end;
		while (*p == ',')
			p++;

	}

	return 0;
}

WL_EXPORT int
module_init(struct weston_compositor *compositor,
            int *argc, char *argv[])
{
    struct ivi_shell  *shell = NULL;
    char ivi_layout_path[PATH_MAX];
    void *module;
    struct ivi_shell_setting setting = { };

    shell = zalloc(sizeof *shell);
    if (shell == NULL) {
        return -1;
    }

    init_ivi_shell(compositor, shell);

    shell->destroy_listener.notify = shell_destroy;
    wl_signal_add(&compositor->destroy_signal, &shell->destroy_listener);

    if (wl_global_create(compositor->wl_display, &ivi_application_interface, 1,
                         shell, bind_ivi_application) == NULL) {
        return -1;
    }

    if (ivi_shell_setting_create(&setting) != 0) {
        return 0;
    }

    /*load module:ivi-layout*/
    /*ivi_layout_interface is referred by ivi-shell to use ivi-layout*/
    snprintf(ivi_layout_path, sizeof ivi_layout_path, "%s/%s", MODULEDIR, "ivi-layout.so");
    module = dlopen(ivi_layout_path, RTLD_NOW | RTLD_NOLOAD);
    if (module) {
	weston_log("ivi-shell: Module '%s' already loaded\n", ivi_layout_path);
	dlclose(module);
	return -1;
    }

    weston_log("ivi-shell: Loading module '%s'\n", ivi_layout_path);
    module = dlopen(ivi_layout_path, RTLD_NOW | RTLD_GLOBAL);
    if (!module) {
	weston_log("ivi-shell: Failed to load module: %s\n", dlerror());
	return -1;
    }

    ivi_layout = dlsym(module,"ivi_layout_interface");
    if (!ivi_layout){
	weston_log("ivi-shell: couldn't find ivi_layout_interface in '%s'\n", ivi_layout_path);
	free(setting.ivi_module);
	return -1;
    }
    else{
	ivi_layout->initWithCompositor(compositor);
    }

    /*Call module_init of ivi-modules which are defined in weston.ini*/
    if (ivi_load_modules(compositor,setting.ivi_module,argc,argv) < 0){
	 free(setting.ivi_module);
	 return -1;
    }

    free(setting.ivi_module);
    return 0;
}
