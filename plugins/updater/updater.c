/*
Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
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
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>

#include "plugin.h"

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#define DEBUG_ON
#ifdef DEBUG_ON
#define DEBUG(fmt,args...) g_message("up: " fmt,##args)
#else
#define DEBUG
#endif

#define SECS_PER_HOUR 3600L
#define UPDATE_ICON "update-avail"

/*----------------------------------------------------------------------------*/
/* Plug-in global data                                                        */
/*----------------------------------------------------------------------------*/

typedef struct {

    GtkWidget *plugin;              /* Back pointer to the widget */
    LXPanel *panel;                 /* Back pointer to panel */
    GtkWidget *tray_icon;           /* Displayed image */
    config_setting_t *settings;     /* Plugin settings */
    GtkWidget *menu;                /* Popup menu */
    GtkWidget *update_dlg;          /* Widget used to display pending update list */
    int n_updates;                  /* Number of pending updates */
    gchar **ids;                    /* ID strings for pending updates */
    int interval;                   /* Number of hours between periodic checks */
    guint timer;                    /* Periodic check timer ID */
} UpdaterPlugin;


/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static gboolean net_available (void);
static gboolean clock_synced (void);
static void check_for_updates (gpointer user_data);
static gpointer refresh_update_cache (gpointer data);
static void refresh_cache_done (PkTask *task, GAsyncResult *res, gpointer data);
static gboolean filter_fn (PkPackage *package, gpointer user_data);
static void check_updates_done (PkTask *task, GAsyncResult *res, gpointer data);
static void install_updates (GtkWidget *widget, gpointer user_data);
static void launch_installer (void);
static void show_updates (GtkWidget *widget, gpointer user_data);
static void handle_close_update_dialog (GtkButton *button, gpointer user_data);
static void handle_close_and_install (GtkButton *button, gpointer user_data);
static gint delete_update_dialog (GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void show_menu (UpdaterPlugin *up);
static void hide_menu (UpdaterPlugin *up);
static void message (char *msg, int prog);
static gboolean close_msg (GtkButton *button, gpointer data);
static void update_icon (UpdaterPlugin *up, gboolean hide);
static gboolean init_check (gpointer data);
static gboolean net_check (gpointer data);
static gboolean periodic_check (gpointer data);
static GtkWidget *updater_constructor (LXPanel *panel, config_setting_t *settings);
static gboolean updater_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel);
static void updater_configuration_changed (LXPanel *panel, GtkWidget *p);
static gboolean updater_control_msg (GtkWidget *plugin, const char *cmd);
static GtkWidget *updater_configure (LXPanel *panel, GtkWidget *p);
static gboolean updater_apply_configuration (gpointer user_data);
static void updater_destructor (gpointer user_data);


/*----------------------------------------------------------------------------*/
/* Utility functions                                                          */
/*----------------------------------------------------------------------------*/

static gboolean net_available (void)
{
    if (system ("hostname -I | grep -q \\\\.") == 0) return TRUE;
    else return FALSE;
}

static gboolean clock_synced (void)
{
    if (system ("test -e /usr/sbin/ntpd") == 0)
    {
        if (system ("ntpq -p | grep -q ^\\*") == 0) return TRUE;
    }
    else
    {
        if (system ("timedatectl status | grep -q \"synchronized: yes\"") == 0) return TRUE;
    }
    return FALSE;
}


/*----------------------------------------------------------------------------*/
/* Handlers for PackageKit asynchronous check for updates                     */
/*----------------------------------------------------------------------------*/

static void check_for_updates (gpointer user_data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) user_data;

    if (!net_available ())
    {
        DEBUG ("No network connection - update check failed");
        return;
    }

    DEBUG ("Checking for updates");
    g_thread_new (NULL, refresh_update_cache, up);
}

static gpointer refresh_update_cache (gpointer data)
{
    PkTask *task = pk_task_new ();
    pk_client_refresh_cache_async (PK_CLIENT (task), TRUE, NULL, NULL, NULL, (GAsyncReadyCallback) refresh_cache_done, data);
    return NULL;
}

static void refresh_cache_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    GError *error = NULL;
    PkResults *results = pk_task_generic_finish (task, res, &error);

    if (error != NULL)
    {
        DEBUG ("Error updating cache - %s", error->message);
        g_error_free (error);
        return;
    }

    DEBUG ("Cache updated - comparing versions");
    pk_client_get_updates_async (PK_CLIENT (task), PK_FILTER_ENUM_NONE, NULL, NULL, NULL, (GAsyncReadyCallback) check_updates_done, data);
}

