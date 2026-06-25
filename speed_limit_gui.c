// speed_limit_gui.c - GTK3 GUI for limiting combined upload+download speed
//
// Compile: gcc -o speed_limit_gui speed_limit_gui.c $(pkg-config --cflags --libs gtk+-3.0)
// Run:     sudo speed_limit_gui   (root needed for `tc`)
//
// Features:
//   - Pick any network interface from a dropdown
//   - The interface currently carrying the default route is pre-selected
//   - Apply / Remove a combined up+down speed limit (KB/s)
//   - About dialog

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID       "org.speedlimit.gui"
#define ICON_NAME    "speed-limit"
#define APP_VERSION  "2.0"
#define SERVICE_NAME "speed-limit.service"
#define SERVICE_PATH "/etc/systemd/system/" SERVICE_NAME

/* ---- widgets we need to reach from callbacks ---- */
typedef struct {
    GtkWidget *window;
    GtkWidget *iface_combo;
    GtkWidget *speed_spin;
    GtkWidget *status_label;
    GtkWidget *apply_btn;
    GtkWidget *remove_btn;
    GtkWidget *add_service_btn;
    GtkWidget *remove_service_btn;
} AppUI;

/* Run a shell command, return its exit status. */
static int run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return -1;
    return WEXITSTATUS(ret);
}

/* Read the interface that owns the default route (the one "in use"). */
static char *get_active_iface(void) {
    char *iface = NULL;
    FILE *fp = popen("ip route show default 2>/dev/null", "r");
    if (fp) {
        char line[512];
        if (fgets(line, sizeof(line), fp)) {
            char *dev = strstr(line, " dev ");
            if (dev) {
                dev += 5;
                char name[64];
                if (sscanf(dev, "%63s", name) == 1)
                    iface = g_strdup(name);
            }
        }
        pclose(fp);
    }
    return iface;
}

/* Populate the combo box with every non-loopback interface and pre-select
   the active one. */
static void populate_interfaces(GtkComboBoxText *combo) {
    char *active = get_active_iface();
    int active_index = -1, index = 0;

    FILE *fp = popen("ip -o link show 2>/dev/null", "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            // format: "2: wlo1: <...> ..."
            char name[64];
            if (sscanf(line, "%*d: %63[^:]:", name) == 1) {
                if (strcmp(name, "lo") == 0) continue;
                gtk_combo_box_text_append_text(combo, name);
                if (active && strcmp(name, active) == 0)
                    active_index = index;
                index++;
            }
        }
        pclose(fp);
    }

    if (index == 0)
        gtk_combo_box_text_append_text(combo, "wlo1"); // fallback

    gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                             active_index >= 0 ? active_index : 0);
    g_free(active);
}

static void set_status(AppUI *ui, const char *markup) {
    gtk_label_set_markup(GTK_LABEL(ui->status_label), markup);
}

static void show_error(AppUI *ui, const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(ui->window),
        GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* Tear down any existing tc rules on the interface. */
static void clear_rules(const char *iface) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", iface);
    run_cmd(cmd);
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s ingress 2>/dev/null", iface);
    run_cmd(cmd);
}

/* Per-direction kbit/s rate for a given total KB/s. Optionally returns the
   per-direction KB/s in *half_kb_out. */
static int kbps_from_total(int total_kb, int *half_kb_out) {
    int half_kb = total_kb / 2;
    if (half_kb < 1) half_kb = 1;
    if (half_kb_out) *half_kb_out = half_kb;
    return half_kb * 8;
}

/* True if the boot-time service unit is currently installed. */
static gboolean service_installed(void) {
    return g_file_test(SERVICE_PATH, G_FILE_TEST_EXISTS);
}

/* Enable/disable the service buttons to reflect the current install state. */
static void refresh_service_buttons(AppUI *ui) {
    gboolean installed = service_installed();
    gtk_widget_set_sensitive(ui->add_service_btn, !installed);
    gtk_widget_set_sensitive(ui->remove_service_btn, installed);
}

/* Write a systemd oneshot unit that reapplies the tc limit at boot.
   Uses /bin/sh wrappers so tc is found via PATH and del errors are ignored. */
