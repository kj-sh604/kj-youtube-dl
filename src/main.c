// kj-youtube-dl - GTK3 GUI wrapper for yt-dlp
// See LICENSE file for copyright and license details.

// expose posix prototypes (kill, setpgid, stat) under strict c99
#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

enum { FMT_BEST = 0, FMT_MP4, FMT_WEBM, FMT_M4A, FMT_MPV, FMT_COUNT };

// yt-dlp format strings for each output type
static const char *format_args[] = {
    [FMT_BEST] = "bestvideo+bestaudio/best",
    [FMT_MP4] = "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best",
    [FMT_WEBM] = "bestvideo[ext=webm]+bestaudio[ext=webm]/best[ext=webm]/best",
    [FMT_M4A] = "bestaudio[ext=m4a]",
    [FMT_MPV] = NULL};

// human-readable format names for UI
static const char *format_names[] = {[FMT_MP4] = "mp4",
                                     [FMT_WEBM] = "webm",
                                     [FMT_M4A] = "m4a",
                                     [FMT_BEST] = "best (auto)",
                                     [FMT_MPV] = "mpv (stream)"};

// application state and UI widget references
typedef struct {
  GtkWidget *window;
  GtkWidget *url_entry;
  GtkWidget *format_combo;
  GtkWidget *browser_combo;
  GtkWidget *dir_entry;
  GtkWidget *dir_button;
  GtkWidget *download_button;
  GtkWidget *stop_button; // cancel running download/stream
  GtkWidget *status_label;
  GtkWidget *progress_bar;
  GtkWidget *no_playlist_check; // --no-playlist toggle
  char *download_dir;           // user's selected download directory
  GPid child_pid;               // currently spawned process pid, 0 when idle
  int is_streaming;             // current child is an mpv stream
  guint pulse_timer;            // progress pulse timer id, 0 when inactive
  int ytdlp_available;          // yt-dlp availability flag
  int mpv_available;            // mpv availability flag
} AppState;

static const char *get_home_dir(void) {
  const char *home = g_get_home_dir();
  return home ? home : "/tmp";
}

// check if a binary exists in PATH without spawning a shell
static int binary_exists(const char *name) {
  gchar *path = g_find_program_in_path(name);

  if (path == NULL)
    return 0;

  g_free(path);
  return 1;
}

// check if a browser profile directory exists
static int browser_profile_exists(const char *path) {
  char expanded_path[1024];
  const char *home;
  struct stat st;

  if (path == NULL)
    return 0;

  // expand ~ to home directory
  if (path[0] == '~') {
    home = get_home_dir();
    snprintf(expanded_path, sizeof(expanded_path), "%s%s", home, path + 1);
  } else {
    snprintf(expanded_path, sizeof(expanded_path), "%s", path);
  }

  return (stat(expanded_path, &st) == 0 && S_ISDIR(st.st_mode));
}

// caller must g_free the returned path
static gchar *get_config_path(void) {
  const char *config_dir = g_getenv("XDG_CONFIG_HOME");

  if (config_dir != NULL && config_dir[0] != '\0')
    return g_build_filename(config_dir, "kj-youtube-dl", NULL);

  return g_build_filename(get_home_dir(), ".config", "kj-youtube-dl", NULL);
}

static void ensure_config_dir(void) {
  gchar *dir = get_config_path();
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
}

static void save_download_dir(const char *dir) {
  gchar *config_dir;
  gchar *path;
  FILE *fp;

  ensure_config_dir();

  config_dir = get_config_path();
  path = g_build_filename(config_dir, "download_dir", NULL);
  g_free(config_dir);

  fp = fopen(path, "w");
  if (fp != NULL) {
    fprintf(fp, "%s\n", dir);
    fclose(fp);
  }

  g_free(path);
}

static char *load_download_dir(void) {
  gchar *config_dir;
  gchar *path;
  gchar *contents = NULL;

  config_dir = get_config_path();
  path = g_build_filename(config_dir, "download_dir", NULL);
  g_free(config_dir);

  if (!g_file_get_contents(path, &contents, NULL, NULL)) {
    g_free(path);
    return NULL;
  }

  g_free(path);
  g_strchomp(contents);

  if (contents[0] == '\0') {
    g_free(contents);
    return NULL;
  }

  return contents;
}

