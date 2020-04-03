/*
Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
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

#define _GNU_SOURCE
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <X11/Xlib.h>

#include <libintl.h>

#include <curl/curl.h>

#define COVER_SIZE 128

#define CATALOGUE_URL   "https://magazines-static.raspberrypi.org/cat.xml"
#define CACHE_PATH      "/.cache/bookshelf/"
#define PDF_PATH        "/MagPi/"

/* Columns in packages and categories list stores */

#define ITEM_CATEGORY       0
#define ITEM_TITLE          1
#define ITEM_DESC           2
#define ITEM_PDFPATH        3
#define ITEM_COVPATH        4
#define ITEM_FILENAME       5
#define ITEM_DOWNLOADED     6
#define ITEM_COVER          7

#define CAT_MAGPI           0
#define CAT_BOOKS           1
#define CAT_HSPACE          2
#define CAT_WFRAME          3

/*----------------------------------------------------------------------------*/
/* Globals                                                                    */
/*----------------------------------------------------------------------------*/

/* Controls */

static GtkWidget *main_dlg, *items_iv, *close_btn;
static GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_btn, *msg_cancel, *msg_pbv;

/* Preloaded default pixbufs */

static GdkPixbuf *cloud, *grey, *nocover;

/* Data store for icon grid */

GtkListStore *items;
GtkTreeIter selitem;
GtkTreeIter covitem;

/* Catalogue file path */

char *catpath, *cbpath;

/* Libcurl variables */

CURL *http_handle;
CURLM *multi_handle;
FILE *outfile;
guint curl_timer;
void (*term_fn) (int success);
char *fname;
char *tmpname;

/* Flags to manage simulataneous download of art and PDF */

gboolean cover_dl, pdf_dl_req;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static char *get_local_path (char *path, const char *dir);
static char *get_shell_string (char *cmd);
static gboolean net_available (void);
static void create_dir (char *dir);
static void copy_file (char *src, char *dst);
static void start_curl_download (char *url, char *file, int progress, void (*end_fn)(int success));
static gboolean curl_poll (gpointer data);
static void finish_curl_download (int success);
static int progress_func (GtkWidget *bar, double t, double d, double ultotal, double ulnow);
static void abort_curl_download (void);
static GdkPixbuf *get_cover (const char *filename);
static void update_cover_entry (char *lpath, int dl);
static gboolean find_cover_for_item (gpointer data);
static void image_download_done (int success);
static void item_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data);
static void open_pdf (char *path);
static void pdf_download_done (int success);
static void get_pending_pdf (void);
static gboolean get_catalogue (gpointer data);
static void load_catalogue (int success);
static void get_param (char *linebuf, char *name, char **dest);
static int read_data_file (char *path);
static gboolean ok_clicked (GtkButton *button, gpointer data);
static void message (char *msg, int wait, int prog);
static void hide_message (void);
static void close_prog (GtkButton* btn, gpointer ptr);

/*----------------------------------------------------------------------------*/
/* Helpers                                                                    */
/*----------------------------------------------------------------------------*/

/* get_local_path - creates a string with path to file in user's home dir */

static char *get_local_path (char *path, const char *dir)
{
    gchar *basename = g_path_get_basename (path);
    gchar *rpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), dir, basename);
    g_free (basename);
    return rpath;
}

/* get_shell_string - read a string in response to a shell command */

static char *get_shell_string (char *cmd)
{
    char *line = NULL, *res = NULL;
    size_t len = 0;
    FILE *fp = popen (cmd, "r");

    if (fp == NULL) return g_strdup ("");
    if (getline (&line, &len, fp) > 0)
    {
        g_strdelimit (line, "\n\r", 0);
        res = line;
        while (*res++) if (g_ascii_isspace (*res)) *res = 0;
        res = g_strdup (line);
    }
    pclose (fp);
    g_free (line);
    return res ? res : g_strdup ("");
}

/* net_available - determine whether there is a current network connection */

static gboolean net_available (void)
{
    char *ip;
    gboolean val = FALSE;

    ip = get_shell_string ("hostname -I | tr ' ' \\\\n | grep \\\\. | tr \\\\n ','");
    if (ip)
    {
        if (strlen (ip)) val = TRUE;
        g_free (ip);
    }
    return val;
}

/* create dir - create a directory if it doesn't exist */

static void create_dir (char *dir)
{
    DIR *dp;
    gchar *path;

    path = g_strdup_printf ("%s%s", g_get_home_dir (), dir);
    dp = opendir (path);
    if (!dp) mkdir (path, 0755);
    else closedir (dp);
    g_free (path);
}