static gboolean filter_fn (PkPackage *package, gpointer user_data)
{
    if (strstr (pk_package_get_arch (package), "amd64")) return FALSE;
    return TRUE;
}

static void check_updates_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) data;
    PkPackageSack *sack = NULL, *fsack;
    int n_up;

    GError *error = NULL;
    PkResults *results = pk_task_generic_finish (task, res, &error);

    if (error != NULL)
    {
        DEBUG ("Error comparing versions - %s", error->message);
        g_error_free (error);
        return;
    }

    if (system ("raspi-config nonint is_pi"))
    {
        sack = pk_results_get_package_sack (results);
        fsack = pk_package_sack_filter (sack, filter_fn, data);
    }
    else
    {
        fsack = pk_results_get_package_sack (results);
    }

    up->n_updates = pk_package_sack_get_size (fsack);
    if (up->ids != NULL) g_strfreev (up->ids);
    if (up->n_updates > 0)
    {
        DEBUG ("Check complete - %d updates available", up->n_updates);
        up->ids = pk_package_sack_get_ids (fsack);
        lxpanel_notify (up->panel, _("Updates are available\nClick the update icon to install"));
    }
    else
    {
        DEBUG ("Check complete - no updates available");
        up->ids = NULL;
    }
    update_icon (up, FALSE);

    if (sack) g_object_unref (sack);
    g_object_unref (fsack);
}


/*----------------------------------------------------------------------------*/
/* Launch installer process                                                   */
/*----------------------------------------------------------------------------*/

static void install_updates (GtkWidget *widget, gpointer user_data)
{
    if (!net_available ())
    {
        message (_("No network connection - cannot install updates."), -3);
        return;
    }

    if (!clock_synced ())
    {
        message (_("Clock not synchronised - cannot install updates. Try again in a few minutes."), -3);
        return;
    }

    launch_installer ();
}

static void launch_installer (void)
{
    char *cmd[3] = {"sudo", "lxplug-updater-install", NULL};

    gchar **environ = g_environ_setenv (g_get_environ (), "SUDO_ASKPASS", "/usr/lib/lxplugins/pwdlpu.sh", TRUE);
    g_spawn_async (NULL, cmd, environ, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_strfreev (environ);
}


/*----------------------------------------------------------------------------*/
/* Dialog box showing pending updates                                         */
/*----------------------------------------------------------------------------*/

static void show_updates (GtkWidget *widget, gpointer user_data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) user_data;
    GtkBuilder *builder;
    GtkWidget *update_list;
    GtkCellRenderer *trend = gtk_cell_renderer_text_new ();
    int count;
    char buffer[1024];

    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/ui/lxplug-updater.ui");
    up->update_dlg = (GtkWidget *) gtk_builder_get_object (builder, "update_dlg");
    g_signal_connect (gtk_builder_get_object (builder, "btn_install"), "clicked", G_CALLBACK (handle_close_and_install), up);
    g_signal_connect (gtk_builder_get_object (builder, "btn_close"), "clicked", G_CALLBACK (handle_close_update_dialog), up);
    g_signal_connect (up->update_dlg, "delete_event", G_CALLBACK (delete_update_dialog), up);

    GtkListStore *ls = gtk_list_store_new (2, G_TYPE_STRING, G_TYPE_STRING);
    count = 0;
    while (count < up->n_updates)
    {
        g_strlcpy (buffer, up->ids[count], sizeof (buffer));
        gtk_list_store_insert_with_values (ls, NULL, count, 0, strtok (buffer, ";"), 1, strtok (NULL, ";"), -1);
        count++;
    }

    update_list = (GtkWidget *) gtk_builder_get_object (builder, "update_list");
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (update_list), -1, "Package", trend, "text", 0, NULL);
    gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (update_list), -1, "Version", trend, "text", 1, NULL);
    gtk_tree_view_set_model (GTK_TREE_VIEW (update_list), GTK_TREE_MODEL (ls));

    gtk_widget_show_all (up->update_dlg);
}

static void handle_close_update_dialog (GtkButton *button, gpointer user_data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) user_data;
    if (up->update_dlg)
    {
        gtk_widget_destroy (up->update_dlg);
        up->update_dlg = NULL;
    }
}

