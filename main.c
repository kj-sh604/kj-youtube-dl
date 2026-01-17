// kj-youtube-dl - GTK3 GUI wrapper for yt-dlp
// See LICENSE file for copyright and license details.

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "config.h"

enum {
	FMT_BEST = 0,
	FMT_MP4,
	FMT_WEBM,
	FMT_M4A,
	FMT_MPV,
	FMT_COUNT
};

static const char *format_args[] = {
	[FMT_BEST] = "-cif 'bestvideo+bestaudio/best'",
	[FMT_MP4]  = "-cif 'bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/best'",
	[FMT_WEBM] = "-cif 'bestvideo[ext=webm]+bestaudio[ext=webm]/best[ext=webm]/best'",
	[FMT_M4A]  = "-cif 'bestaudio[ext=m4a]'",
	[FMT_MPV]  = NULL
};

static const char *format_names[] = {
	[FMT_BEST] = "Best Quality",
	[FMT_MP4]  = "MP4 (Video)",
	[FMT_WEBM] = "WebM (Video)",
	[FMT_M4A]  = "M4A (Audio)",
	[FMT_MPV]  = "Play in mpv"
};

typedef struct {
	GtkWidget *window;
	GtkWidget *url_entry;
	GtkWidget *format_combo;
	GtkWidget *dir_entry;
	GtkWidget *dir_button;
	GtkWidget *download_button;
	GtkWidget *status_label;
	GtkWidget *progress_bar;
	char      *download_dir;
	int        ytdlp_available;
	int        mpv_available;
} AppState;

static int
binary_exists(const char *name)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
	return (system(cmd) == 0);
}

static const char *
get_home_dir(void)
{
	const char *home = getenv("HOME");
	return home ? home : "/tmp";
}

static char *
get_config_path(void)
{
	static char path[512];
	const char *config_dir = getenv("XDG_CONFIG_HOME");

	if (config_dir != NULL)
		snprintf(path, sizeof(path), "%s/kj-youtube-dl", config_dir);
	else
		snprintf(path, sizeof(path), "%s/.config/kj-youtube-dl", get_home_dir());

	return path;
}

static void
ensure_config_dir(void)
{
	mkdir(get_config_path(), 0755);
}

static void
save_download_dir(const char *dir)
{
	char path[1024];
	FILE *fp;

	ensure_config_dir();
	snprintf(path, sizeof(path), "%s/download_dir", get_config_path());

	fp = fopen(path, "w");
	if (fp != NULL) {
		fprintf(fp, "%s\n", dir);
		fclose(fp);
	}
}

static char *
load_download_dir(void)
{
	char path[1024];
	char buf[1024];
	FILE *fp;
	size_t len;

	snprintf(path, sizeof(path), "%s/download_dir", get_config_path());

	fp = fopen(path, "r");
	if (fp == NULL)
		return NULL;

	if (fgets(buf, sizeof(buf), fp) != NULL) {
		len = strlen(buf);
		if (len > 0 && buf[len - 1] == '\n')
			buf[len - 1] = '\0';
		fclose(fp);
		return g_strdup(buf);
	}

	fclose(fp);
	return NULL;
}

static char *
get_default_download_dir(void)
{
	char *saved;
	char path[1024];

	saved = load_download_dir();
	if (saved != NULL)
		return saved;

	snprintf(path, sizeof(path), "%s/%s", get_home_dir(), DEFAULT_DOWNLOAD_DIR);
	return g_strdup(path);
}

static void
show_error(GtkWidget *parent, const char *message)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);

	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

static void
show_info(GtkWidget *parent, const char *message)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new(GTK_WINDOW(parent),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", message);

	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}

static void
set_status(AppState *app, const char *status)
{
	gtk_label_set_text(GTK_LABEL(app->status_label), status);
}

static void
on_dir_button_clicked(GtkWidget *button, gpointer data)
{
	AppState *app = (AppState *)data;
	GtkWidget *dialog;
	gint res;

	(void)button;

	dialog = gtk_file_chooser_dialog_new("Select Download Directory",
		GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
		"_Cancel", GTK_RESPONSE_CANCEL,
		"_Select", GTK_RESPONSE_ACCEPT, NULL);

	gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), app->download_dir);

	res = gtk_dialog_run(GTK_DIALOG(dialog));
	if (res == GTK_RESPONSE_ACCEPT) {
		g_free(app->download_dir);
		app->download_dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		gtk_entry_set_text(GTK_ENTRY(app->dir_entry), app->download_dir);
		save_download_dir(app->download_dir);
	}

	gtk_widget_destroy(dialog);
}

static int
validate_url(const char *url)
{
	if (url == NULL || strlen(url) == 0)
		return 0;

	if (strstr(url, "youtube.com") != NULL ||
	    strstr(url, "youtu.be") != NULL ||
	    strstr(url, "vimeo.com") != NULL ||
	    strstr(url, "twitch.tv") != NULL ||
	    strstr(url, "dailymotion.com") != NULL ||
	    strstr(url, "http://") != NULL ||
	    strstr(url, "https://") != NULL)
		return 1;

	return 0;
}

