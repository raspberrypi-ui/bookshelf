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

/*----------------------------------------------------------------------------*/
/* Macros                                                                     */
/*----------------------------------------------------------------------------*/

#define COVER_SIZE      128
#define CELL_WIDTH      150

#define CATALOGUE_URL   "https://magpi.raspberrypi.org/bookshelf.xml"
#define CACHE_PATH      "/.cache/bookshelf/"
#define PDF_PATH        "/MagPi/"

#define MIN_SPACE       10000000.0

/* Columns in item list store */

#define ITEM_CATEGORY       0
#define ITEM_TITLE          1
#define ITEM_DESC           2
#define ITEM_PDFPATH        3
#define ITEM_COVPATH        4
#define ITEM_FILENAME       5
#define ITEM_DOWNLOADED     6
#define ITEM_COVER          7

/* Publication category */

#define CAT_MAGPI           0
#define CAT_BOOKS           1
#define CAT_HSPACE          2
#define CAT_WFRAME          3
#define NUM_CATS            4

/* Termination function arguments */

typedef enum {
    NOSPACE = -2,
    CANCELLED = -1,
    FAILURE = 0,
    SUCCESS = 1
} tf_status;

/*----------------------------------------------------------------------------*/
/* Globals                                                                    */
/*----------------------------------------------------------------------------*/

/* Controls */

static GtkWidget *main_dlg, *close_btn;
static GtkWidget *item_ivs[NUM_CATS];
static GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_ok, *msg_cancel;

/* Preloaded default pixbufs */

static GdkPixbuf *cloud, *grey, *nocover, *nodl;

/* Data store for icon grid */

GtkListStore *items;

/* Filtered versions of the above */

GtkTreeModel *filtered[NUM_CATS];

/* Download items */

GtkTreeIter selitem, covitem;

/* Catalogue file path */

char *catpath, *cbpath;

/* Libcurl variables */

CURL *http_handle;
CURLM *multi_handle;
FILE *outfile;
guint curl_timer;
void (*term_fn) (tf_status success);
char *fname, *tmpname;
gboolean cancelled;
tf_status downstat;

/* Flags to manage simultaneous download of art and PDF */

gboolean cover_dl, pdf_dl_req;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static char *get_local_path (char *path, const char *dir);
static void create_dir (char *dir);
static void copy_file (char *src, char *dst);
static unsigned long int get_val (char *cmd);
static double free_space (void);
static void start_curl_download (char *url, char *file, void (*end_fn)(tf_status success));
static gboolean curl_poll (gpointer data);
static void finish_curl_download (void);
static int progress_func (GtkWidget *bar, double t, double d, double ultotal, double ulnow);
static GdkPixbuf *get_cover (const char *filename);
static void update_cover_entry (char *lpath, int dl);
static gboolean find_cover_for_item (gpointer data);
static void image_download_done (tf_status success);
static void pdf_selected (void);
static void open_pdf (char *path);
static gboolean reset_cursor (gpointer data);
static void pdf_download_done (tf_status success);
static void get_pending_pdf (void);
static gboolean get_catalogue (gpointer data);
static void load_catalogue (tf_status success);
static void get_param (char *linebuf, char *name, char **dest);
static int read_data_file (char *path);
static gboolean match_category (GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
static gboolean ok_clicked (GtkButton *button, gpointer data);
static gboolean cancel_clicked (GtkButton *button, gpointer data);
static void message (char *msg, gboolean wait);
static void hide_message (void);
static void item_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data);
static void handle_menu_open (GtkWidget *widget, gpointer user_data);
static void handle_menu_delete_file (GtkWidget *widget, gpointer user_data);
static void create_cs_menu (GdkEvent *event);
static gboolean icon_clicked (GtkWidget *wid, GdkEventButton *event, gpointer user_data);
static void refresh_icons (void);
static void close_prog (GtkButton* btn, gpointer ptr);

/*----------------------------------------------------------------------------*/
/* Helpers                                                                    */
/*----------------------------------------------------------------------------*/

/* get_local_path - creates a string with path to file in user's home dir */

static char *get_local_path (char *path, const char *dir)
{
    gchar *basename, *rpath;
    basename = g_path_get_basename (path);
    rpath = strchr (basename, '?');
    if (rpath) *rpath = 0;
    rpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), dir, basename);
    g_free (basename);
    return rpath;
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

/* get_val - call a system command and convert the first string returned to a number */

