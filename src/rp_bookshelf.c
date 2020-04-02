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

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <X11/Xlib.h>

#include <libintl.h>

#include <curl/curl.h>

#define COVER_SIZE 128

#define CACHE_PATH      "/.cache/bookshelf/"
#define PDF_PATH        "/MagPi/"
#define DOWNLOAD_CMD    "curl -s"

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

/* Controls */

static GtkWidget *main_dlg, *items_iv, *install_btn;
static GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_btn, *msg_cancel, *msg_pbv;
static GtkWidget *err_dlg, *err_msg, *err_btn;

GdkPixbuf *cloud, *grey;

/* Data store for icon grid */

GtkListStore *items;
GtkTreeIter selitem;
GtkTreeIter covitem;

/* Libcurl variables */

CURL *http_handle;
CURLM *multi_handle;
FILE *outfile;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static gboolean update_self (gpointer data);
static void read_data_file (void);
static gboolean ok_clicked (GtkButton *button, gpointer data);
static void error_box (char *msg);
static void message (char *msg, int wait, int prog);
static char *get_shell_string (char *cmd);
static gboolean net_available (void);
static void cancel (GtkButton* btn, gpointer ptr);
char *get_local_path (char *path, const char *dir);
GdkPixbuf *get_cover (const char *filename);
void open_pdf (char *path);
void start_curl_download (char *url, char *file);
gboolean curl_poll (gpointer data);
void finish_curl_download (void);


int progress_func(GtkWidget *bar, double t, double d, double ultotal, double ulnow)
{
  if (d > 0.0) gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), d/t);
  return 0;
}


void start_curl_download (char *url, char *file)
{
    http_handle = curl_easy_init ();
    multi_handle = curl_multi_init ();

    outfile = fopen (file, "wb");

    curl_easy_setopt (http_handle, CURLOPT_URL, url);
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, outfile);
    curl_easy_setopt (http_handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt (http_handle, CURLOPT_PROGRESSFUNCTION, progress_func);

    curl_multi_add_handle (multi_handle, http_handle);

    g_timeout_add (250, curl_poll, NULL);
}

gboolean curl_poll (gpointer data)
{
    int still_running;
    curl_multi_perform (multi_handle, &still_running);
    if (still_running) return TRUE;
    finish_curl_download ();
    return FALSE;
}

void finish_curl_download (void)
{
    gchar *cpath, *fpath, *lpath, *clpath;

    fclose (outfile);

    curl_multi_remove_handle (multi_handle, http_handle);
    curl_easy_cleanup (http_handle);
    curl_multi_cleanup (multi_handle);
    curl_global_cleanup ();

    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    msg_dlg = NULL;

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
}

/*----------------------------------------------------------------------------*/
/* Handlers for asynchronous initialisation sequence at start                 */
/*----------------------------------------------------------------------------*/

static gboolean update_self (gpointer data)
{

    message (_("Reading list of publications - please wait..."), 0 , -1);
    
    // check the cache folder exists
    // set path for user
    
    // get the XML file - assume already in cache for now
    
    //system ("curl https://magpi.raspberrypi.org/ > /home/pi/.cache/bookshelf/front.html");
    //system ("curl https://magpi.raspberrypi.org/issues > /home/pi/.cache/bookshelf/issues.html");
    //system ("curl https://magpi.raspberrypi.org/books > /home/pi/.cache/bookshelf/books.html");
    
    read_data_file ();

    return FALSE;
}


char *get_local_path (char *path, const char *dir)
{
    gchar *basename = g_path_get_basename (path);
    gchar *rpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), dir, basename);
    g_free (basename);
    return rpath;
}


GdkPixbuf *get_cover (const char *filename)
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


void *update_covers (void *param)
{
    GtkTreeIter item;
    gboolean valid;
    gchar *path, *lpath, *cmd;
    GdkPixbuf *cover, *cloud, *grey;
    int dl, w, h;
    cloud = get_cover (PACKAGE_DATA_DIR "/cloud.png");
    grey = get_cover (PACKAGE_DATA_DIR "/grey.png");

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (items), &item);
    while (valid)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (items), &item, ITEM_COVPATH, &path, ITEM_DOWNLOADED, &dl, -1);
        lpath = get_local_path (path, CACHE_PATH);
        if (access (lpath, F_OK) == -1)
        {
            cmd = g_strdup_printf ("%s %s > %s", DOWNLOAD_CMD, path, lpath);
            system (cmd);
            g_free (cmd);
        }
        cover = get_cover (lpath);
        if (!dl)
        {
            w = gdk_pixbuf_get_width (cover);
            h = gdk_pixbuf_get_height (cover);
            gdk_pixbuf_composite (grey, cover, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_BILINEAR, 128);
            gdk_pixbuf_composite (cloud, cover, (w - 64) / 2, 32, 64, 64, (w - 64) / 2, 32.0, 0.5, 0.5, GDK_INTERP_BILINEAR, 255);
        }
        gtk_list_store_set (items, &item, ITEM_COVER, cover, -1);
        g_object_unref (cover);
        g_free (lpath);
        g_free (path);

        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (items), &item);
    }
    gtk_widget_queue_draw (items_iv);
    return NULL;
}

