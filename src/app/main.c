/* main.c — skimmer-for-linux GTK4/libadwaita front-end. M0: an empty window
 * with a placeholder status page. The station list + decode log land in M5
 * (docs/SCOPE.md); the engine under src/engine/ stays headless and GLib-only.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0.0.0"
#endif

static void on_activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  GtkWidget *window = adw_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Skimmer for Linux");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);

  GtkWidget *header = adw_header_bar_new();

  AdwStatusPage *status = ADW_STATUS_PAGE(adw_status_page_new());
  adw_status_page_set_icon_name(status, "network-wireless-symbolic");
  adw_status_page_set_title(status, "Skimmer for Linux");
  adw_status_page_set_description(
      status, "M0 scaffold — TCI client, channelizer and CW decode land next.\n"
               "See docs/SCOPE.md.");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(status));
  gtk_widget_set_vexpand(GTK_WIDGET(status), TRUE);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
  g_set_application_name("Skimmer for Linux");

  AdwApplication *app =
      adw_application_new("cz.ok1br.SkimmerForLinux", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