static unsigned long int get_val (char *cmd)
{
    FILE *fp;
    char buf[64];
    unsigned long res;

    fp = popen (cmd, "r");
    if (fp == NULL) return 0;
    if (fgets (buf, sizeof (buf) - 1, fp) == NULL)
    {
        pclose (fp);
        return 0;
    }
    else
    {
        pclose (fp);
        if (sscanf (buf, "%ld", &res) != 1) return 0;
        return res;
    }
}

/* free_space - find free space on filesystem */

static double free_space (void)
{
    char *cmd;
    unsigned long fs;
    double ffs = 1024.0;
    cmd = g_strdup_printf ("df --output=avail %s%s | grep -v Avail", g_get_home_dir (), PDF_PATH);
    fs = get_val (cmd);
    g_free (cmd);
    ffs *= fs;
    return ffs;
}


/*----------------------------------------------------------------------------*/
/* libcurl interface                                                          */
/*----------------------------------------------------------------------------*/

static void start_curl_download (char *url, char *file, void (*end_fn)(tf_status success))
{
    cancelled = FALSE;
    downstat = FAILURE;
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
        term_fn (FAILURE);
    }

    curl_easy_setopt (http_handle, CURLOPT_URL, url);
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, outfile);
    curl_easy_setopt (http_handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt (http_handle, CURLOPT_PROGRESSFUNCTION, progress_func);

    curl_multi_add_handle (multi_handle, http_handle);

    curl_timer = g_timeout_add (100, curl_poll, NULL);
}

static gboolean curl_poll (gpointer data)
{
    int still_running;
    if (curl_multi_perform (multi_handle, &still_running) != CURLE_OK)
    {
        downstat = FAILURE;
        finish_curl_download ();
        return FALSE;
    }
    if (still_running) return TRUE;
    else
    {
        finish_curl_download ();
        return FALSE;
    }
}

static void finish_curl_download (void)
{
    if (outfile) fclose (outfile);
    if (downstat == SUCCESS) rename (tmpname, fname);
    else remove (tmpname);

    curl_multi_remove_handle (multi_handle, http_handle);
    curl_easy_cleanup (http_handle);
    curl_multi_cleanup (multi_handle);
    curl_global_cleanup ();

    g_free (fname);
    g_free (tmpname);

    term_fn (downstat);
}

static int progress_func (GtkWidget *bar, double t, double d, double ultotal, double ulnow)
{
    double prog = d / t;
    if (cancelled)
    {
        downstat = CANCELLED;
        return 1;
    }
    if (prog >= 0.0 && prog <= 1.0)
    {
        if (downstat == FAILURE)
        {
            if (t + MIN_SPACE >= free_space ())
            {
                downstat = NOSPACE;
                return 1;
            }
            downstat = SUCCESS;
        }
        if (msg_pb)
        {
            if (pdf_dl_req) gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
            else gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), prog);
        }
    }
    return 0;
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
    GdkPixbuf *cover;
    int w, h;

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
}

/* get_cover_for_item - tries to load cover for covitem from local cache; starts download if not */

static gboolean find_cover_for_item (gpointer data)
{
    int dl;
    gchar *cpath, *clpath;

    if (pdf_dl_req)
    {
        get_pending_pdf ();
        return FALSE;
    }

    gtk_tree_model_get (GTK_TREE_MODEL (items), &covitem, ITEM_COVPATH, &cpath, ITEM_DOWNLOADED, &dl, -1);
    clpath = get_local_path (cpath, CACHE_PATH);
    if (access (clpath, F_OK) != -1)
    {
        update_cover_entry (clpath, dl);
        g_free (clpath);
        g_free (cpath);

        if (gtk_tree_model_iter_next (GTK_TREE_MODEL (items), &covitem)) return TRUE;
        else
        {
            cover_dl = FALSE;
            refresh_icons ();
            if (pdf_dl_req) get_pending_pdf ();
            return FALSE;
        }
    }
    else
    {
        start_curl_download (cpath, clpath, image_download_done);
        g_free (clpath);
        g_free (cpath);
        return FALSE;
    }
}

/* image_download_done - called on completed curl image download */

static void image_download_done (tf_status success)
{
    gchar *cpath, *clpath;
    int dl;

    if (success == SUCCESS)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (items), &covitem, ITEM_COVPATH, &cpath, ITEM_DOWNLOADED, &dl, -1);
        clpath = get_local_path (cpath, CACHE_PATH);
        update_cover_entry (clpath, dl);
        g_free (clpath);
        g_free (cpath);
    }

    if (gtk_tree_model_iter_next (GTK_TREE_MODEL (items), &covitem))
       g_idle_add (find_cover_for_item, NULL);
    else
    {
        cover_dl = FALSE;
        refresh_icons ();
        if (pdf_dl_req) get_pending_pdf ();
    }
}