static void
on_child_watch(GPid pid, gint status, gpointer data)
{
	AppState *app = (AppState *)data;

	g_spawn_close_pid(pid);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		set_status(app, "Download completed successfully!");
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 1.0);
	} else {
		set_status(app, "Download failed. Check URL and try again.");
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->progress_bar), 0.0);
	}

	gtk_widget_set_sensitive(app->download_button, TRUE);
	gtk_widget_set_sensitive(app->url_entry, TRUE);
	gtk_widget_set_sensitive(app->format_combo, TRUE);
}

static gboolean
pulse_progress(gpointer data)
{
	AppState *app = (AppState *)data;

	if (!gtk_widget_get_sensitive(app->download_button)) {
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->progress_bar));
		return G_SOURCE_CONTINUE;
	}

	return G_SOURCE_REMOVE;
}

static void
on_download_clicked(GtkWidget *button, gpointer data)
{
	AppState *app = (AppState *)data;
	const char *url;
	int format_idx;
	char *cmd;
	GPid pid;
	GError *error = NULL;
	gchar *argv[4];

	(void)button;

	url = gtk_entry_get_text(GTK_ENTRY(app->url_entry));

	if (!validate_url(url)) {
		show_error(app->window, "Please enter a valid video URL.");
		return;
	}

	format_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->format_combo));
	if (format_idx < 0 || format_idx >= FMT_COUNT) {
		show_error(app->window, "Please select a format.");
		return;
	}

	g_free(app->download_dir);
	app->download_dir = g_strdup(gtk_entry_get_text(GTK_ENTRY(app->dir_entry)));
	save_download_dir(app->download_dir);

	if (mkdir(app->download_dir, 0755) != 0 && errno != EEXIST) {
		show_error(app->window, "Failed to create download directory.");
		return;
	}

	if (format_idx == FMT_MPV) {
		cmd = g_strdup_printf("cd '%s' && mpv --ytdl-format='bestvideo+bestaudio/best' '%s'",
			app->download_dir, url);
	} else {
		cmd = g_strdup_printf("cd '%s' && yt-dlp %s '%s'",
			app->download_dir, format_args[format_idx], url);
	}

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = cmd;
	argv[3] = NULL;

	gtk_widget_set_sensitive(app->download_button, FALSE);
	gtk_widget_set_sensitive(app->url_entry, FALSE);
	gtk_widget_set_sensitive(app->format_combo, FALSE);

	set_status(app, format_idx == FMT_MPV ? "Opening in mpv..." : "Downloading...");

	g_timeout_add(100, pulse_progress, app);

	if (!g_spawn_async(NULL, argv, NULL,
		G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
		NULL, NULL, &pid, &error)) {

		show_error(app->window, error->message);
		g_error_free(error);

		gtk_widget_set_sensitive(app->download_button, TRUE);
		gtk_widget_set_sensitive(app->url_entry, TRUE);
		gtk_widget_set_sensitive(app->format_combo, TRUE);
		set_status(app, "Ready");
	} else {
		g_child_watch_add(pid, on_child_watch, app);
	}

	g_free(cmd);
}

static void
on_url_activate(GtkWidget *entry, gpointer data)
{
	(void)entry;
	on_download_clicked(NULL, data);
}

static void
setup_format_combo(AppState *app)
{
	int i;
	char label[64];

	for (i = 0; i < FMT_COUNT; i++) {
		if (i == FMT_MPV && !app->mpv_available)
			snprintf(label, sizeof(label), "%s (not installed)", format_names[i]);
		else if (i != FMT_MPV && !app->ytdlp_available)
			snprintf(label, sizeof(label), "%s (yt-dlp not installed)", format_names[i]);
		else
			snprintf(label, sizeof(label), "%s", format_names[i]);

		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(app->format_combo), label);
	}

	if (app->ytdlp_available)
		gtk_combo_box_set_active(GTK_COMBO_BOX(app->format_combo), FMT_BEST);
	else if (app->mpv_available)
		gtk_combo_box_set_active(GTK_COMBO_BOX(app->format_combo), FMT_MPV);
}

static void
on_format_changed(GtkWidget *combo, gpointer data)
{
	AppState *app = (AppState *)data;
	int format_idx;

	(void)combo;

	format_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(app->format_combo));

	if (format_idx == FMT_MPV && !app->mpv_available) {
		gtk_widget_set_sensitive(app->download_button, FALSE);
		set_status(app, "mpv is not installed");
	} else if (format_idx != FMT_MPV && !app->ytdlp_available) {
		gtk_widget_set_sensitive(app->download_button, FALSE);
		set_status(app, "yt-dlp is not installed");
	} else {
		gtk_widget_set_sensitive(app->download_button, TRUE);
		set_status(app, "Ready");
	}
}