static char *get_default_download_dir(void) {
  char *saved = load_download_dir();

  if (saved != NULL)
    return saved;

  return g_build_filename(get_home_dir(), DEFAULT_DOWNLOAD_DIR, NULL);
}

static void show_error(GtkWidget *parent, const char *message) {
  GtkWidget *dialog;

  dialog = gtk_message_dialog_new(
      GTK_WINDOW(parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void show_info(GtkWidget *parent, const char *message) {
  GtkWidget *dialog;

  dialog = gtk_message_dialog_new(
      GTK_WINDOW(parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", message);

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void set_status(AppState *app, const char *status) {
  gtk_label_set_text(GTK_LABEL(app->status_label), status);
}

static void on_dir_button_clicked(GtkWidget *button, gpointer data) {
  AppState *app = (AppState *)data;
  GtkWidget *dialog;
  gint res;

  (void)button;

  dialog = gtk_file_chooser_dialog_new(
      "choose download directory", GTK_WINDOW(app->window),
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_cancel", GTK_RESPONSE_CANCEL,
      "_download _here", GTK_RESPONSE_ACCEPT, NULL);

  gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), app->download_dir);

  res = gtk_dialog_run(GTK_DIALOG(dialog));
  if (res == GTK_RESPONSE_ACCEPT) {
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));

    if (filename != NULL) {
      g_free(app->download_dir);
      app->download_dir = filename;
      gtk_entry_set_text(GTK_ENTRY(app->dir_entry), app->download_dir);
      save_download_dir(app->download_dir);
    }
  }

  gtk_widget_destroy(dialog);
}

// require an http(s) scheme so a pasted url can never be
// interpreted as a command line option by yt-dlp or mpv
static int validate_url(const char *url) {
  if (url == NULL)
    return 0;

  return (g_str_has_prefix(url, "http://") ||
          g_str_has_prefix(url, "https://"));
}

static void on_stop_clicked(GtkWidget *button, gpointer data) {
  AppState *app = (AppState *)data;

  (void)button;

  if (app->child_pid > 0) {
    // signal the whole process group, not just the leader
    kill(-(app->child_pid), SIGTERM);
    app->child_pid = 0;
    app->is_streaming = 0;
    gtk_widget_show(app->download_button);
    gtk_widget_hide(app->stop_button);
    gtk_widget_set_sensitive(app->url_entry, TRUE);
    gtk_widget_set_sensitive(app->format_combo, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
    set_status(app, "canceled.");
  }
}

static void on_child_watch(GPid pid, gint status, gpointer data) {
  AppState *app = (AppState *)data;

  g_spawn_close_pid(pid);

  // ignore exits from canceled or superseded children
  if (pid != app->child_pid)
    return;

  app->child_pid = 0;

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    set_status(app, app->is_streaming ? "stream ended."
                                      : "download completed successfully!");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar),
                                  app->is_streaming ? 0.0 : 1.0);
  } else {
    set_status(app, app->is_streaming
                        ? "stream failed, check url and try again."
                        : "download failed, check url and try again.");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
  }

  app->is_streaming = 0;

  gtk_widget_show(app->download_button);
  gtk_widget_hide(app->stop_button);
  gtk_widget_set_sensitive(app->url_entry, TRUE);
  gtk_widget_set_sensitive(app->format_combo, TRUE);
}

static gboolean pulse_progress(gpointer data) {
  AppState *app = (AppState *)data;

  if (app->child_pid == 0 || !gtk_widget_is_visible(app->stop_button)) {
    app->pulse_timer = 0;
    return G_SOURCE_REMOVE;
  }

  gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress_bar));
  return G_SOURCE_CONTINUE;
}

// run each download in its own process group so stop can kill the
// whole pipeline, including any ffmpeg helper processes
static void child_setup(gpointer data) {
  (void)data;

  setpgid(0, 0);
}