/* copy_file - system file copy */

static void copy_file (char *src, char *dst)
{
    gchar *cmd;

    cmd = g_strdup_printf ("cp %s %s", catpath, cbpath);
    system (cmd);
    g_free (cmd);
}


/*----------------------------------------------------------------------------*/
/* libcurl interface                                                          */
/*----------------------------------------------------------------------------*/

static void start_curl_download (char *url, char *file, int progress, void (*end_fn)(int success))
{
    term_fn = end_fn;
    http_handle = curl_easy_init ();
    multi_handle = curl_multi_init ();

    fname = g_strdup (file);
    tmpname = g_strdup_printf ("%s.curl", file);
    outfile = fopen (tmpname, "wb");
    if (!outfile)
    {
        g_free (fname);
        g_free (tmpname);
        end_fn (0);
    }

    curl_easy_setopt (http_handle, CURLOPT_URL, url);
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, outfile);
    if (progress)
    {
        curl_easy_setopt (http_handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt (http_handle, CURLOPT_PROGRESSFUNCTION, progress_func);
    }
    else curl_easy_setopt (http_handle, CURLOPT_NOPROGRESS, 1L);

    curl_multi_add_handle (multi_handle, http_handle);

    curl_timer = g_timeout_add (100, curl_poll, NULL);
}

static gboolean curl_poll (gpointer data)
{
    int still_running;
    if (curl_multi_perform (multi_handle, &still_running) != CURLE_OK)
    {
        finish_curl_download (0);
        return FALSE;
    }
    if (still_running) return TRUE;
    else
    {
        finish_curl_download (1);
        return FALSE;
    }
}

static void finish_curl_download (int success)
{
    if (outfile) fclose (outfile);
    if (success) rename (tmpname, fname);

    curl_multi_remove_handle (multi_handle, http_handle);
    curl_easy_cleanup (http_handle);
    curl_multi_cleanup (multi_handle);
    curl_global_cleanup ();

    g_free (fname);
    g_free (tmpname);

    term_fn (success);
}

static int progress_func (GtkWidget *bar, double t, double d, double ultotal, double ulnow)
{
    double prog = d / t;
    if (prog >= 0.0 && prog <= 1.0) gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), prog);
    return 0;
}

static void abort_curl_download (void)
{
    g_source_remove (curl_timer);
    hide_message ();

    if (outfile) fclose (outfile);
    remove (tmpname);

    curl_multi_remove_handle (multi_handle, http_handle);
    curl_easy_cleanup (http_handle);
    curl_multi_cleanup (multi_handle);
    curl_global_cleanup ();

    g_free (fname);
    g_free (tmpname);

    message (_("Download aborted"), 1, -1);
}


/*----------------------------------------------------------------------------*/
/* Cover art handling                                                         */
/*----------------------------------------------------------------------------*/

/* get_cover - reads in cover from filename to pixbuf and scales */

static GdkPixbuf *get_cover (const char *filename)
{
    GdkPixbuf *pb, *spb;
    int w, h;
    
    pb = gdk_pixbuf_new_from_file (filename, NULL);
    if (!pb) pb = gdk_pixbuf_new_from_file (PACKAGE_DATA_DIR "/nocover.png", NULL);

    w = gdk_pixbuf_get_width (pb);
    h = gdk_pixbuf_get_height (pb);

    spb = gdk_pixbuf_scale_simple (pb, ((w > h) ? COVER_SIZE : COVER_SIZE * w / h), 
        ((w > h) ? COVER_SIZE * h / w : COVER_SIZE), GDK_INTERP_BILINEAR);
    g_object_unref (pb);
    return spb;
}

/* update_cover_entry - uses the cover at lpath to update the cover info for covitem */

static void update_cover_entry (char *lpath, int dl)
{
    int w, h;
    GdkPixbuf *cover = get_cover (lpath);
    if (!dl)
    {
        w = gdk_pixbuf_get_width (cover);
        h = gdk_pixbuf_get_height (cover);
        gdk_pixbuf_composite (grey, cover, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_BILINEAR, 128);
        gdk_pixbuf_composite (cloud, cover, (w - 64) / 2, 32, 64, 64, (w - 64) / 2, 32.0, 0.5, 0.5, GDK_INTERP_BILINEAR, 255);
    }
    gtk_list_store_set (items, &covitem, ITEM_COVER, cover, -1);
    g_object_unref (cover);
}