gboolean get_cover_for_item (gpointer data)
{
    int dl, w, h;
    gchar *path, *lpath;
    GdkPixbuf *cover;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &covitem, ITEM_COVPATH, &path, ITEM_DOWNLOADED, &dl, -1);
    lpath = get_local_path (path, CACHE_PATH);
    if (access (lpath, F_OK) != -1)
    {
        cover = get_cover (lpath);
        if (!dl)
        {
            w = gdk_pixbuf_get_width (cover);
            h = gdk_pixbuf_get_height (cover);
            gdk_pixbuf_composite (grey, cover, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_BILINEAR, 128);
            gdk_pixbuf_composite (cloud, cover, (w - 64) / 2, 32, 64, 64, (w - 64) / 2, 32.0, 0.5, 0.5, GDK_INTERP_BILINEAR, 255);
        }
        gtk_list_store_set (items, &covitem, ITEM_COVER, cover, -1);
        g_object_unref (cover);
        g_free (lpath);
        g_free (path);

        if (gtk_tree_model_iter_next (GTK_TREE_MODEL (items), &covitem)) return TRUE;
        else
        {
            gtk_widget_queue_draw (items_iv);
            return FALSE;
        }
    }
}

void start_cover_load (void)
{
    if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (items), &covitem))
        g_idle_add (get_cover_for_item, NULL);
}


void open_pdf (char *path)
{
    if (fork () == 0)
    {
        dup2 (open ("/dev/null", O_WRONLY), STDERR_FILENO); // redirect stderr...
        execl ("/usr/bin/qpdfview", "qpdfview", "--unique", "--quiet", path, NULL);
    }
}


void item_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data)
{
    gchar *lpath, *fpath;
    pthread_t download_thread;

    gtk_tree_model_get_iter (GTK_TREE_MODEL (items), &selitem, path);
    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_PDFPATH, &fpath, -1);

    lpath = get_local_path (fpath, PDF_PATH);

    if (access (lpath, F_OK) == -1)
    {
        message (_("Downloading - please wait..."), 0 , 0);
        start_curl_download (fpath, lpath);
    }
    else open_pdf (lpath);

    g_free (fpath);
    g_free (lpath);
}


void get_param (char *linebuf, char *name, char **dest)
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


static void read_data_file (void)
{
    char *linebuf = NULL, *title = NULL, *desc = NULL, *covpath = NULL, *pdfpath = NULL;
    size_t nchars = 0;
    GtkTreeIter entry;
    int category = -1, in_item = FALSE, downloaded;
    pthread_t update_thread;

    GdkPixbuf *pb = get_cover (PACKAGE_DATA_DIR "/nocover.png");

    // fix this path!
    FILE *fp = fopen ("/home/pi/.cache/bookshelf/cat.xml", "rb");
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
                        ITEM_COVER, pb, ITEM_DOWNLOADED, downloaded, -1);
                    in_item = FALSE;
                    g_free (title);
                    g_free (desc);
                    g_free (covpath);
                    g_free (pdfpath);
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
    }
    // else handle no file error here...
    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    msg_dlg = NULL;
    gtk_widget_set_sensitive (install_btn, TRUE);

    //pthread_create (&update_thread, NULL, update_covers, NULL);
    start_cover_load ();
}






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

/*----------------------------------------------------------------------------*/
/* Progress / error box                                                       */
/*----------------------------------------------------------------------------*/

static gboolean ok_clicked (GtkButton *button, gpointer data)
{
    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }
    if (err_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (err_dlg));
        err_dlg = NULL;
    }
    gtk_main_quit ();
    return FALSE;
}

