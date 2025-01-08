#include <glibmm.h>
#include "updater.hpp"

extern "C" {
    WayfireWidget *create () { return new WayfireUpdater; }
    void destroy (WayfireWidget *w) { delete w; }

    static constexpr conf_table_t conf_table[2] = {
        {CONF_INT,  "interval", N_("Hours between checks for updates")},
        {CONF_NONE, NULL,       NULL}
    };
    const conf_table_t *config_params (void) { return conf_table; };
    const char *display_name (void) { return N_("Updater"); };
    const char *package_name (void) { return GETTEXT_PACKAGE; };
}

void WayfireUpdater::bar_pos_changed_cb (void)
{
    if ((std::string) bar_pos == "bottom") up->bottom = TRUE;
    else up->bottom = FALSE;
}

void WayfireUpdater::icon_size_changed_cb (void)
{
    up->icon_size = icon_size;
    updater_update_display (up);
}

void WayfireUpdater::command (const char *cmd)
{
    updater_control_msg (up, cmd);
}

bool WayfireUpdater::set_icon (void)
{
    updater_update_display (up);
    return false;
}

void WayfireUpdater::settings_changed_cb (void)
{
    up->interval = interval;
    updater_set_interval (up);
}

void WayfireUpdater::init (Gtk::HBox *container)
{
    /* Create the button */
    plugin = std::make_unique <Gtk::Button> ();
    plugin->set_name (PLUGIN_NAME);
    container->pack_start (*plugin, false, false);

    /* Setup structure */
    up = g_new0 (UpdaterPlugin, 1);
    up->plugin = (GtkWidget *)((*plugin).gobj());
    up->icon_size = icon_size;
    icon_timer = Glib::signal_idle().connect (sigc::mem_fun (*this, &WayfireUpdater::set_icon));
    bar_pos_changed_cb ();

    /* Initialise the plugin */
    updater_init (up);

    /* Setup callbacks */
    icon_size.set_callback (sigc::mem_fun (*this, &WayfireUpdater::icon_size_changed_cb));
    bar_pos.set_callback (sigc::mem_fun (*this, &WayfireUpdater::bar_pos_changed_cb));
    interval.set_callback (sigc::mem_fun (*this, &WayfireUpdater::settings_changed_cb));

    settings_changed_cb ();
}

WayfireUpdater::~WayfireUpdater()
{
    icon_timer.disconnect ();
    updater_destructor (up);
}