static void handle_close_and_install (GtkButton *button, gpointer user_data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) user_data;
    if (up->update_dlg)
    {
        gtk_widget_destroy (up->update_dlg);
        up->update_dlg = NULL;
    }
    if (net_available () && clock_synced ()) launch_installer ();
}

static gint delete_update_dialog (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) user_data;
    handle_close_update_dialog (NULL, user_data);
    return TRUE;
}


/*----------------------------------------------------------------------------*/
/* Menu                                                                       */
/*----------------------------------------------------------------------------*/

static void show_menu (UpdaterPlugin *up)
{
    GtkWidget *item;

    hide_menu (up);

    up->menu = gtk_menu_new ();
    gtk_menu_set_reserve_toggle_size (GTK_MENU (up->menu), FALSE);

    item = lxpanel_plugin_new_menu_item (up->panel, _("Show Updates..."), 0, NULL);
    g_signal_connect (G_OBJECT (item), "activate", G_CALLBACK (show_updates), up);
    gtk_menu_shell_append (GTK_MENU_SHELL (up->menu), item);

    item = lxpanel_plugin_new_menu_item (up->panel, _("Install Updates"), 0, NULL);
    g_signal_connect (G_OBJECT (item), "activate", G_CALLBACK (install_updates), up);
    gtk_menu_shell_append (GTK_MENU_SHELL (up->menu), item);

    gtk_widget_show_all (up->menu);
    gtk_menu_popup_at_widget (GTK_MENU (up->menu), up->plugin, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static void hide_menu (UpdaterPlugin *up)
{
    if (up->menu)
    {
		gtk_menu_popdown (GTK_MENU (up->menu));
		gtk_widget_destroy (up->menu);
		up->menu = NULL;
	}
}


/*----------------------------------------------------------------------------*/
/* Error box                                                                  */
/*----------------------------------------------------------------------------*/

static void message (char *msg, int prog)
{
    GtkBuilder *builder;
    GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_btn;

    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/ui/lxpanel-modal.ui");
    msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "modal");
    msg_msg = (GtkWidget *) gtk_builder_get_object (builder, "modal_msg");
    msg_pb = (GtkWidget *) gtk_builder_get_object (builder, "modal_pb");
    msg_btn = (GtkWidget *) gtk_builder_get_object (builder, "modal_ok");
    gtk_widget_hide (GTK_WIDGET (gtk_builder_get_object (builder, "modal_cancel")));
    gtk_label_set_text (GTK_LABEL (msg_msg), msg);
    g_object_unref (builder);

    gtk_widget_set_visible (msg_btn, prog == -3);
    gtk_widget_set_visible (msg_pb, prog > -2);
    g_signal_connect (msg_btn, "clicked", G_CALLBACK (close_msg), msg_dlg);

    if (prog >= 0)
    {
        float progress = prog / 100.0;
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), progress);
    }
    else if (prog == -1) gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
    gtk_widget_show (msg_dlg);
}

static gboolean close_msg (GtkButton *button, gpointer data)
{
    gtk_widget_destroy (GTK_WIDGET (data));
    return FALSE;
}


/*----------------------------------------------------------------------------*/
/* Icon                                                                       */
/*----------------------------------------------------------------------------*/

static void update_icon (UpdaterPlugin *up, gboolean hide)
{
    /* if updates are available, show the icon */
    if (up->n_updates && !hide)
    {
        gtk_widget_show_all (up->plugin);
        gtk_widget_set_sensitive (up->plugin, TRUE);
    }
    else
    {
        gtk_widget_hide (up->plugin);
        gtk_widget_set_sensitive (up->plugin, FALSE);
    }
}


/*----------------------------------------------------------------------------*/
/* Timer handlers                                                             */
/*----------------------------------------------------------------------------*/

static gboolean init_check (gpointer data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) data;
    update_icon (up, TRUE);

    /* Don't bother with the check if the wizard is running - it checks anyway... */
    if (!system ("ps ax | grep -v grep | grep -q piwiz")) return FALSE;

    if (net_available ()) check_for_updates (up);
    else
    {
        DEBUG ("No network connection - polling...");
        g_timeout_add_seconds (60, net_check, up);
    }
    return FALSE;
}

static gboolean net_check (gpointer data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) data;
    if (net_available ())
    {
        check_for_updates (up);
        return FALSE;
    }
    DEBUG ("No network connection - polling...");
    return TRUE;
}

static gboolean periodic_check (gpointer data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) data;
    check_for_updates (up);
    return TRUE;
}