/* get_cover_for_item - tries to load cover for covitem from local cache; starts download if not */

static gboolean find_cover_for_item (gpointer data)
{
    int dl;
    gchar *path, *lpath;

    if (pdf_dl_req)
    {
        get_pending_pdf ();
        return FALSE;
    }

    gtk_tree_model_get (GTK_TREE_MODEL (items), &covitem, ITEM_COVPATH, &path, ITEM_DOWNLOADED, &dl, -1);
    lpath = get_local_path (path, CACHE_PATH);
    if (access (lpath, F_OK) != -1)
    {
        update_cover_entry (lpath, dl);
        g_free (lpath);
        g_free (path);

        if (gtk_tree_model_iter_next (GTK_TREE_MODEL (items), &covitem)) return TRUE;
        else
        {
            cover_dl = FALSE;
            gtk_widget_queue_draw (items_iv);
            if (pdf_dl_req) get_pending_pdf ();
            return FALSE;
        }
    }
    else
    {
        start_curl_download (path, lpath, 0, image_download_done);
        return FALSE;
    }
}

/* image_download_done - called on completed curl image download */

static void image_download_done (int success)
{
    gchar *lpath, *path;
    int dl;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &covitem, ITEM_COVPATH, &path, ITEM_DOWNLOADED, &dl, -1);
    lpath = get_local_path (path, CACHE_PATH);
    update_cover_entry (lpath, dl);
    g_free (lpath);
    g_free (path);

    if (gtk_tree_model_iter_next (GTK_TREE_MODEL (items), &covitem))
       g_idle_add (find_cover_for_item, NULL);
    else
    {
        cover_dl = FALSE;
        gtk_widget_queue_draw (items_iv);
        if (pdf_dl_req) get_pending_pdf ();
    }
}


/*----------------------------------------------------------------------------*/
/* PDF handling                                                               */
/*----------------------------------------------------------------------------*/

/* item_selected - called when icon is activated to open or download PDF */

static void item_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data)
{
    gchar *lpath, *fpath;

    gtk_tree_model_get_iter (GTK_TREE_MODEL (items), &selitem, path);
    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_PDFPATH, &fpath, -1);

    lpath = get_local_path (fpath, PDF_PATH);

    if (access (lpath, F_OK) == -1)
    {
        message (_("Downloading - please wait..."), 0 , 0);
        if (!cover_dl) start_curl_download (fpath, lpath, 1, pdf_download_done);
        else pdf_dl_req = TRUE;
    }
    else open_pdf (lpath);

    g_free (fpath);
    g_free (lpath);
}

/* open_pdf - launches qpdfview with supplied file */

static void open_pdf (char *path)
{
    if (fork () == 0)
    {
        dup2 (open ("/dev/null", O_WRONLY), STDERR_FILENO); // redirect stderr...
        execl ("/usr/bin/qpdfview", "qpdfview", "--unique", "--quiet", path, NULL);
    }
}

/* pdf_download_done - called on completed curl PDF download */

static void pdf_download_done (int success)
{
    gchar *cpath, *fpath, *lpath, *clpath;

    hide_message ();

    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_COVPATH, &cpath, ITEM_PDFPATH, &fpath, -1);
    lpath = get_local_path (fpath, PDF_PATH);
    clpath = get_local_path (cpath, CACHE_PATH);
    open_pdf (lpath);

    GdkPixbuf *cover = get_cover (clpath);
    gtk_list_store_set (items, &selitem, ITEM_COVER, cover, ITEM_DOWNLOADED, 1, -1);
    gtk_widget_queue_draw (items_iv);

    g_free (lpath);
    g_free (cpath);
    g_free (clpath);
    g_free (fpath);

    if (cover_dl) g_idle_add (find_cover_for_item, NULL);
}

/* get_pending_pdf - start a PDF download that interrupts artwork fetch */

static void get_pending_pdf (void)
{
    gchar *lpath, *fpath;

    pdf_dl_req = FALSE;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_PDFPATH, &fpath, -1);

    lpath = get_local_path (fpath, PDF_PATH);
    start_curl_download (fpath, lpath, 1, pdf_download_done);

    g_free (fpath);
    g_free (lpath);
}


/*----------------------------------------------------------------------------*/
/* Catalogue management                                                       */
/*----------------------------------------------------------------------------*/

/* get_catalogue - start a catalogue download if there is a net connection */