static GdkPixbuf *
create_icon_pixbuf(int size)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	GdkPixbuf *pixbuf;
	double cx, cy, r;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(surface);

	cx = size / 2.0;
	cy = size / 2.0;
	r = size / 2.0 - 1;

	// red circle background
	cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
	cairo_set_source_rgb(cr, 0.8, 0.0, 0.0);
	cairo_fill(cr);

	// white play triangle
	cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
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

static void
set_window_icon(GtkWidget *window)
{
	GdkPixbuf *icon16, *icon32, *icon48;
	GList *icons = NULL;

	icon16 = create_icon_pixbuf(16);
	icon32 = create_icon_pixbuf(32);
	icon48 = create_icon_pixbuf(48);

	if (icon16) icons = g_list_append(icons, icon16);
	if (icon32) icons = g_list_append(icons, icon32);
	if (icon48) icons = g_list_append(icons, icon48);

	if (icons)
		gtk_window_set_icon_list(GTK_WINDOW(window), icons);

	g_list_free_full(icons, g_object_unref);
}

static void
create_ui(AppState *app)
{
	GtkWidget *vbox, *grid, *label, *hbox;

	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app->window), WINDOW_TITLE);
	gtk_window_set_default_size(GTK_WINDOW(app->window), 500, 200);
	gtk_window_set_resizable(GTK_WINDOW(app->window), TRUE);
	gtk_container_set_border_width(GTK_CONTAINER(app->window), 15);

	set_window_icon(app->window);

	g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_add(GTK_CONTAINER(app->window), vbox);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

	// URL input
	label = gtk_label_new("Video URL:");
	gtk_widget_set_halign(label, GTK_ALIGN_END);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);

	app->url_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(app->url_entry),
		"https://www.youtube.com/watch?v=...");
	gtk_widget_set_hexpand(app->url_entry, TRUE);
	gtk_grid_attach(GTK_GRID(grid), app->url_entry, 1, 0, 2, 1);
	g_signal_connect(app->url_entry, "activate", G_CALLBACK(on_url_activate), app);

	// format selection
	label = gtk_label_new("Format:");
	gtk_widget_set_halign(label, GTK_ALIGN_END);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);

	app->format_combo = gtk_combo_box_text_new();
	gtk_widget_set_hexpand(app->format_combo, TRUE);
	gtk_grid_attach(GTK_GRID(grid), app->format_combo, 1, 1, 2, 1);
	g_signal_connect(app->format_combo, "changed", G_CALLBACK(on_format_changed), app);

	// download directory
	label = gtk_label_new("Save to:");
	gtk_widget_set_halign(label, GTK_ALIGN_END);
	gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);

	app->dir_entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(app->dir_entry), app->download_dir);
	gtk_widget_set_hexpand(app->dir_entry, TRUE);
	gtk_grid_attach(GTK_GRID(grid), app->dir_entry, 1, 2, 1, 1);

	app->dir_button = gtk_button_new_with_label("Browse...");
	gtk_grid_attach(GTK_GRID(grid), app->dir_button, 2, 2, 1, 1);
	g_signal_connect(app->dir_button, "clicked", G_CALLBACK(on_dir_button_clicked), app);

	// progress bar
	app->progress_bar = gtk_progress_bar_new();
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app->progress_bar), FALSE);
	gtk_box_pack_start(GTK_BOX(vbox), app->progress_bar, FALSE, FALSE, 5);

	// status and download button
	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	app->status_label = gtk_label_new("Ready");
	gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(hbox), app->status_label, TRUE, TRUE, 0);

	app->download_button = gtk_button_new_with_label("Download");
	gtk_widget_set_size_request(app->download_button, 120, -1);
	gtk_box_pack_end(GTK_BOX(hbox), app->download_button, FALSE, FALSE, 0);
	g_signal_connect(app->download_button, "clicked", G_CALLBACK(on_download_clicked), app);

	setup_format_combo(app);
}

static void
init_app(AppState *app)
{
	memset(app, 0, sizeof(*app));
	app->ytdlp_available = binary_exists("yt-dlp");
	app->mpv_available = binary_exists("mpv");
	app->download_dir = get_default_download_dir();
}

static void
cleanup_app(AppState *app)
{
	g_free(app->download_dir);
}

static void
check_dependencies(AppState *app)
{
	if (!app->ytdlp_available && !app->mpv_available) {
		show_error(app->window,
			"Neither yt-dlp nor mpv is installed.\n\n"
			"Please install at least one:\n"
			"  sudo pacman -S yt-dlp mpv  (Arch)\n"
			"  sudo apt install yt-dlp mpv  (Debian/Ubuntu)\n"
			"  brew install yt-dlp mpv  (macOS)");
	} else if (!app->ytdlp_available) {
		show_info(app->window,
			"yt-dlp is not installed. Only 'Play in mpv' is available.\n\n"
			"To enable downloading, install yt-dlp.");
	}
}

int
main(int argc, char *argv[])
{
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