/*----------------------------------------------------------------------------*/
/* Plugin functions                                                           */
/*----------------------------------------------------------------------------*/

/* Plugin constructor */
static GtkWidget *updater_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    UpdaterPlugin *up = g_new0 (UpdaterPlugin, 1);
    int val;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    up->tray_icon = gtk_image_new ();
    lxpanel_plugin_set_taskbar_icon (panel, up->tray_icon, UPDATE_ICON);
    gtk_widget_set_tooltip_text (up->tray_icon, _("Updates are available - click to install"));
    gtk_widget_set_visible (up->tray_icon, TRUE);

    /* Allocate top level widget and set into Plugin widget pointer. */
    up->panel = panel;
    up->plugin = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (up->plugin), GTK_RELIEF_NONE);
    g_signal_connect (up->plugin, "button-press-event", G_CALLBACK (updater_button_press_event), NULL);
    up->settings = settings;
    lxpanel_plugin_set_data (up->plugin, up, updater_destructor);
    gtk_widget_add_events (up->plugin, GDK_BUTTON_PRESS_MASK);

    /* Allocate icon as a child of top level */
    gtk_container_add (GTK_CONTAINER (up->plugin), up->tray_icon);

    /* Initialise data structures */
    up->menu = NULL;

    /* Hide the widget and start the check for updates */
    up->n_updates = 0;
    up->ids = NULL;
    gtk_widget_show_all (up->plugin);
    g_idle_add (init_check, up);

    /* Set timer for update checks */
    if (!config_setting_lookup_int (settings, "Interval", &up->interval)) up->interval = 24;
    if (up->interval)
        up->timer = g_timeout_add_seconds (up->interval * SECS_PER_HOUR, periodic_check, up);
    else
        up->timer = 0;

    return up->plugin;
}

/* Handler for menu button click */
static gboolean updater_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel)
{
    UpdaterPlugin *up = lxpanel_plugin_get_data (widget);

#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif
    /* Show or hide the popup menu on left-click */
    if (event->button == 1)
    {
        show_menu (up);
        return TRUE;
    }
    else return FALSE;
}

/* Handler for system config changed message from panel */
static void updater_configuration_changed (LXPanel *panel, GtkWidget *p)
{
    UpdaterPlugin *up = lxpanel_plugin_get_data (p);

    lxpanel_plugin_set_taskbar_icon (panel, up->tray_icon, UPDATE_ICON);
}

/* Handler for control message from panel */
static gboolean updater_control_msg (GtkWidget *plugin, const char *cmd)
{
    UpdaterPlugin *up = lxpanel_plugin_get_data (plugin);

    if (!strncmp (cmd, "check", 5))
    {
        update_icon (up, TRUE);
        check_for_updates (up);
        return TRUE;
    }

    return FALSE;
}

/* Handler to open config dialog */
static GtkWidget *updater_configure (LXPanel *panel, GtkWidget *p)
{
    UpdaterPlugin *up = lxpanel_plugin_get_data (p);
#ifdef ENABLE_NLS
    textdomain (GETTEXT_PACKAGE);
#endif
    return lxpanel_generic_config_dlg(_("Updater"), panel,
        updater_apply_configuration, p,
        _("Hours between checks for updates"), &up->interval, CONF_TYPE_INT,
        NULL);
}

/* Handler on closing config dialog */
static gboolean updater_apply_configuration (gpointer user_data)
{
    UpdaterPlugin *up = lxpanel_plugin_get_data ((GtkWidget *) user_data);
    config_group_set_int (up->settings, "Interval", up->interval);
    if (up->timer) g_source_remove (up->timer);
    if (up->interval)
        up->timer = g_timeout_add_seconds (up->interval * SECS_PER_HOUR, periodic_check, up);
    else
        up->timer = 0;
}

/* Plugin destructor. */
static void updater_destructor (gpointer user_data)
{
    UpdaterPlugin *up = (UpdaterPlugin *) user_data;

    /* Deallocate memory */
    g_free (up);
}

FM_DEFINE_MODULE(lxpanel_gtk, updater)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Updater"),
    .description = N_("Checks for updates"),
    .new_instance = updater_constructor,
    .reconfigure = updater_configuration_changed,
    .button_press_event = updater_button_press_event,
    .config = updater_configure,
    .control = updater_control_msg,
    .gettext_package = GETTEXT_PACKAGE
};


/* End of file */
/*----------------------------------------------------------------------------*/