/*----------------------------------------------------------------------------*/
/* PDF handling                                                               */
/*----------------------------------------------------------------------------*/

/* pdf_selected - called when icon is activated to open or download PDF */

static void pdf_selected (void)
{
    gchar *ppath, *plpath;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_PDFPATH, &ppath, -1);
    plpath = get_local_path (ppath, PDF_PATH);

    if (access (plpath, F_OK) == -1)
    {
        message (_("Downloading - please wait..."), FALSE);
        if (!cover_dl) start_curl_download (ppath, plpath, pdf_download_done);
        else pdf_dl_req = TRUE;
    }
    else open_pdf (plpath);

    g_free (plpath);
    g_free (ppath);
}

/* open_pdf - launches qpdfview with supplied file */

static void open_pdf (char *path)
{
    GdkCursor *busy = gdk_cursor_new_from_name (gdk_display_get_default (), "left_ptr_watch");
    gdk_window_set_cursor (gtk_widget_get_window (main_dlg), busy);
    g_timeout_add  (5000, reset_cursor, NULL);
    if (fork () == 0)
    {
        dup2 (open ("/dev/null", O_WRONLY), STDERR_FILENO); // redirect stderr...
        execl ("/usr/bin/qpdfview", "qpdfview", "--unique", "--quiet", path, NULL);
    }
}

/* reset_cursor - the PDF viewer doesn't show busy, so bodge it... */

static gboolean reset_cursor (gpointer data)
{
    gdk_window_set_cursor (gtk_widget_get_window (main_dlg), NULL);
    return FALSE;
}

/* pdf_download_done - called on completed curl PDF download */

static void pdf_download_done (tf_status success)
{
    gchar *cpath, *ppath, *clpath, *plpath;

    hide_message ();
    if (success == SUCCESS)
    {
        gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_COVPATH, &cpath, ITEM_PDFPATH, &ppath, -1);
        plpath = get_local_path (ppath, PDF_PATH);
        clpath = get_local_path (cpath, CACHE_PATH);
        open_pdf (plpath);

        GdkPixbuf *cover = get_cover (clpath);
        gtk_list_store_set (items, &selitem, ITEM_COVER, cover, ITEM_DOWNLOADED, 1, -1);
        refresh_icons ();

        g_free (plpath);
        g_free (clpath);
        g_free (ppath);
        g_free (cpath);
        g_object_unref (cover);
    }
    else if (success == FAILURE) message (_("Unable to download file"), TRUE);
    else if (success == NOSPACE) message (_("Disk full - unable to download file"), TRUE);

    if (cover_dl) g_idle_add (find_cover_for_item, NULL);
}

/* get_pending_pdf - start a PDF download that interrupts artwork fetch */

static void get_pending_pdf (void)
{
    gchar *ppath, *plpath;

    pdf_dl_req = FALSE;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_PDFPATH, &ppath, -1);

    plpath = get_local_path (ppath, PDF_PATH);
    start_curl_download (ppath, plpath, pdf_download_done);

    g_free (plpath);
    g_free (ppath);
}


/*----------------------------------------------------------------------------*/
/* Catalogue management                                                       */
/*----------------------------------------------------------------------------*/

/* get_catalogue - start a catalogue download */

static gboolean get_catalogue (gpointer data)
{
    catpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), CACHE_PATH, "cat.xml");
    cbpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), CACHE_PATH, "catbak.xml");

    message (_("Reading list of publications - please wait..."), FALSE);
    start_curl_download (CATALOGUE_URL, catpath, load_catalogue);
    return FALSE;
}

/* get_catalogue - open a catalogue file - either main, backup or fallback */

static void load_catalogue (tf_status success)
{
    hide_message ();

    if (success == SUCCESS && read_data_file (catpath))
    {
        copy_file (catpath, cbpath);
        return;
    }
    if (success == NOSPACE) message (_("Disk full - unable to download updates"), TRUE);
    else if (success == SUCCESS || success == FAILURE) message (_("Unable to download updates"), TRUE);
    if (read_data_file (cbpath)) return;
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
        if (p2)
        {
            *p2 = 0;
            *dest = g_strdup (p1);
        }
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
                    if (title && desc && covpath && pdfpath)
                    {
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
                            ITEM_COVER, downloaded ? nocover : nodl, ITEM_DOWNLOADED, downloaded, -1);
                    }
                    in_item = FALSE;
                    g_free (title);
                    g_free (desc);
                    g_free (covpath);
                    g_free (pdfpath);
                    title = desc = covpath = pdfpath = NULL;
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