// spawn yt-dlp or mpv when the download button is clicked
static void on_download_clicked(GtkWidget *button, gpointer data) {
  AppState *app = (AppState *)data;
  gchar *url;
  gchar *browser = NULL;
  GPid pid;
  GError *error = NULL;
  gchar *argv[12];
  int format_idx;
  int i = 0;

  (void)button;

  // strip stray whitespace from pasted urls before building argv
  url = g_strdup(gtk_entry_get_text(GTK_ENTRY(app->url_entry)));
  g_strstrip(url);

  if (!validate_url(url)) {
    show_error(app->window, "please enter a valid video url.");
    g_free(url);
    return;
  }

  format_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->format_combo));
  if (format_idx < 0 || format_idx >= FMT_COUNT) {
    show_error(app->window, "please select a format.");
    g_free(url);
    return;
  }

  g_free(app->download_dir);
  app->download_dir = g_strdup(gtk_entry_get_text(GTK_ENTRY(app->dir_entry)));
  save_download_dir(app->download_dir);

  if (g_mkdir_with_parents(app->download_dir, 0755) != 0) {
    show_error(app->window, "failed to create download directory.");
    g_free(url);
    return;
  }

  if (format_idx == FMT_MPV) {
    argv[i++] = "mpv";
    argv[i++] = "--ytdl-format=bestvideo+bestaudio/best";
    argv[i++] = url;
  } else {
    int browser_idx =
        gtk_combo_box_get_active(GTK_COMBO_BOX(app->browser_combo));
    int no_playlist =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->no_playlist_check));

    argv[i++] = "yt-dlp";
    argv[i++] = "-c";
    argv[i++] = "-i";
    argv[i++] = "-f";
    argv[i++] = (gchar *)format_args[format_idx];

    if (no_playlist)
      argv[i++] = "--no-playlist";

    if (browser_idx > 0) {
      GtkTreeModel *model =
          gtk_combo_box_get_model(GTK_COMBO_BOX(app->browser_combo));
      GtkTreeIter iter;

      if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(app->browser_combo),
                                        &iter))
        gtk_tree_model_get(model, &iter, 0, &browser, -1);
    }

    if (browser != NULL) {
      argv[i++] = "--cookies-from-browser";
      argv[i++] = browser;
    }

    argv[i++] = url;
  }
  argv[i] = NULL;

  gtk_widget_hide(app->download_button);
  gtk_widget_show(app->stop_button);
  gtk_widget_set_sensitive(app->url_entry, FALSE);
  gtk_widget_set_sensitive(app->format_combo, FALSE);

  set_status(app,
             format_idx == FMT_MPV ? "streaming in mpv..." : "downloading...");

  // pass arguments directly, no shell means no injection risk
  if (!g_spawn_async(app->download_dir, argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                     child_setup, NULL, &pid, &error)) {

    show_error(app->window, error->message);
    g_error_free(error);

    gtk_widget_show(app->download_button);
    gtk_widget_hide(app->stop_button);
    gtk_widget_set_sensitive(app->url_entry, TRUE);
    gtk_widget_set_sensitive(app->format_combo, TRUE);
    set_status(app, "ready");
  } else {
    app->child_pid = pid;
    app->is_streaming = (format_idx == FMT_MPV);
    g_child_watch_add(pid, on_child_watch, app);

    if (app->pulse_timer == 0)
      app->pulse_timer = g_timeout_add(100, pulse_progress, app);
  }

  g_free(url);
  g_free(browser);
}

static void on_url_activate(GtkWidget *entry, gpointer data) {
  (void)entry;
  on_download_clicked(NULL, data);
}

static void setup_format_combo(AppState *app) {
  int i;
  char label[64];

  for (i = 0; i < FMT_COUNT; i++) {
    if (i == FMT_MPV && !app->mpv_available)
      snprintf(label, sizeof(label), "%s (not installed)", format_names[i]);
    else if (i != FMT_MPV && !app->ytdlp_available)
      snprintf(label, sizeof(label), "%s (yt-dlp not installed)",
               format_names[i]);
    else
      snprintf(label, sizeof(label), "%s", format_names[i]);

    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->format_combo),
                                   label);
  }

  if (app->ytdlp_available)
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->format_combo), FMT_BEST);
  else if (app->mpv_available)
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->format_combo), FMT_MPV);
}