static int write_service_file(const char *iface, int total_kb) {
    int kbps = kbps_from_total(total_kb, NULL);
    int burst_bytes = (kbps * 1000 / 8) / 10;
    if (burst_bytes < 16000) burst_bytes = 16000;

    FILE *fp = fopen(SERVICE_PATH, "w");
    if (!fp) return -1;

    fprintf(fp,
        "[Unit]\n"
        "Description=Network Speed Limiter (%d KB/s total on %s)\n"
        "After=network-online.target\n"
        "Wants=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        "ExecStartPre=-/bin/sh -c 'tc qdisc del dev %s root 2>/dev/null'\n"
        "ExecStartPre=-/bin/sh -c 'tc qdisc del dev %s ingress 2>/dev/null'\n"
        "ExecStart=/bin/sh -c 'tc qdisc add dev %s root handle 1: htb default 10'\n"
        "ExecStart=/bin/sh -c 'tc class add dev %s parent 1: classid 1:1 htb rate %dKbit ceil %dKbit'\n"
        "ExecStart=/bin/sh -c 'tc class add dev %s parent 1:1 classid 1:10 htb rate %dKbit ceil %dKbit'\n"
        "ExecStart=/bin/sh -c 'tc filter add dev %s parent 1: protocol ip prio 1 u32 match ip src 0.0.0.0/0 flowid 1:10'\n"
        "ExecStart=/bin/sh -c 'tc qdisc add dev %s handle ffff: ingress'\n"
        "ExecStart=/bin/sh -c 'tc filter add dev %s parent ffff: protocol ip u32 match u32 0 0 police rate %dKbit burst %dKb drop flowid :1'\n"
        "ExecStop=-/bin/sh -c 'tc qdisc del dev %s root 2>/dev/null'\n"
        "ExecStop=-/bin/sh -c 'tc qdisc del dev %s ingress 2>/dev/null'\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        total_kb, iface,
        iface, iface,
        iface,
        iface, kbps, kbps,
        iface, kbps, kbps,
        iface,
        iface,
        iface, kbps, burst_bytes / 1000,
        iface, iface);

    fclose(fp);
    return 0;
}

/* If a service unit exists, read the saved speed (and interface) back out of
   its Description line: "... (%d KB/s total on %s)". Returns the total KB/s,
   or 0 if not found. On success, *iface_out (caller frees) gets the iface. */
static int read_service_settings(char **iface_out) {
    if (iface_out) *iface_out = NULL;
    FILE *fp = fopen(SERVICE_PATH, "r");
    if (!fp) return 0;

    int total_kb = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "(");
        char iface[64];
        if (p && sscanf(p, "(%d KB/s total on %63[^)]", &total_kb, iface) == 2) {
            if (iface_out) *iface_out = g_strdup(iface);
            break;
        }
        total_kb = 0;
    }
    fclose(fp);
    return total_kb;
}

/* Select the given interface name in the combo, if present. */
static void select_iface(GtkComboBoxText *combo, const char *iface) {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter iter;
    int index = 0;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gchar *name = NULL;
        gtk_tree_model_get(model, &iter, 0, &name, -1);
        gboolean match = (name && strcmp(name, iface) == 0);
        g_free(name);
        if (match) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), index);
            return;
        }
        index++;
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

static void on_add_service(GtkButton *btn, gpointer data) {
    (void)btn;
    AppUI *ui = data;

    gchar *iface = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(ui->iface_combo));
    if (!iface) { show_error(ui, "No interface selected."); return; }

    int total_kb = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(ui->speed_spin));
    if (total_kb <= 0) {
        show_error(ui, "Speed must be greater than 0.");
        g_free(iface);
        return;
    }

    if (write_service_file(iface, total_kb) != 0) {
        show_error(ui, "Could not write the service file.\n"
                       "Make sure you are running as root (sudo).");
        g_free(iface);
        return;
    }

    char cmd[256];
    int ok = (run_cmd("systemctl daemon-reload") == 0);
    snprintf(cmd, sizeof(cmd), "systemctl enable %s 2>/dev/null", SERVICE_NAME);
    ok &= (run_cmd(cmd) == 0);
    /* Apply it immediately too, so it takes effect without a reboot. */
    snprintf(cmd, sizeof(cmd), "systemctl start %s", SERVICE_NAME);
    run_cmd(cmd);

    if (ok) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "<span foreground='#2e7d32'><b>\xe2\x9c\x93 Installed as a service</b></span>\n"
            "%d KB/s on %s will be applied automatically at boot.",
            total_kb, iface);
        set_status(ui, msg);
    } else {
        set_status(ui,
            "<span foreground='#c62828'><b>Failed to enable the service.</b></span>\n"
            "Make sure systemd is available and you are root (sudo).");
    }

    refresh_service_buttons(ui);
    g_free(iface);
}

static void on_remove_service(GtkButton *btn, gpointer data) {
    (void)btn;
    AppUI *ui = data;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl disable --now %s 2>/dev/null", SERVICE_NAME);
    run_cmd(cmd);

    if (remove(SERVICE_PATH) != 0 && service_installed()) {
        show_error(ui, "Could not remove the service file.\n"
                       "Make sure you are running as root (sudo).");
        refresh_service_buttons(ui);
        return;
    }

    run_cmd("systemctl daemon-reload");
    set_status(ui,
        "<span foreground='#555'>Service removed. The limit will no longer "
        "start at boot.</span>");
    refresh_service_buttons(ui);
}