static void error_box (char *msg)
{
    if (msg_dlg)
    {
        // clear any existing message box
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }

    if (!err_dlg)
    {
        GtkBuilder *builder;
        GtkWidget *wid;
        GdkColor col;

        builder = gtk_builder_new ();
        gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_prefapps.ui", NULL);

        err_dlg = (GtkWidget *) gtk_builder_get_object (builder, "error");
        gtk_window_set_modal (GTK_WINDOW (err_dlg), TRUE);
        gtk_window_set_transient_for (GTK_WINDOW (err_dlg), GTK_WINDOW (main_dlg));
        gtk_window_set_position (GTK_WINDOW (err_dlg), GTK_WIN_POS_CENTER_ON_PARENT);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (err_dlg), TRUE);
        gtk_window_set_default_size (GTK_WINDOW (err_dlg), 400, 200);

        gdk_color_parse ("#FFFFFF", &col);
        wid = (GtkWidget *) gtk_builder_get_object (builder, "err_eb");
        gtk_widget_modify_bg (wid, GTK_STATE_NORMAL, &col);
        wid = (GtkWidget *) gtk_builder_get_object (builder, "err_sw");
        gtk_widget_modify_bg (wid, GTK_STATE_NORMAL, &col);
        wid = (GtkWidget *) gtk_builder_get_object (builder, "err_vp");
        gtk_widget_modify_bg (wid, GTK_STATE_NORMAL, &col);

        err_msg = (GtkWidget *) gtk_builder_get_object (builder, "err_lbl");
        err_btn = (GtkWidget *) gtk_builder_get_object (builder, "err_btn");

        gtk_label_set_text (GTK_LABEL (err_msg), msg);

        gtk_button_set_label (GTK_BUTTON (err_btn), "_OK");
        g_signal_connect (err_btn, "clicked", G_CALLBACK (ok_clicked), NULL);

        gtk_widget_show_all (err_dlg);
        g_object_unref (builder);
    }
    else gtk_label_set_text (GTK_LABEL (err_msg), msg);
}


static void message (char *msg, int wait, int prog)
{
    if (!msg_dlg)
    {
        GtkBuilder *builder;
        GtkWidget *wid;
        GdkColor col;

        builder = gtk_builder_new ();
        gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_prefapps.ui", NULL);

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
        gtk_widget_set_visible (msg_cancel, FALSE);
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

/*----------------------------------------------------------------------------*/
/* Handlers for main window user interaction                                  */
/*----------------------------------------------------------------------------*/

static void cancel (GtkButton* btn, gpointer ptr)
{
    gtk_main_quit ();
}


/*----------------------------------------------------------------------------*/
/* Main window                                                                */
/*----------------------------------------------------------------------------*/

int main (int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkCellRenderer *crp, *crt, *crb;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    curl_global_init (CURL_GLOBAL_ALL);

    // GTK setup
    gdk_threads_init ();
    gdk_threads_enter ();
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    cloud = get_cover (PACKAGE_DATA_DIR "/cloud.png");
    grey = get_cover (PACKAGE_DATA_DIR "/grey.png");

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_bookshelf.ui", NULL);

    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "main_window");
    items_iv = (GtkWidget *) gtk_builder_get_object (builder, "treeview_prog");
    install_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_ok");

    // create list store
    items = gtk_list_store_new (8, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF); 

    // set up icon view
    gtk_icon_view_set_model (GTK_ICON_VIEW (items_iv), GTK_TREE_MODEL (items));
    gtk_icon_view_set_pixbuf_column (GTK_ICON_VIEW (items_iv), 7);
    gtk_icon_view_set_text_column (GTK_ICON_VIEW (items_iv), 1);
    gtk_icon_view_set_tooltip_column (GTK_ICON_VIEW (items_iv), 2);
    g_signal_connect (items_iv, "item-activated", G_CALLBACK (item_selected), NULL);

    g_signal_connect (install_btn, "clicked", G_CALLBACK (cancel), NULL);
    g_signal_connect (main_dlg, "delete_event", G_CALLBACK (cancel), NULL);

    gtk_widget_set_sensitive (install_btn, FALSE);

    gtk_window_set_default_size (GTK_WINDOW (main_dlg), 640, 400);
    gtk_widget_show_all (main_dlg);

    // update application, load the data file and check with backend
    if (net_available ()) g_idle_add (update_self, NULL);
    else error_box (_("No network connection - bookshelf cannot be updated"));

    gtk_main ();

    g_object_unref (builder);
    gtk_widget_destroy (main_dlg);
    gdk_threads_leave ();
    return 0;
}

/* End of file                                                                */
/*----------------------------------------------------------------------------*/
