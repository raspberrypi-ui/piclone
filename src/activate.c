/*============================================================================
Copyright (c) 2026 Raspberry Pi
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
============================================================================*/

#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include "xdg-activation-v1-client-protocol.h"

#include "activate.h"

/*----------------------------------------------------------------------------*/
/* Global data                                                                */
/*----------------------------------------------------------------------------*/

static char *app_id;
static guint bus_id;
static GtkWidget *wd_to_act;
static struct xdg_activation_v1 *activation;
static struct wl_seat *seat;
static uint32_t last_serial;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static void name_acquired (GDBusConnection *connection, const gchar *name, gpointer);
static void name_lost (GDBusConnection *connection, const gchar *name, gpointer);
static void handle_method_call (GDBusConnection *, const gchar*, const gchar*, const gchar*,
    const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer);
static void activate_app (void);
static void token_done (void *data, struct xdg_activation_token_v1 *token, const char *token_string);

/*----------------------------------------------------------------------------*/
/* DBus interface                                                             */
/*----------------------------------------------------------------------------*/

void init_dbus (const char *id)
{
    char *bus_name = g_strdup_printf ("com.raspberrypi.%s", id);
    app_id = g_strdup (id);
    bus_id = g_bus_own_name (G_BUS_TYPE_SESSION, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, name_acquired, name_lost, NULL, NULL);
    g_free (bus_name);
}

void close_dbus (void)
{
    g_bus_unown_name (bus_id);
}

static const GDBusInterfaceVTable interface_vtable =
{
    handle_method_call, NULL, NULL, { 0 }
};

static void name_acquired (GDBusConnection *connection, const gchar *name, gpointer)
{
    /* name not on DBus, so this is the first instance - set up handler for activate function */
    char *object_path = g_strdup_printf ("/com/raspberrypi/%s", app_id);
    char *introspection_xml = g_strdup_printf ("<node><interface name='com.raspberrypi.%s'><method name='activate'></method></interface></node>", app_id);
    GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

    g_dbus_connection_register_object (connection, object_path, introspection_data->interfaces[0], &interface_vtable, NULL, NULL, NULL);

    g_dbus_node_info_unref (introspection_data);
    g_free (introspection_xml);
    g_free (object_path);
}

