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

#define MAGPI_URL       "https://store.rpipress.cc/collections/the-magpi-essentials"
#define BOOKS_URL       "https://store.rpipress.cc/collections/latest-bookazines"

#define CATALOGUE_URL   "https://magpi.raspberrypi.com/bookshelf.xml"
#define CACHE_PATH      "/.cache/bookshelf/"
#define PDF_PATH        "/Bookshelf/"

#define USER_AGENT      "Raspberry Pi Bookshelf/0.1"

#define MIN_SPACE       10000000.0

#define CURL_TIMEOUT    1000

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
#define NUM_CATS            2

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

static GtkWidget *main_dlg, *close_btn, *web_btn, *items_nb;
static GtkWidget *item_ivs[NUM_CATS];
static GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_ok, *msg_cancel;

/* Preloaded default pixbufs */

static GdkPixbuf *cloud, *grey, *nocover, *nodl, *newcorn;

/* Data store for icon grid */

GtkListStore *items;

/* Filtered versions of the above */

GtkTreeModel *filtered[NUM_CATS];
GtkTreeModel *sorted;

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
gulong draw_id;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static char *get_local_path (char *path, const char *dir);
static void create_dir (char *dir);
static unsigned long int get_val (char *cmd);
static char *get_string (char *cmd);
static curl_off_t free_space (void);
static void start_curl_download (char *url, char *file, void (*end_fn)(tf_status success));
static gboolean curl_poll (gpointer data);
static void finish_curl_download (void);
static int progress_func (GtkWidget *bar, curl_off_t t, curl_off_t d, curl_off_t ultotal, curl_off_t ulnow);
static GdkPixbuf *get_cover (const char *filename);
static void update_cover_entry (char *lpath, int dl, gboolean new);
static gboolean find_cover_for_item (gpointer data);
static void image_download_done (tf_status success);
static void pdf_selected (void);
static void open_pdf (char *path);
static void pdf_download_done (tf_status success);
static void get_pending_pdf (void);
static void remap_title (char **title);
static void load_catalogue (tf_status success);
static void get_param (char *linebuf, char *name, char *lang, char **dest);
static int read_data_file (char *path);
static gboolean match_category (GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
static gboolean ok_clicked (GtkButton *button, gpointer data);
static gboolean cancel_clicked (GtkButton *button, gpointer data);
static void message (char *msg, gboolean wait);
static void hide_message (void);
static void item_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data);
static void book_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data);
static void handle_menu_open (GtkWidget *widget, gpointer user_data);
static void handle_menu_delete_file (GtkWidget *widget, gpointer user_data);
static void create_cs_menu (GdkEvent *event);
static gboolean icon_clicked (GtkWidget *wid, GdkEventButton *event, gpointer user_data);
static gboolean book_icon_clicked (GtkWidget *wid, GdkEventButton *event, gpointer user_data);
static void refresh_icons (void);
static void web_link (GtkButton* btn, gpointer ptr);
static void close_prog (GtkButton* btn, gpointer ptr);
static gboolean first_draw (GtkWidget *instance);

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

/* get_system_path - creates a string with path to file in package data dir */