static gboolean get_catalogue (gpointer data)
{
    catpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), CACHE_PATH, "cat.xml");
    cbpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), CACHE_PATH, "catbak.xml");

    if (!net_available ()) load_catalogue (0);
    else
    {
        message (_("Reading list of publications - please wait..."), 0 , 0);
        copy_file (catpath, cbpath);
        start_curl_download (CATALOGUE_URL, catpath, 1, load_catalogue);
    }
    return FALSE;
}

/* get_catalogue - open a catalogue file - either main, backup or fallback */

static void load_catalogue (int success)
{
    hide_message ();

    if (success && read_data_file (catpath)) return;

    message (_("Unable to download updates."), 1, -1);
    copy_file (cbpath, catpath);
    if (read_data_file (catpath)) return;
    read_data_file (PACKAGE_DATA_DIR "/cat.xml");
}

/* get_param - helper function to look for tag in line */

static void get_param (char *linebuf, char *name, char **dest)
{
    char *p1, *p2;
    
    if (p1 = strstr (linebuf, name))
    {
        p1 += strlen (name);
        p2 = strchr (p1, '<');
        *p2 = 0;
        *dest = g_strdup (p1);
    }
}

/* read_data_file - main file parser routine */

static int read_data_file (char *path)
{
    char *linebuf = NULL, *title = NULL, *desc = NULL, *covpath = NULL, *pdfpath = NULL;
    size_t nchars = 0;
    GtkTreeIter entry;
    int category = -1, in_item = FALSE, downloaded, count = 0;

    FILE *fp = fopen (path, "rb");
    if (fp)
    {
        while (getline (&linebuf, &nchars, fp) != -1)
        {
            if (in_item)
            {
                if (strstr (linebuf, "</ITEM>"))
                {
                    // item end flag - add the entry
                    downloaded = FALSE;
                    if (pdfpath)
                    {
                        gchar *lpath = get_local_path (pdfpath, PDF_PATH);
                        if (access (lpath, F_OK) != -1) downloaded = TRUE;
                        g_free (lpath);
                    }
                    gtk_list_store_append (items, &entry);
                    gtk_list_store_set (items, &entry, ITEM_CATEGORY, category, ITEM_TITLE, title,
                        ITEM_DESC, desc, ITEM_PDFPATH, pdfpath, ITEM_COVPATH, covpath,
                        ITEM_COVER, nocover, ITEM_DOWNLOADED, downloaded, -1);
                    in_item = FALSE;
                    g_free (title);
                    g_free (desc);
                    g_free (covpath);
                    g_free (pdfpath);
                    count++;
                }
                get_param (linebuf, "<TITLE>", &title);
                get_param (linebuf, "<DESC>", &desc);
                get_param (linebuf, "<COVER>", &covpath);
                get_param (linebuf, "<PDF>", &pdfpath);
            }   
            else
            {
                if (strstr (linebuf, "<MAGPI>")) category = CAT_MAGPI;
                if (strstr (linebuf, "<BOOKS>")) category = CAT_BOOKS;
                if (strstr (linebuf, "<HACKSPACE>")) category = CAT_HSPACE;
                if (strstr (linebuf, "<WIREFRAME>")) category = CAT_WFRAME;
                if (strstr (linebuf, "<ITEM>")) in_item = TRUE;
            }
        }
        fclose (fp);
    } else return 0;

    if (!count) return count;

    // start loading covers
    if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (items), &covitem))
    {
        cover_dl = TRUE;
        g_idle_add (find_cover_for_item, NULL);
    }
    return count;
}


/*----------------------------------------------------------------------------*/
/* Message box                                                                */
/*----------------------------------------------------------------------------*/

static gboolean ok_clicked (GtkButton *button, gpointer data)
{
    hide_message ();
    return FALSE;
}

static gboolean cancel_clicked (GtkButton *button, gpointer data)
{
    abort_curl_download ();
    return FALSE;
}