static void name_lost (GDBusConnection *connection, const gchar *name, gpointer)
{
    /* name already on DBus, so application already running - call the activate function on the existing instance and then exit */
    char *bus_name = g_strdup_printf ("com.raspberrypi.%s", app_id);
    char *object_path = g_strdup_printf ("/com/raspberrypi/%s", app_id);
    char *interface_name =  g_strdup_printf ("com.raspberrypi.%s", app_id);

    GDBusProxy *proxy = g_dbus_proxy_new_sync (connection, G_DBUS_PROXY_FLAGS_NONE, NULL, bus_name, object_path, interface_name, NULL, NULL);
    g_dbus_proxy_call_sync (proxy, "activate", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    g_dbus_connection_close_sync (connection, NULL, NULL);

    g_object_unref (proxy);
    g_free (bus_name);
    g_free (object_path);
    g_free (interface_name);
    exit (0);
}

static void handle_method_call (GDBusConnection *, const gchar*, const gchar*, const gchar*,
    const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer)
{
    if (g_strcmp0 (method_name, "activate") == 0)
    {
        g_dbus_method_invocation_return_value (invocation, NULL);
        if (getenv ("WAYLAND_DISPLAY")) activate_app ();
        else gtk_window_present (GTK_WINDOW (wd_to_act));
    }
    else
    {
        char *err = g_strdup_printf ("com.raspberrypi.%s.Failed", app_id);
        g_dbus_method_invocation_return_dbus_error (invocation, err, "Unsupported method call");
        g_free (err);
    }
}

/*----------------------------------------------------------------------------*/
/* Wayland activation protocol                                                */
/*----------------------------------------------------------------------------*/

static const struct xdg_activation_token_v1_listener token_listener =
{
    .done = token_done,
};

static void activate_app (void)
{
    // request an activation token and fill in the data
    struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token (activation);
    xdg_activation_token_v1_add_listener (token, &token_listener, NULL);
    xdg_activation_token_v1_set_app_id (token, "rp-prefapps");
    xdg_activation_token_v1_set_serial (token, last_serial, seat);
    xdg_activation_token_v1_commit (token);
}

static void token_done (void *data, struct xdg_activation_token_v1 *token, const char *token_string)
{
    // activation token valid and ready to use
    struct wl_surface *surface = gdk_wayland_window_get_wl_surface (gtk_widget_get_window (wd_to_act));
    xdg_activation_v1_activate (activation, token_string, surface);
    xdg_activation_token_v1_destroy (token);
}

static void registry_add_object (void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    if (!g_strcmp0 (interface, xdg_activation_v1_interface.name))
        activation = wl_registry_bind (registry, name, &xdg_activation_v1_interface, 1);
}

static void registry_remove_object (void *, struct wl_registry *, uint32_t)
{
    xdg_activation_v1_destroy (activation);
}

static struct wl_registry_listener registry_listener =
{
    &registry_add_object,
    &registry_remove_object
};

/*----------------------------------------------------------------------------*/
/* Event listeners                                                            */
/*----------------------------------------------------------------------------*/

static void pointer_enter (void *, struct wl_pointer *, uint32_t serial, struct wl_surface *, wl_fixed_t, wl_fixed_t) { last_serial = serial; }
static void pointer_leave (void *, struct wl_pointer *, uint32_t serial, struct wl_surface *) { last_serial = serial; }
static void pointer_motion (void *, struct wl_pointer *, uint32_t serial, wl_fixed_t, wl_fixed_t) { last_serial = serial; }
static void pointer_button (void *, struct wl_pointer *, uint32_t serial, uint32_t, uint32_t, uint32_t) { last_serial = serial; }
static void pointer_frame (void *, struct wl_pointer *) {}
static void pointer_axis (void *, struct wl_pointer *, uint32_t serial, uint32_t, wl_fixed_t) { last_serial = serial; }
static void pointer_axis_discrete (void *, struct wl_pointer *, uint32_t, int32_t) {}
static void pointer_axis_source (void *, struct wl_pointer *, uint32_t) {}
static void pointer_axis_stop (void *, struct wl_pointer *, uint32_t, uint32_t) {}
static void keyboard_keymap (void *, struct wl_keyboard *, uint32_t, int, uint32_t) {}
static void keyboard_enter (void *, struct wl_keyboard *, uint32_t serial, struct wl_surface *, struct wl_array *) { last_serial = serial; }
static void keyboard_leave (void *, struct wl_keyboard *, uint32_t serial, struct wl_surface *) { last_serial = serial; }
static void keyboard_key (void *, struct wl_keyboard *, uint32_t serial, uint32_t, uint32_t, uint32_t) { last_serial = serial; }
static void keyboard_modifiers (void *, struct wl_keyboard *, uint32_t serial, uint32_t, uint32_t, uint32_t, uint32_t) { last_serial = serial; }
static void keyboard_repeat_info (void *, struct wl_keyboard *, int32_t, int32_t) {}

static const struct wl_pointer_listener pointer_listener =
{
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .frame = pointer_frame,
    .axis = pointer_axis,
    .axis_discrete = pointer_axis_discrete,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
};

static const struct wl_keyboard_listener keyboard_listener =
{
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

void setup_activate (GtkWidget *window)
{
    wd_to_act = window;

    if (getenv ("WAYLAND_DISPLAY"))
    {
        GdkDisplay *gdk_display = gdk_display_get_default ();
        GdkSeat *gdk_seat = gdk_display_get_default_seat (gdk_display);

        seat = gdk_wayland_seat_get_wl_seat (gdk_seat);

        struct wl_display *display = gdk_wayland_display_get_wl_display (gdk_display);
        struct wl_registry *registry = wl_display_get_registry (display);
        wl_registry_add_listener (registry, &registry_listener, NULL);
        wl_display_roundtrip (display);
        wl_registry_destroy (registry);

        wl_pointer_add_listener (wl_seat_get_pointer (seat), &pointer_listener, NULL);
        wl_keyboard_add_listener (wl_seat_get_keyboard (seat), &keyboard_listener, NULL);
    }
}

/* End of file */
/*----------------------------------------------------------------------------*/