static void setup_browser_combo(AppState *app) {
  static const struct {
    const char *name;
    const char *path;
  } browsers[] = {{"chrome", "~/.config/google-chrome"},
                  {"chromium", "~/.config/chromium"},
                  {"firefox", "~/.mozilla/firefox"},
                  {"brave", "~/.config/BraveSoftware/Brave-Browser"},
                  {"edge", "~/.config/microsoft-edge"},
                  {"opera", "~/.config/opera"},
                  {"vivaldi", "~/.config/vivaldi"},
                  {NULL, NULL}};

  int i;

  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->browser_combo),
                                 "none");

  for (i = 0; browsers[i].name != NULL; i++) {
    if (browser_profile_exists(browsers[i].path)) {
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->browser_combo),
                                     browsers[i].name);
    }
  }

  gtk_combo_box_set_active(GTK_COMBO_BOX(app->browser_combo), 0);
}

static void on_format_changed(GtkWidget *combo, gpointer data) {
  AppState *app = (AppState *)data;
  int format_idx;

  (void)combo;

  format_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->format_combo));

  // update button label based on format
  if (format_idx == FMT_MPV) {
    gtk_button_set_label(GTK_BUTTON(app->download_button), "stream");
  } else {
    gtk_button_set_label(GTK_BUTTON(app->download_button), "download");
  }

  if (format_idx == FMT_MPV && !app->mpv_available) {
    gtk_widget_set_sensitive(app->download_button, FALSE);
    set_status(app, "mpv is not installed");
  } else if (format_idx != FMT_MPV && !app->ytdlp_available) {
    gtk_widget_set_sensitive(app->download_button, FALSE);
    set_status(app, "yt-dlp is not installed");
  } else {
    gtk_widget_set_sensitive(app->download_button, TRUE);
    set_status(app, "ready");
  }
}

// create app icon: red circle with white play triangle
static GdkPixbuf *create_icon_pixbuf(int size) {
  cairo_surface_t *surface;
  cairo_t *cr;
  GdkPixbuf *pixbuf;
  double cx, cy, r;

  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cr = cairo_create(surface);

  cx = size / 2.0;
  cy = size / 2.0;
  r = size / 2.0 - 1;

  // #69baa7 circle background
  cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
  cairo_set_source_rgb(cr, 0x69 / 255.0, 0xba / 255.0, 0xa7 / 255.0);
  cairo_fill(cr);

  // play triangle
  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_move_to(cr, cx - r * 0.3, cy - r * 0.5);
  cairo_line_to(cr, cx + r * 0.5, cy);
  cairo_line_to(cr, cx - r * 0.3, cy + r * 0.5);
  cairo_close_path(cr);
  cairo_fill(cr);

  cairo_destroy(cr);

  pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);
  cairo_surface_destroy(surface);

  return pixbuf;
}

static void set_window_icon(GtkWidget *window) {
  GdkPixbuf *icon16, *icon32, *icon48;
  GList *icons = NULL;

  icon16 = create_icon_pixbuf(16);
  icon32 = create_icon_pixbuf(32);
  icon48 = create_icon_pixbuf(48);

  if (icon16)
    icons = g_list_append(icons, icon16);
  if (icon32)
    icons = g_list_append(icons, icon32);
  if (icon48)
    icons = g_list_append(icons, icon48);

  if (icons)
    gtk_window_set_icon_list(GTK_WINDOW(window), icons);

  g_list_free_full(icons, g_object_unref);
}

// closing the window cancels a running download, except when streaming with mpv
static void on_window_destroy(GtkWidget *widget, gpointer data) {
  AppState *app = (AppState *)data;

  (void)widget;

  if (app->child_pid > 0 && !app->is_streaming)
    kill(-(app->child_pid), SIGTERM);

  gtk_main_quit();
}