static void message (char *msg, int wait, int prog)
{
    if (!msg_dlg)
    {
        GtkBuilder *builder;
        GtkWidget *wid;
        GdkColor col;

        builder = gtk_builder_new ();
        gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_bookshelf.ui", NULL);

        msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "msg");
        gtk_window_set_modal (GTK_WINDOW (msg_dlg), TRUE);
        gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));
        gtk_window_set_position (GTK_WINDOW (msg_dlg), GTK_WIN_POS_CENTER_ON_PARENT);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (msg_dlg), TRUE);
        gtk_window_set_default_size (GTK_WINDOW (msg_dlg), 340, 100);

        wid = (GtkWidget *) gtk_builder_get_object (builder, "msg_eb");
        gdk_color_parse ("#FFFFFF", &col);
        gtk_widget_modify_bg (wid, GTK_STATE_NORMAL, &col);

        msg_msg = (GtkWidget *) gtk_builder_get_object (builder, "msg_lbl");
        msg_pb = (GtkWidget *) gtk_builder_get_object (builder, "msg_pb");
        msg_btn = (GtkWidget *) gtk_builder_get_object (builder, "msg_btn");
        msg_cancel = (GtkWidget *) gtk_builder_get_object (builder, "msg_cancel");

        gtk_label_set_text (GTK_LABEL (msg_msg), msg);

        gtk_widget_show_all (msg_dlg);
        g_object_unref (builder);
    }
    else gtk_label_set_text (GTK_LABEL (msg_msg), msg);

    if (wait)
    {
        gtk_widget_set_visible (msg_pb, FALSE);
        if (wait > 1)
        {
            gtk_button_set_label (GTK_BUTTON (msg_btn), "_Yes");
            g_signal_connect (msg_cancel, "clicked", G_CALLBACK (ok_clicked), NULL);
            gtk_widget_set_visible (msg_cancel, TRUE);
        }
        else
        {
            gtk_button_set_label (GTK_BUTTON (msg_btn), "_OK");
            g_signal_connect (msg_btn, "clicked", G_CALLBACK (ok_clicked), NULL);
            gtk_widget_set_visible (msg_cancel, FALSE);
        }
        gtk_widget_set_visible (msg_btn, TRUE);
    }
    else
    {
        g_signal_connect (msg_cancel, "clicked", G_CALLBACK (cancel_clicked), NULL);
        gtk_widget_set_visible (msg_cancel, TRUE);
        gtk_widget_set_visible (msg_btn, FALSE);
        gtk_widget_set_visible (msg_pb, TRUE);
        if (prog == -1) gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
        else
        {
            float progress = prog / 100.0;
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), progress);
        }
    }
}

static void hide_message (void)
{
    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }
}

/*----------------------------------------------------------------------------*/
/* Handlers for main window user interaction                                  */
/*----------------------------------------------------------------------------*/

static void close_prog (GtkButton* btn, gpointer ptr)
{
    gtk_main_quit ();
}


/*----------------------------------------------------------------------------*/
/* Main window                                                                */
/*----------------------------------------------------------------------------*/

int main (int argc, char *argv[])
{
    GtkBuilder *builder;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    // check that directories exist
    create_dir ("/.cache/");
    create_dir (CACHE_PATH);
    create_dir (PDF_PATH);

    curl_global_init (CURL_GLOBAL_ALL);

    // GTK setup
    gdk_threads_init ();
    gdk_threads_enter ();
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    cloud = get_cover (PACKAGE_DATA_DIR "/cloud.png");
    grey = get_cover (PACKAGE_DATA_DIR "/grey.png");
    nocover = get_cover (PACKAGE_DATA_DIR "/nocover.png");

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_bookshelf.ui", NULL);

    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "main_window");
    items_iv = (GtkWidget *) gtk_builder_get_object (builder, "treeview_prog");
    close_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_ok");

    // create list store
    items = gtk_list_store_new (8, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF); 

    // set up icon view
    gtk_icon_view_set_model (GTK_ICON_VIEW (items_iv), GTK_TREE_MODEL (items));
    gtk_icon_view_set_pixbuf_column (GTK_ICON_VIEW (items_iv), 7);
    gtk_icon_view_set_text_column (GTK_ICON_VIEW (items_iv), 1);
    gtk_icon_view_set_tooltip_column (GTK_ICON_VIEW (items_iv), 2);

    g_signal_connect (items_iv, "item-activated", G_CALLBACK (item_selected), NULL);
    g_signal_connect (close_btn, "clicked", G_CALLBACK (close_prog), NULL);
    g_signal_connect (main_dlg, "delete_event", G_CALLBACK (close_prog), NULL);

    gtk_window_set_default_size (GTK_WINDOW (main_dlg), 640, 400);
    gtk_widget_show_all (main_dlg);

    // update catalogue
    g_idle_add (get_catalogue, NULL);
    cover_dl = FALSE;
    pdf_dl_req = FALSE;

    gtk_main ();

    g_object_unref (builder);
    gtk_widget_destroy (main_dlg);
    gdk_threads_leave ();
    return 0;
}

/* End of file                                                                */
/*----------------------------------------------------------------------------*/