/* match_category - filter function for tab pages */

static gboolean match_category (GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    int cat;

    gtk_tree_model_get (model, iter, ITEM_CATEGORY, &cat, -1);
    return (cat == (int) data);
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
    cancelled = TRUE;
    return FALSE;
}

static void message (char *msg, gboolean wait)
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
        msg_ok = (GtkWidget *) gtk_builder_get_object (builder, "msg_btn");
        msg_cancel = (GtkWidget *) gtk_builder_get_object (builder, "msg_cancel");

        gtk_label_set_text (GTK_LABEL (msg_msg), msg);

        gtk_widget_show_all (msg_dlg);
        g_object_unref (builder);
    }
    else gtk_label_set_text (GTK_LABEL (msg_msg), msg);

    if (wait)
    {
        g_signal_connect (msg_ok, "clicked", G_CALLBACK (ok_clicked), NULL);
        gtk_widget_set_visible (msg_cancel, FALSE);
        gtk_widget_set_visible (msg_ok, TRUE);
        gtk_widget_set_visible (msg_pb, FALSE);
    }
    else
    {
        g_signal_connect (msg_cancel, "clicked", G_CALLBACK (cancel_clicked), NULL);
        gtk_widget_set_visible (msg_cancel, TRUE);
        gtk_widget_set_visible (msg_ok, FALSE);
        gtk_widget_set_visible (msg_pb, TRUE);
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), 0.0);
    }
}

static void hide_message (void)
{
    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
        msg_pb = NULL;
    }
}

/*----------------------------------------------------------------------------*/
/* Handlers for main window user interaction                                  */
/*----------------------------------------------------------------------------*/

static void item_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data)
{
    GtkTreeIter fitem;
    gtk_tree_model_get_iter (GTK_TREE_MODEL (user_data), &fitem, path);
    gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (user_data), &selitem, &fitem);

    pdf_selected ();
}

static void handle_menu_open (GtkWidget *widget, gpointer user_data)
{
    pdf_selected ();
}

static void handle_menu_delete_file (GtkWidget *widget, gpointer user_data)
{
    gchar *cpath, *ppath, *clpath, *plpath;
    GdkPixbuf *cover;
    int w, h;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_COVPATH, &cpath, ITEM_PDFPATH, &ppath, -1);
    plpath = get_local_path (ppath, PDF_PATH);
    clpath = get_local_path (cpath, CACHE_PATH);

    remove (plpath);

    cover = get_cover (clpath);
    w = gdk_pixbuf_get_width (cover);
    h = gdk_pixbuf_get_height (cover);
    gdk_pixbuf_composite (grey, cover, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_BILINEAR, 128);
    gdk_pixbuf_composite (cloud, cover, (w - 64) / 2, 32, 64, 64, (w - 64) / 2, 32.0, 0.5, 0.5, GDK_INTERP_BILINEAR, 255);

    gtk_list_store_set (items, &selitem, ITEM_COVER, cover, ITEM_DOWNLOADED, 0, -1);
    refresh_icons ();

    g_free (plpath);
    g_free (clpath);
    g_free (ppath);
    g_free (cpath);
    g_object_unref (cover);
}

static void create_cs_menu (GdkEvent *event)
{
    gchar *ppath, *plpath;
    GtkWidget *menu, *mi;
    int dl;

    gtk_tree_model_get (GTK_TREE_MODEL (items), &selitem, ITEM_PDFPATH, &ppath, ITEM_DOWNLOADED, &dl, -1);
    plpath = get_local_path (ppath, PDF_PATH);

    menu = gtk_menu_new ();

    if (access (plpath, F_OK) == -1)
    {
        mi = gtk_menu_item_new_with_label (_("Download & open item"));
        g_signal_connect (mi, "activate", G_CALLBACK (handle_menu_open), NULL);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    }
    else
    {
        mi = gtk_menu_item_new_with_label (_("Open item"));
        g_signal_connect (mi, "activate", G_CALLBACK (handle_menu_open), NULL);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);

        mi = gtk_menu_item_new_with_label (_("Delete item"));
        g_signal_connect (mi, "activate", G_CALLBACK (handle_menu_delete_file), plpath);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
    }

    g_free (plpath);
    g_free (ppath);

    gtk_widget_show_all (menu);
    gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL, 3, gdk_event_get_time (event));
}