static void on_apply(GtkButton *btn, gpointer data) {
    (void)btn;
    AppUI *ui = data;

    gchar *iface = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(ui->iface_combo));
    if (!iface) { show_error(ui, "No interface selected."); return; }

    int total_kb = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(ui->speed_spin));
    if (total_kb <= 0) {
        show_error(ui, "Speed must be greater than 0.");
        g_free(iface);
        return;
    }

    int half_kb = total_kb / 2;
    if (half_kb < 1) half_kb = 1;
    int kbps = half_kb * 8;

    clear_rules(iface);

    char cmd[512];
    int ok = 1;

    snprintf(cmd, sizeof(cmd),
        "tc qdisc add dev %s root handle 1: htb default 10", iface);
    ok &= (run_cmd(cmd) == 0);

    snprintf(cmd, sizeof(cmd),
        "tc class add dev %s parent 1: classid 1:1 htb rate %dKbit ceil %dKbit",
        iface, kbps, kbps);
    ok &= (run_cmd(cmd) == 0);

    snprintf(cmd, sizeof(cmd),
        "tc class add dev %s parent 1:1 classid 1:10 htb rate %dKbit ceil %dKbit",
        iface, kbps, kbps);
    ok &= (run_cmd(cmd) == 0);

    snprintf(cmd, sizeof(cmd),
        "tc filter add dev %s parent 1: protocol ip prio 1 u32 "
        "match ip src 0.0.0.0/0 flowid 1:10", iface);
    ok &= (run_cmd(cmd) == 0);

    snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s handle ffff: ingress", iface);
    ok &= (run_cmd(cmd) == 0);

    int burst_bytes = (kbps * 1000 / 8) / 10;
    if (burst_bytes < 16000) burst_bytes = 16000;
    snprintf(cmd, sizeof(cmd),
        "tc filter add dev %s parent ffff: protocol ip u32 match u32 0 0 "
        "police rate %dKbit burst %dKb drop flowid :1",
        iface, kbps, burst_bytes / 1000);
    ok &= (run_cmd(cmd) == 0);

    if (ok) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "<span foreground='#2e7d32'><b>\xe2\x9c\x93 Limit active on %s</b></span>\n"
            "%d KB/s up + %d KB/s down (%d KB/s total)",
            iface, half_kb, half_kb, half_kb * 2);
        set_status(ui, msg);
        gtk_widget_set_sensitive(ui->remove_btn, TRUE);
    } else {
        clear_rules(iface);
        set_status(ui,
            "<span foreground='#c62828'><b>Failed to apply limit.</b></span>\n"
            "Make sure you are running as root (sudo).");
    }

    g_free(iface);
}

static void on_remove(GtkButton *btn, gpointer data) {
    (void)btn;
    AppUI *ui = data;
    gchar *iface = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(ui->iface_combo));
    if (!iface) return;
    clear_rules(iface);
    char msg[128];
    snprintf(msg, sizeof(msg),
        "<span foreground='#555'>Limit removed on %s.</span>", iface);
    set_status(ui, msg);
    gtk_widget_set_sensitive(ui->remove_btn, FALSE);
    g_free(iface);
}