static char *get_system_path (char *path)
{
    gchar *basename, *rpath;
    basename = g_path_get_basename (path);
    rpath = strchr (basename, '?');
    if (rpath) *rpath = 0;
    rpath = g_strdup_printf ("%s/%s", PACKAGE_DATA_DIR, basename);
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

/* get_string - call a system command and return the first string in the output */

static char *get_string (char *cmd)
{
    char *line = NULL, *res = NULL;
    size_t len = 0;
    FILE *fp = popen (cmd, "r");

    if (fp == NULL) return NULL;
    if (getline (&line, &len, fp) > 0)
    {
        res = line;
        while (*res)
        {
            if (g_ascii_isspace (*res)) *res = 0;
            res++;
        }
        res = g_strdup (line);
    }
    pclose (fp);
    g_free (line);
    return res;
}

/* free_space - find free space on filesystem */

static curl_off_t free_space (void)
{
    char *cmd;
    unsigned long fs;
    curl_off_t ffs = 1024;
    cmd = g_strdup_printf ("df --output=avail %s%s | tail -n 1", g_get_home_dir (), PDF_PATH);
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
    int still_running;
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
    curl_easy_setopt (http_handle, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt (http_handle, CURLOPT_WRITEDATA, outfile);
    curl_easy_setopt (http_handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt (http_handle, CURLOPT_XFERINFOFUNCTION, progress_func);

    curl_multi_add_handle (multi_handle, http_handle);
    if (curl_multi_perform (multi_handle, &still_running) == CURLM_OK && still_running)
        curl_timer = g_idle_add (curl_poll, NULL);
    else
    {
        downstat = FAILURE;
        finish_curl_download ();
    }
}

static gboolean curl_poll (gpointer data)
{
    int still_running, numfds;
    if (curl_multi_wait (multi_handle, NULL, 0, CURL_TIMEOUT, &numfds) != CURLM_OK)
    {
        downstat = FAILURE;
        finish_curl_download ();
        return FALSE;
    }
    if (curl_multi_perform (multi_handle, &still_running) != CURLM_OK)
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

static int progress_func (GtkWidget *bar, curl_off_t t, curl_off_t d, curl_off_t ultotal, curl_off_t ulnow)
{
    double prog = d;
    prog /= t;

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

    h = gdk_pixbuf_get_height (pb);
    if (h == COVER_SIZE) return pb;
    w = gdk_pixbuf_get_width (pb);

    spb = gdk_pixbuf_scale_simple (pb, ((w > h) ? COVER_SIZE : COVER_SIZE * w / h), 
        ((w > h) ? COVER_SIZE * h / w : COVER_SIZE), GDK_INTERP_BILINEAR);
    g_object_unref (pb);
    return spb;
}

/* update_cover_entry - uses the cover at lpath to update the cover info for covitem */

static void update_cover_entry (char *lpath, int dl, gboolean new)
{
    GdkPixbuf *cover;
    int w, h;

    cover = get_cover (lpath);
    if (!dl)
    {
        w = gdk_pixbuf_get_width (cover);
        h = gdk_pixbuf_get_height (cover);
        gdk_pixbuf_composite (grey, cover, 0, 0, w, h, 0, 0, 1, 1, GDK_INTERP_BILINEAR, 128);
        gdk_pixbuf_composite (cloud, cover, (w - 64) / 2, 32, 64, 64, (w - 64) / 2, 32, 1, 1, GDK_INTERP_BILINEAR, 255);
    }
    if (new)
    {
        w = gdk_pixbuf_get_width (cover);
        h = gdk_pixbuf_get_height (cover);
        gdk_pixbuf_composite (newcorn, cover, w - 32, 0, 32, 32, w - 32, 0, 1, 1, GDK_INTERP_BILINEAR, 255);
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
        update_cover_entry (clpath, dl, FALSE);
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
        update_cover_entry (clpath, dl, TRUE);
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

    plpath = get_system_path (ppath);
    if (access (plpath, F_OK) != -1)
    {
        open_pdf (plpath);
        g_free (plpath);
        g_free (ppath);
        return;
    }
    g_free (plpath);

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

/* open_pdf - launches default viewer with supplied file */

static void open_pdf (char *path)
{
    if (fork () == 0)
    {
        execl ("/usr/bin/xdg-open", "xdg-open", path, NULL);
        exit (0);
    }
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

#define N_REMAPS 32

const char *titlemap[N_REMAPS][2] = {
    {"Build Your Own First-Person Shooter in Unity",     "Build a First-Person Shooter"},
    {"Book of Making Volume 2",                          "HackSpace Book of Making Volume 2"},
    {"Code the Classics – Volume 1",                     "Code the Classics Volume 1"},
    {"Get Started with Arduino",                         "Get Started with Arduino"},
    {"Projects Book 5",                                  "Raspberry Pi Projects Book 5"},
    {"Retro Gaming with Raspberry Pi",                   "Retro Gaming with Raspberry Pi"},
    {"Get Started with Raspberry Pi",                    NULL},
    {"Raspberry Pi Beginner's Guide v3",                 "Raspberry Pi Beginner's Guide"},
    {"Beginner's Guide v2",                              NULL},
    {"Wearable Tech Projects",                           "Wearable Tech Projects"},
    {"C &amp; GUI Programming",                          "C &amp; GUI Programming"},
    {"Command Line 2",                                   "Essentials - Conquer the Command Line"},
    {"Book of Scratch",                                  "Code Club Book of Scratch"},
    {"Beginner's Guide",                                 NULL},
    {"Book of Making Volume 1",                          "HackSpace Book of Making Volume 1"},
    {"Projects Book 4",                                  "Raspberry Pi Projects Book 4"},
    {"Annual 2018",                                      "Raspberry Pi Annual 2018"},
    {"Beginner's Book 1",                                NULL},
    {"Projects Book 3",                                  "Raspberry Pi Projects Book 3"},
    {"AIY Voice Essentials",                             "Essentials - AIY Voice Projects"},
    {"Camera Essentials",                                "Essentials - Camera Module Guide"},
    {"Projects Book 2",                                  "Raspberry Pi Projects Book 2"},
    {"Learn C Essentials",                               "Essentials - Learn to Code with C"},
    {"GPIO Zero Essentials",                             "Essentials - GPIO Zero Electronics"},
    {"Minecraft Essentials",                             "Essentials - Making with Minecraft"},
    {"Scratch Essentials",                               "Essentials - Learn to Code with Scratch"},
    {"Sonic Pi Essentials",                              "Essentials - Code Music with Sonic Pi"},
    {"Sense HAT Essentials",                             "Essentials - Sense HAT Experiments"},
    {"Make Games",                                       "Essentials - Make Games with Python"},
    {"Projects Book",                                    "Raspberry Pi Projects Book 1"},
    {"Command Line",                                     NULL},
    {"MagPi SE 1",                                       NULL}
};

static void remap_title (char **title)
{
    int i;

    for (i = 0; i < N_REMAPS; i++)
    {
        if (!g_strcmp0 (*title, titlemap[i][0]))
        {
            g_free (*title);
            *title = g_strdup (titlemap[i][1]);
            return;
        }
    }
}

/* load_catalogue - open a catalogue file - either main, backup or fallback */

static void load_catalogue (tf_status success)
{
    hide_message ();

    if (success == SUCCESS && read_data_file (catpath))
    {
        gchar *cmd = g_strdup_printf ("cp %s %s", catpath, cbpath);
        system (cmd);
        g_free (cmd);
        return;
    }
    if (success == NOSPACE) message (_("Disk full - unable to download updates"), TRUE);
    else if (success == SUCCESS || success == FAILURE) message (_("Unable to download updates"), TRUE);
    if (read_data_file (cbpath)) return;
    read_data_file (PACKAGE_DATA_DIR "/cat.xml");
}

/* get_param - helper function to look for tag in line */

static void get_param (char *linebuf, char *name, char *lang, char **dest)
{
    char *p1, *p2, *search;

    if (lang) search = g_strdup_printf ("<%s LANG=\"%s\">", name, lang);
    else search = g_strdup_printf ("<%s>", name);
    
    if ((p1 = strstr (linebuf, search)))
    {
        p1 += strlen (search);
        p2 = strchr (p1, '<');
        if (p2)
        {
            *p2 = 0;
            *dest = g_strdup (p1);
        }
    }
    g_free (search);
}

/* read_data_file - main file parser routine */

static int read_data_file (char *path)
{
    char *linebuf = NULL, *lang, *title = NULL, *desc = NULL, *covpath = NULL, *pdfpath = NULL, *tr_title = NULL, *tr_desc = NULL, *tr_covpath = NULL, *tr_pdfpath = NULL;
    size_t nchars = 0;
    GtkTreeIter entry;
    int i, category = -1, in_item = FALSE, downloaded, counts[NUM_CATS], count = 0;

    for (i = 0; i < NUM_CATS; i++) counts[i] = 0;

    FILE *fp = fopen (path, "rb");
    if (fp)
    {
        lang = get_string ("grep LANG= /etc/default/locale | cut -d= -f2 | cut -d_ -f1");

        while (getline (&linebuf, &nchars, fp) != -1)
        {
            if (in_item)
            {
                if (strstr (linebuf, "</ITEM>"))
                {
                    if (category == CAT_BOOKS) remap_title (&title);

                    // item end flag - add the entry
                    if (title && desc && covpath && pdfpath)
                    {
                        if (tr_title)
                        {
                            g_free (title);
                            title = tr_title;
                        }
                        if (tr_desc)
                        {
                            g_free (desc);
                            desc = tr_desc;
                        }
                        if (tr_covpath)
                        {
                            g_free (covpath);
                            covpath = tr_covpath;
                        }
                        if (tr_pdfpath)
                        {
                            g_free (pdfpath);
                            pdfpath = tr_pdfpath;
                        }
                        downloaded = FALSE;
                        if (pdfpath)
                        {
                            char *lpath;
                            lpath = get_system_path (pdfpath);
                            if (access (lpath, F_OK) != -1) downloaded = TRUE;
                            g_free (lpath);
                            lpath = get_local_path (pdfpath, PDF_PATH);
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
                    tr_title = tr_desc = tr_covpath = tr_pdfpath = NULL;
                    counts[category]++;
                    count++;
                }
                get_param (linebuf, "TITLE", NULL, &title);
                get_param (linebuf, "DESC", NULL, &desc);
                get_param (linebuf, "COVER", NULL, &covpath);
                get_param (linebuf, "PDF", NULL, &pdfpath);
                get_param (linebuf, "TITLE", lang, &tr_title);
                get_param (linebuf, "DESC", lang, &tr_desc);
                get_param (linebuf, "COVER", lang, &tr_covpath);
                get_param (linebuf, "PDF", lang, &tr_pdfpath);
            }   
            else
            {
                if (strstr (linebuf, "<MAGPI>")) category = CAT_MAGPI;
                if (strstr (linebuf, "<BOOKS>")) category = CAT_BOOKS;
                if (strstr (linebuf, "<ITEM>")) in_item = TRUE;
            }
        }

        fclose (fp);
        g_free (lang);
    }
    else return 0;

    // hide any tab with no entries
    for (i = 0; i < NUM_CATS; i++)
    {
        if (!counts[i]) gtk_widget_hide (gtk_notebook_get_nth_page (GTK_NOTEBOOK (items_nb), i));
    }
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
    return (cat == (long) data);
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

        builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/rp_bookshelf.ui");

        msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "modal");
        gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));

        msg_msg = (GtkWidget *) gtk_builder_get_object (builder, "modal_msg");
        msg_pb = (GtkWidget *) gtk_builder_get_object (builder, "modal_pb");
        msg_ok = (GtkWidget *) gtk_builder_get_object (builder, "modal_ok");
        msg_cancel = (GtkWidget *) gtk_builder_get_object (builder, "modal_cancel");

        gtk_label_set_text (GTK_LABEL (msg_msg), msg);

        g_object_unref (builder);
    }
    else gtk_label_set_text (GTK_LABEL (msg_msg), msg);

    if (wait)
    {
        g_signal_connect (msg_ok, "clicked", G_CALLBACK (ok_clicked), NULL);
        gtk_widget_hide (msg_cancel);
        gtk_widget_show (msg_ok);
        gtk_widget_hide (msg_pb);
    }
    else
    {
        g_signal_connect (msg_cancel, "clicked", G_CALLBACK (cancel_clicked), NULL);
        gtk_widget_show (msg_cancel);
        gtk_widget_hide (msg_ok);
        gtk_widget_show (msg_pb);
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), 0.0);
    }

    gtk_widget_show (msg_dlg);
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

static void book_selected (GtkIconView *iconview, GtkTreePath *path, gpointer user_data)
{
    GtkTreeIter fitem, sitem;
    gtk_tree_model_get_iter (GTK_TREE_MODEL (sorted), &sitem, path);
    gtk_tree_model_sort_convert_iter_to_child_iter (GTK_TREE_MODEL_SORT (sorted), &fitem, &sitem);
    gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (filtered[CAT_BOOKS]), &selitem, &fitem);

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
    gdk_pixbuf_composite (cloud, cover, (w - 64) / 2, 32, 64, 64, (w - 64) / 2, 32, 1, 1, GDK_INTERP_BILINEAR, 255);

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
    gtk_menu_popup_at_pointer (GTK_MENU (menu), event);
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

static gboolean book_icon_clicked (GtkWidget *wid, GdkEventButton *event, gpointer user_data)
{
    GtkTreeIter fitem, sitem;

    if (event->button == 3)
    {
        GtkTreePath *path = gtk_icon_view_get_path_at_pos (GTK_ICON_VIEW (user_data), event->x, event->y);
        if (path)
        {
            gtk_tree_model_get_iter (sorted, &sitem, path);
            gtk_tree_model_sort_convert_iter_to_child_iter (GTK_TREE_MODEL_SORT (sorted), &fitem, &sitem);
            gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER (filtered[CAT_BOOKS]), &selitem, &fitem);
            create_cs_menu ((GdkEvent *) event);
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

static void web_link (GtkButton* btn, gpointer ptr)
{
    if (fork () == 0)
    {
        switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (items_nb)))
        {
            case 0 :    execl ("/usr/bin/xdg-open", "xdg-open", MAGPI_URL, NULL);
                        exit (0);
            case 1 :    execl ("/usr/bin/xdg-open", "xdg-open", BOOKS_URL, NULL);
                        exit (0);
        }
    }
}

static void close_prog (GtkButton* btn, gpointer ptr)
{
    gtk_main_quit ();
}

/* There's a potential race condition with some WMs whereby trying to draw
 * a modal dialog before the main dialog has realized, the modal dialog`
 * cannot centre on the main dialog - this function ensures that modals will
 * not be drawn until the main dialog has realized. */

static gboolean first_draw (GtkWidget *instance)
{
    catpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), CACHE_PATH, "cat.xml");
    cbpath = g_strdup_printf ("%s%s%s", g_get_home_dir (), CACHE_PATH, "catbak.xml");

    message (_("Reading list of publications - please wait..."), FALSE);
    start_curl_download (CATALOGUE_URL, catpath, load_catalogue);
    g_signal_handler_disconnect (instance, draw_id);
    return FALSE;
}


/*----------------------------------------------------------------------------*/
/* Main window                                                                */
/*----------------------------------------------------------------------------*/

int main (int argc, char *argv[])
{
    GtkBuilder *builder;
    GtkCellLayout *layout;
    GtkCellRenderer *renderer;
    long i;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    // check that directories exist
    create_dir ("/.cache/");
    create_dir (CACHE_PATH);
    create_dir (PDF_PATH);

    curl_global_init (CURL_GLOBAL_ALL);

    // terminate zombies automatically
    signal (SIGCHLD, SIG_IGN);

    // GTK setup
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    cloud = gdk_pixbuf_new_from_file (PACKAGE_DATA_DIR "/cloud.png", NULL);
    grey = gdk_pixbuf_new_from_file (PACKAGE_DATA_DIR "/grey.png", NULL);
    newcorn = gdk_pixbuf_new_from_file (PACKAGE_DATA_DIR "/new.png", NULL);
    nocover = gdk_pixbuf_new_from_file (PACKAGE_DATA_DIR "/nocover.png", NULL);
    nodl = gdk_pixbuf_new_from_file (PACKAGE_DATA_DIR "/nocover.png", NULL);
    i = gdk_pixbuf_get_width (nodl);
    gdk_pixbuf_composite (cloud, nodl, (i - 64) / 2, 32, 64, 64, (i - 64) / 2, 32, 1, 1, GDK_INTERP_BILINEAR, 255);

    // build the UI
    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/rp_bookshelf.ui");

    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "main_window");
    item_ivs[CAT_MAGPI] = (GtkWidget *) gtk_builder_get_object (builder, "iconview_magpi");
    item_ivs[CAT_BOOKS] = (GtkWidget *) gtk_builder_get_object (builder, "iconview_books");
    close_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_ok");
    web_btn = (GtkWidget *) gtk_builder_get_object (builder, "button_web");
    items_nb = (GtkWidget *) gtk_builder_get_object (builder, "notebook1");

    // create list store
    items = gtk_list_store_new (8, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF); 

    // create filtered lists and set up icon views
    for (i = 0; i < NUM_CATS; i++)
    {
        filtered[i] = gtk_tree_model_filter_new (GTK_TREE_MODEL (items), NULL);
        gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (filtered[i]), (GtkTreeModelFilterVisibleFunc) match_category, (gpointer) i, NULL);
        if (i == CAT_BOOKS)
        {
            sorted = gtk_tree_model_sort_new_with_model (GTK_TREE_MODEL (filtered[i]));
            gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (sorted), ITEM_TITLE, GTK_SORT_ASCENDING);
        }

        gtk_icon_view_set_tooltip_column (GTK_ICON_VIEW (item_ivs[i]), ITEM_DESC);
        layout = GTK_CELL_LAYOUT (item_ivs[i]);

        renderer = gtk_cell_renderer_pixbuf_new ();
        gtk_cell_renderer_set_fixed_size (renderer, CELL_WIDTH, -1);
        gtk_cell_layout_pack_start (layout, renderer, FALSE);
        gtk_cell_layout_add_attribute (layout, renderer, "pixbuf", ITEM_COVER);

        renderer = gtk_cell_renderer_text_new ();
        gtk_cell_renderer_set_alignment (renderer, 0.5, 0.0);
        g_object_set (renderer, "wrap-width", CELL_WIDTH, "wrap-mode", PANGO_WRAP_WORD, "alignment", PANGO_ALIGN_CENTER, NULL);
        gtk_cell_layout_pack_start (layout, renderer, FALSE);
        gtk_cell_layout_add_attribute (layout, renderer, "markup", ITEM_TITLE);

        if (i == CAT_BOOKS)
        {
            gtk_icon_view_set_model (GTK_ICON_VIEW (item_ivs[i]), sorted);
            g_signal_connect (item_ivs[i], "item-activated", G_CALLBACK (book_selected), NULL);
            g_signal_connect (item_ivs[i], "button-press-event", G_CALLBACK (book_icon_clicked), item_ivs[i]);
        }
        else
        {
            gtk_icon_view_set_model (GTK_ICON_VIEW (item_ivs[i]), filtered[i]);
            g_signal_connect (item_ivs[i], "item-activated", G_CALLBACK (item_selected), filtered[i]);
            g_signal_connect (item_ivs[i], "button-press-event", G_CALLBACK (icon_clicked), item_ivs[i]);
        }
    }

    g_signal_connect (web_btn, "clicked", G_CALLBACK (web_link), NULL);
    g_signal_connect (close_btn, "clicked", G_CALLBACK (close_prog), NULL);
    g_signal_connect (main_dlg, "delete_event", G_CALLBACK (close_prog), NULL);

    gtk_widget_show_all (main_dlg);
    msg_dlg = NULL;
    msg_pb = NULL;

    // update catalogue
    cover_dl = FALSE;
    pdf_dl_req = FALSE;
    draw_id = g_signal_connect (main_dlg, "draw", G_CALLBACK (first_draw), NULL);

    gtk_main ();

    g_object_unref (builder);
    gtk_widget_destroy (main_dlg);
    return 0;
}

/* End of file                                                                */
/*----------------------------------------------------------------------------*/