static gboolean icon_clicked (GtkWidget *wid, GdkEventButton *event, gpointer user_data)
{
    GtkTreeIter fitem;

    if (event->button == 3)
    {
        GtkTreePath *path = gtk_icon_view_get_path_at_pos (GTK_ICON_VIEW (user_data), event->x, event->y);
        if (path)
        {
            GtkTreeModel *model = gtk_icon_view_get_model (GTK_ICON_VIEW (user_data));
            if (model)
            {
                gtk_tree_model_get_iter (model, &fitem, path);
                gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (model), &selitem, &fitem);
                create_cs_menu ((GdkEvent *) event);
            }
        }
        return TRUE;
    }
    return FALSE;
}

static void refresh_icons (void)
{
    int i;
    for (i = 0; i < NUM_CATS; i++) gtk_widget_queue_draw (item_ivs[i]);
}

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
    int i;

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
    nodl = get_cover (PACKAGE_DATA_DIR "/nocover.png");
    i = gdk_pixbuf_get_width (nodl);
    gdk_pixbuf_composite (cloud, nodl, (i - 64) / 2, 32, 64, 64, (i - 64) / 2, 32.0, 0.5, 0.5, GDK_INTERP_BILINEAR, 255);

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/rp_bookshelf.ui", NULL);

    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "main_window");
    item_ivs[CAT_MAGPI] = (GtkWidget *) gtk_builder_get_object (builder, "iconview_magpi");
    item_ivs[CAT_BOOKS] = (GtkWidget *) gtk_builder_get_object (builder, "iconview_books");
    item_ivs[CAT_HSPACE] = (GtkWidget *) gtk_builder_get_object (builder, "iconview_hack");
    item_ivs[CAT_WFRAME] = (GtkWidget *) gtk_builder_get_object (builder, "iconview_wire");
    close_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_ok");

    // create list store
    items = gtk_list_store_new (8, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF); 

    // create filtered lists and set up icon views
    GtkCellLayout *layout;
    GtkCellRenderer *renderer;
    for (i = 0; i < NUM_CATS; i++)
    {
        filtered[i] = gtk_tree_model_filter_new (GTK_TREE_MODEL (items), NULL);
        gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (filtered[i]), (GtkTreeModelFilterVisibleFunc) match_category, (gpointer) i, NULL);
        gtk_icon_view_set_tooltip_column (GTK_ICON_VIEW (item_ivs[i]), ITEM_DESC);
        layout = GTK_CELL_LAYOUT (item_ivs[i]);

        renderer = gtk_cell_renderer_pixbuf_new ();
        gtk_cell_renderer_set_fixed_size (renderer, CELL_WIDTH, -1);
        gtk_cell_layout_pack_start (layout, renderer, FALSE);
        gtk_cell_layout_add_attribute (layout, renderer, "pixbuf", ITEM_COVER);

        renderer = gtk_cell_renderer_text_new ();
        g_object_set (renderer, "wrap-width", CELL_WIDTH, "wrap-mode", PANGO_WRAP_WORD, "alignment", PANGO_ALIGN_CENTER, NULL);
        gtk_cell_layout_pack_start (layout, renderer, FALSE);
        gtk_cell_layout_add_attribute (layout, renderer, "markup", ITEM_TITLE);

        gtk_icon_view_set_model (GTK_ICON_VIEW (item_ivs[i]), filtered[i]);
        g_signal_connect (item_ivs[i], "item-activated", G_CALLBACK (item_selected), filtered[i]);
        g_signal_connect (item_ivs[i], "button-press-event", G_CALLBACK (icon_clicked), item_ivs[i]);
    }

    g_signal_connect (close_btn, "clicked", G_CALLBACK (close_prog), NULL);
    g_signal_connect (main_dlg, "delete_event", G_CALLBACK (close_prog), NULL);

    gtk_window_set_default_size (GTK_WINDOW (main_dlg), 1000, 600);
    gtk_widget_show_all (main_dlg);
    msg_dlg = NULL;
    msg_pb = NULL;

    // update catalogue
    cover_dl = FALSE;
    pdf_dl_req = FALSE;
    g_idle_add (get_catalogue, NULL);

    gtk_main ();

    g_object_unref (builder);
    gtk_widget_destroy (main_dlg);
    gdk_threads_leave ();
    return 0;
}

/* End of file                                                                */
/*----------------------------------------------------------------------------*/