static void on_about(GtkButton *btn, gpointer data) {
    (void)btn;
    AppUI *ui = data;
    const char *authors[] = { "Jean-Francois Lachance-Caumartin", NULL };
    gtk_show_about_dialog(GTK_WINDOW(ui->window),
        "program-name", "Network Speed Limiter",
        "version", APP_VERSION,
        "comments", "Limit combined upload + download bandwidth on a network "
                    "interface using Linux traffic control (tc).",
        "logo-icon-name", ICON_NAME,
        "icon-name", ICON_NAME,
        "authors", authors,
        "copyright", "\xc2\xa9 2026 Jean-Francois Lachance-Caumartin",
        "license-type", GTK_LICENSE_MIT_X11,
        "website-label", "Network Speed Limiter",
        NULL);
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AppUI *ui = g_new0(AppUI, 1);

    ui->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui->window), "Network Speed Limiter");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 420, 260);
    gtk_window_set_icon_name(GTK_WINDOW(ui->window), ICON_NAME);

    /* Header bar with an About button */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Network Speed Limiter");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    GtkWidget *about_btn = gtk_button_new_from_icon_name(
        "help-about-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(about_btn, "About");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), about_btn);
    gtk_window_set_titlebar(GTK_WINDOW(ui->window), header);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    g_object_set(grid, "margin", 18, NULL);
    gtk_container_add(GTK_CONTAINER(ui->window), grid);

    /* Interface row */
    GtkWidget *iface_lbl = gtk_label_new("Interface:");
    gtk_widget_set_halign(iface_lbl, GTK_ALIGN_START);
    ui->iface_combo = gtk_combo_box_text_new();
    populate_interfaces(GTK_COMBO_BOX_TEXT(ui->iface_combo));
    gtk_widget_set_hexpand(ui->iface_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), iface_lbl,        0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->iface_combo,  1, 0, 1, 1);

    /* Speed row */
    GtkWidget *speed_lbl = gtk_label_new("Total speed (KB/s):");
    gtk_widget_set_halign(speed_lbl, GTK_ALIGN_START);
    ui->speed_spin = gtk_spin_button_new_with_range(1, 1000000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->speed_spin), 500);
    gtk_grid_attach(GTK_GRID(grid), speed_lbl,       0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->speed_spin,  1, 1, 1, 1);

    /* Buttons row */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui->apply_btn  = gtk_button_new_with_label("Apply Limit");
    ui->remove_btn = gtk_button_new_with_label("Remove Limit");
    gtk_style_context_add_class(
        gtk_widget_get_style_context(ui->apply_btn), "suggested-action");
    gtk_widget_set_sensitive(ui->remove_btn, FALSE);
    gtk_box_pack_start(GTK_BOX(btn_box), ui->apply_btn,  TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), ui->remove_btn, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(grid), btn_box, 0, 2, 2, 1);

    /* Service buttons row */
    GtkWidget *svc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui->add_service_btn    = gtk_button_new_with_label("Add as Service");
    ui->remove_service_btn = gtk_button_new_with_label("Remove Service");
    gtk_widget_set_tooltip_text(ui->add_service_btn,
        "Install a systemd service so the current limit is applied at boot.");
    gtk_widget_set_tooltip_text(ui->remove_service_btn,
        "Remove the boot-time service.");
    gtk_box_pack_start(GTK_BOX(svc_box), ui->add_service_btn,    TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(svc_box), ui->remove_service_btn, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(grid), svc_box, 0, 3, 2, 1);

    /* Status */
    ui->status_label = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(ui->status_label), TRUE);
    gtk_label_set_justify(GTK_LABEL(ui->status_label), GTK_JUSTIFY_CENTER);
    set_status(ui, "<span foreground='#555'>Ready. Select an interface and "
                   "apply a limit.</span>");
    gtk_grid_attach(GTK_GRID(grid), ui->status_label, 0, 4, 2, 1);

    refresh_service_buttons(ui);

    /* If a limit is already installed as a service, restore its saved speed
       and interface instead of showing the defaults. */
    {
        char *saved_iface = NULL;
        int saved_kb = read_service_settings(&saved_iface);
        if (saved_kb > 0) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->speed_spin), saved_kb);
            if (saved_iface)
                select_iface(GTK_COMBO_BOX_TEXT(ui->iface_combo), saved_iface);
            char msg[256];
            snprintf(msg, sizeof(msg),
                "<span foreground='#2e7d32'>Service installed: %d KB/s on %s "
                "(applied at boot).</span>",
                saved_kb, saved_iface ? saved_iface : "?");
            set_status(ui, msg);
        }
        g_free(saved_iface);
    }

    g_signal_connect(ui->apply_btn,  "clicked", G_CALLBACK(on_apply),  ui);
    g_signal_connect(ui->remove_btn, "clicked", G_CALLBACK(on_remove), ui);
    g_signal_connect(ui->add_service_btn,    "clicked",
                     G_CALLBACK(on_add_service),    ui);
    g_signal_connect(ui->remove_service_btn, "clicked",
                     G_CALLBACK(on_remove_service), ui);
    g_signal_connect(about_btn,      "clicked", G_CALLBACK(on_about),  ui);
    g_signal_connect_swapped(ui->window, "destroy", G_CALLBACK(g_free), ui);

    gtk_widget_show_all(ui->window);
}

int main(int argc, char **argv) {
    /* This GUI runs as root (via pkexec) on the user's shared X display. GTK
     * would otherwise auto-start a root-owned at-spi accessibility bus under
     * /root/.cache/at-spi and stamp its path into the per-display AT_SPI_BUS
     * X root-window property, which every other (non-root) client then fails
     * to connect to ("Permission denied"). We need no accessibility bridge,
     * so disable it before GTK initialises and leave the shared state alone. */
    g_setenv("NO_AT_BRIDGE", "1", TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