static void create_ui(AppState *app) {
  GtkWidget *vbox, *grid, *label, *hbox;

  app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(app->window), WINDOW_TITLE);
  gtk_window_set_default_size(GTK_WINDOW(app->window), 500, 200);
  gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(app->window), 15);

  set_window_icon(app->window);

  g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);

  vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_add(GTK_CONTAINER(app->window), vbox);

  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

  // URL input
  label = gtk_label_new("url:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

  app->url_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(app->url_entry),
                                 "https://www.youtube.com/watch?v=...");
  gtk_widget_set_hexpand(app->url_entry, TRUE);
  gtk_grid_attach(GTK_GRID(grid), app->url_entry, 1, 0, 2, 1);
  g_signal_connect(app->url_entry, "activate", G_CALLBACK(on_url_activate),
                   app);

  // format selection
  label = gtk_label_new("pipe-to:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);

  app->format_combo = gtk_combo_box_text_new();
  gtk_widget_set_hexpand(app->format_combo, TRUE);
  gtk_grid_attach(GTK_GRID(grid), app->format_combo, 1, 1, 2, 1);
  g_signal_connect(app->format_combo, "changed", G_CALLBACK(on_format_changed),
                   app);

  // cookies from browser
  label = gtk_label_new("cookies:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);

  app->browser_combo = gtk_combo_box_text_new();
  gtk_widget_set_hexpand(app->browser_combo, TRUE);
  gtk_grid_attach(GTK_GRID(grid), app->browser_combo, 1, 2, 2, 1);

  // no playlist checkbox
  app->no_playlist_check = gtk_check_button_new_with_label("--no-playlist");
  gtk_widget_set_halign(app->no_playlist_check, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), app->no_playlist_check, 1, 3, 2, 1);

  // download directory
  label = gtk_label_new("save-to:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, 4, 1, 1);

  app->dir_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(app->dir_entry), app->download_dir);
  gtk_widget_set_hexpand(app->dir_entry, TRUE);
  gtk_grid_attach(GTK_GRID(grid), app->dir_entry, 1, 4, 1, 1);

  app->dir_button = gtk_button_new_with_label("browse...");
  gtk_grid_attach(GTK_GRID(grid), app->dir_button, 2, 4, 1, 1);
  g_signal_connect(app->dir_button, "clicked",
                   G_CALLBACK(on_dir_button_clicked), app);

  // progress bar
  app->progress_bar = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), FALSE);
  gtk_box_pack_start(GTK_BOX(vbox), app->progress_bar, FALSE, FALSE, 5);

  // status and download button
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

  app->status_label = gtk_label_new("ready");
  gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(hbox), app->status_label, TRUE, TRUE, 0);

  app->download_button = gtk_button_new_with_label("download");
  gtk_widget_set_size_request(app->download_button, 120, -1);
  gtk_box_pack_end(GTK_BOX(hbox), app->download_button, FALSE, FALSE, 0);
  g_signal_connect(app->download_button, "clicked",
                   G_CALLBACK(on_download_clicked), app);

  app->stop_button = gtk_button_new_with_label("stop");
  gtk_widget_set_size_request(app->stop_button, 120, -1);
  gtk_box_pack_end(GTK_BOX(hbox), app->stop_button, FALSE, FALSE, 0);
  g_signal_connect(app->stop_button, "clicked", G_CALLBACK(on_stop_clicked),
                   app);
  gtk_widget_set_no_show_all(app->stop_button, TRUE);

  setup_format_combo(app);
  setup_browser_combo(app);
}

static void init_app(AppState *app) {
  memset(app, 0, sizeof(*app));
  app->child_pid = 0;
  app->ytdlp_available = binary_exists("yt-dlp");
  app->mpv_available = binary_exists("mpv");
  app->download_dir = get_default_download_dir();
}

static void cleanup_app(AppState *app) { g_free(app->download_dir); }

static void check_dependencies(AppState *app) {
  if (!app->ytdlp_available && !app->mpv_available) {
    show_error(app->window, "neither yt-dlp nor mpv is installed.\n\n"
                            "please install at least one:\n"
                            "  sudo pacman -S yt-dlp mpv  (Arch)\n"
                            "  sudo apt install yt-dlp mpv  (Debian/Ubuntu)\n"
                            "  brew install yt-dlp mpv  (macOS)");
  } else if (!app->ytdlp_available) {
    show_info(app->window,
              "yt-dlp is not installed, only 'mpv (stream)' is available.\n\n"
              "to enable downloading, install yt-dlp.");
  }
}

int main(int argc, char *argv[]) {
  AppState app;

  gtk_init(&argc, &argv);

  init_app(&app);
  create_ui(&app);

  gtk_widget_show_all(app.window);

  check_dependencies(&app);
  on_format_changed(NULL, &app);

  gtk_main();

  cleanup_app(&app);

  return 0;
}
