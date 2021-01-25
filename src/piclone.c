/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
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

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

/*---------------------------------------------------------------------------*/
/* Variable and macro definitions */
/*---------------------------------------------------------------------------*/

/* struct to store partition data */

#define MAXPART 9

typedef struct
{
    int pnum;
    long start;
    long end;
    char ptype[10];
    char ftype[20];
    char flags[10];
} partition_t;

partition_t parts[MAXPART];

/* global volume monitor object */
GVolumeMonitor *monitor;

/* control widget globals */
static GtkWidget *main_dlg, *msg_dlg, *status, *no, *yes, *progress, *cancel, *to_cb, *from_cb, *start_btn, *help_btn, *close_btn, *cpuidcheck;

/* combo box counters */
int src_count, dst_count;

/* device names */
char src_dev[32], dst_dev[32];

/* mount points */
char src_mnt[32], dst_mnt[32];

/* flag to show that new partition UUIDs should be created */
char new_uuid;

/* flag to show that copy thread is running */
char copying;

/* flag to show that copy has been interrupted */
char ended;

/* flag to show that backup has been cancelled by the user */
char cancelled;
#define CANCEL_CHECK if (cancelled) { if (cancelled == 1) g_idle_add (close_msg, NULL); else terminate_dialog (_("Drives changed - copy aborted")); return NULL; }

/* flag to show state - inactive, confirm prompt or cloning */
typedef enum {
    STATE_IDLE,
    STATE_CONF,
    STATE_COPY
} state_t;

state_t state;

/*---------------------------------------------------------------------------*/
/* Function definitions */
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/* System helpers */

/* Call a system command and read the first string returned */

static int get_string (char *cmd, char *name)
{
    FILE *fp;
    char buf[64];
    int res;

    name[0] = 0;
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
        res = sscanf (buf, "%s", name);
        if (res != 1) return 0;
        return 1;
    }
}

/* System function with printf formatting */

static int sys_printf (const char * format, ...)
{
    char buffer[256];
    va_list args;
    FILE *fp;

    va_start (args, format);
    vsprintf (buffer, format, args);
    fp = popen (buffer, "r");
    va_end (args);
    return pclose (fp);
}


/*---------------------------------------------------------------------------*/
/* UI helpers */

/* Close the progress dialog - usually called on idle */

static gboolean close_msg (gpointer data)
{
    gtk_widget_destroy (GTK_WIDGET (msg_dlg));
    return FALSE;
}


/* Update the progress dialog with a message to show that backup has ended */

static gboolean cb_terminate (gpointer data)
{
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 1.0);
    gtk_label_set_text (GTK_LABEL (status), (char *) data);
    gtk_button_set_label (GTK_BUTTON (cancel), _("OK"));
    gtk_widget_set_sensitive (GTK_WIDGET (cancel), TRUE);
    return FALSE;
}

static void terminate_dialog (char *msg)
{
    ended = 1;
    state = STATE_IDLE;
    gdk_threads_add_idle (cb_terminate, msg);
}


/* Get a partition name - format is different on mmcblk from sd */

static char *partition_name (char *device, char *buffer)
{
    if (!strncmp (device, "/dev/mmcblk", 11))
        sprintf (buffer, "%sp", device);
    else
        sprintf (buffer, "%s", device);
    return buffer;
}

/* Callbacks to main thread to update UI */

static gboolean cb_update_progress (gpointer data)
{
    float *fptr = (float *) &data;
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), *fptr);
    return FALSE;
}

static void update_progress (float prog)
{
    void *ptr;
    float *fptr = (float *) &ptr;
    *fptr = prog;
    gdk_threads_add_idle (cb_update_progress, ptr);
}

static gboolean cb_update_label (gpointer data)
{
    gtk_label_set_text (GTK_LABEL (status), (char *) data);
    return FALSE;
}

static void update_label (char *msg)
{
    gdk_threads_add_idle (cb_update_label, msg);
}

/*---------------------------------------------------------------------------*/
/* Threads */

/* Thread which calls the system copy command to do the bulk of the work */

static gpointer copy_thread (gpointer data)
{
    copying = 1;
    sys_printf ("cp -ax %s/. %s/.", src_mnt, dst_mnt);
    copying = 0;
    return NULL;
}


/* Thread which sets up all the partitions */

static gpointer backup_thread (gpointer data)
{
    char buffer[256], res[256], dev[16], uuid[64], puuid[64], npuuid[64];
    int n, p, lbl, uid, puid;
    long srcsz, dstsz, stime;
    double prog;
    FILE *fp;

    // get a new partition UUID
    get_string ("uuid | cut -f1 -d-", npuuid);

    // check the source has an msdos partition table
    sprintf (buffer, "parted %s unit s print | tail -n +4 | head -n 1", src_dev);
    fp = popen (buffer, "r");
    if (fp == NULL) return NULL;
    if (fgets (buffer, sizeof (buffer) - 1, fp) == NULL)
    {
        pclose (fp);
        terminate_dialog (_("Unable to read source."));
        return NULL;
    }
    pclose (fp);
    if (strncmp (buffer, "Partition Table: msdos", 22))
    {
        terminate_dialog (_("Non-MSDOS partition table on source."));
        return NULL;
    }
    else
    CANCEL_CHECK;

    update_progress (1.0);
    update_label (_("Preparing target..."));
    update_progress (0.0);

    // unmount any partitions on the target device
    for (n = 9; n >= 1; n--)
    {
        sys_printf ("umount %s%d", partition_name (dst_dev, dev), n);
        CANCEL_CHECK;
    }

    // wipe the FAT on the target
    if (sys_printf ("dd if=/dev/zero of=%s bs=512 count=1", dst_dev))
    {
        terminate_dialog (_("Could not write to destination."));
        return NULL;
    }
    CANCEL_CHECK;
    
    // prepare temp mount points
    get_string ("mktemp -d", src_mnt);
    CANCEL_CHECK;
    get_string ("mktemp -d", dst_mnt);
    CANCEL_CHECK;
    
    // prepare the new FAT
    if (sys_printf ("parted -s %s mklabel msdos", dst_dev))
    {
        terminate_dialog (_("Could not create FAT."));
        return NULL;
    }
    CANCEL_CHECK;

    update_progress (1.0);
    update_label (_("Reading partitions..."));
    update_progress (0.0);

    // read in the source partition table
    n = 0;
    sprintf (buffer, "parted %s unit s print | sed '/^ /!d'", src_dev);
    fp = popen (buffer, "r");
    if (fp != NULL)
    {
        while (1)
        {
            if (fgets (buffer, sizeof (buffer) - 1, fp) == NULL) break;
            if (n >= MAXPART)
            {
                pclose (fp);
                terminate_dialog (_("Too many partitions on source."));
                return NULL;
            }
            sscanf (buffer, "%d %lds %lds %*ds %s %s %s", &(parts[n].pnum), &(parts[n].start),
                &(parts[n].end), (char *) &(parts[n].ptype), (char *) &(parts[n].ftype), (char *) &(parts[n].flags));
            n++;
        }
        pclose (fp);
    }
    CANCEL_CHECK;

    update_progress (1.0);
    update_label (_("Preparing partitions..."));
    update_progress (0.0);
    
    // recreate the partitions on the target
    for (p = 0; p < n; p++)
    {
        // create the partition
        if (!strcmp (parts[p].ptype, "extended"))
        {
            if (sys_printf ("parted -s %s -- mkpart extended %lds -1s", dst_dev, parts[p].start))
            {
                terminate_dialog (_("Could not create partition."));
                return NULL;
            }
        }
        else
        {
            if (p == (n - 1))
            {
                if (sys_printf ("parted -s %s -- mkpart %s %s %lds -1s", dst_dev,
                    parts[p].ptype, parts[p].ftype, parts[p].start))
                {
                    terminate_dialog (_("Could not create partition."));
                    return NULL;
                }
            }
            else
            {
                if (sys_printf ("parted -s %s mkpart %s %s %lds %lds", dst_dev,
                    parts[p].ptype, parts[p].ftype, parts[p].start, parts[p].end))
                {
                    terminate_dialog (_("Could not create partition."));
                    return NULL;
                }
            }
        }
        CANCEL_CHECK;

        // refresh the kernel partion table
        sys_printf ("partprobe");
        CANCEL_CHECK;

        // get the UUID
        sprintf (buffer, "lsblk -o name,uuid %s | grep %s%d | tr -s \" \" | cut -d \" \" -f 2", src_dev, partition_name (src_dev, dev) + 5, parts[p].pnum);
        uid = get_string (buffer, uuid);
        if (uid)
        {
            // sanity check the ID
            if (strlen (uuid) == 9)
            {
                if (uuid[4] == '-')
                {
                    // remove the hyphen from the middle of a FAT volume ID
                    uuid[4] = uuid[5];
                    uuid[5] = uuid[6];
                    uuid[6] = uuid[7];
                    uuid[7] = uuid[8];
                    uuid[8] = 0;
                }
                else uid = 0;
            }
            else if (strlen (uuid) == 36)
            {
                // check there are hyphens in the right places in a UUID
                if (uuid[8] != '-') uid = 0;
                if (uuid[13] != '-') uid = 0;
                if (uuid[18] != '-') uid = 0;
                if (uuid[23] != '-') uid = 0;
            }
            else uid = 0;
        }

        // get the label
        sprintf (buffer, "lsblk -o name,label %s | grep %s%d | tr -s \" \" | cut -d \" \" -f 2", src_dev, partition_name (src_dev, dev) + 5, parts[p].pnum);
        lbl = get_string (buffer, res);
        if (!strlen (res)) lbl = 0;

        // get the partition UUID
        sprintf (buffer, "blkid %s | rev | cut -f 2 -d ' ' | rev | cut -f 2 -d \\\"", src_dev);
        puid = get_string (buffer, puuid);
        if (!strlen (puuid)) puid = 0;

        // create file systems
        if (!strncmp (parts[p].ftype, "fat", 3))
        {
            if (uid) sprintf (buffer, "mkfs.fat -F 32 -i %s %s%d", uuid, partition_name (dst_dev, dev), parts[p].pnum);
            else sprintf (buffer, "mkfs.fat -F 32 %s%d", partition_name (dst_dev, dev), parts[p].pnum);

            if (sys_printf (buffer))
            {
                if (uid)
                {
                    // second try just in case the only problem was a corrupt UUID
                    sprintf (buffer, "mkfs.fat -F 32 %s%d", partition_name (dst_dev, dev), parts[p].pnum);
                    if (sys_printf (buffer))
                    {
                        terminate_dialog (_("Could not create file system."));
                        return NULL;
                    }
                }
                else
                {
                    terminate_dialog (_("Could not create file system."));
                    return NULL;
                }
            }

            if (lbl) sys_printf ("fatlabel %s%d %s", partition_name (dst_dev, dev), parts[p].pnum, res);
        }
        CANCEL_CHECK;

        if (!strcmp (parts[p].ftype, "ext4"))
        {
            if (uid) sprintf (buffer, "mkfs.ext4 -F -U %s %s%d", uuid, partition_name (dst_dev, dev), parts[p].pnum);
            else sprintf (buffer, "mkfs.ext4 -F %s%d", partition_name (dst_dev, dev), parts[p].pnum);

            if (sys_printf (buffer))
            {
                if (uid)
                {
                    // second try just in case the only problem was a corrupt UUID
                    sprintf (buffer, "mkfs.ext4 -F %s%d", partition_name (dst_dev, dev), parts[p].pnum);
                    if (sys_printf (buffer))
                    {
                        terminate_dialog (_("Could not create file system."));
                        return NULL;
                    }
                }
                else
                {
                    terminate_dialog (_("Could not create file system."));
                    return NULL;
                }
            }

            if (lbl) sys_printf ("e2label %s%d %s", partition_name (dst_dev, dev), parts[p].pnum, res);
        }
        CANCEL_CHECK;

        // write the partition UUID
        if (puid) sys_printf ("echo \"x\ni\n0x%s\nr\nw\n\" | fdisk %s", new_uuid ? npuuid : puuid, dst_dev);
        CANCEL_CHECK;

        prog = p + 1;
        prog /= n;
        update_progress (prog);
    }

    // do the copy for each partition
    for (p = 0; p < n; p++)
    {
        // don't try to copy extended partitions
        if (strcmp (parts[p].ptype, "extended"))
        {
            sprintf (buffer, _("Copying partition %d of %d..."), p + 1, n);
            update_label (buffer);
            update_progress (0.0);

            // belt-and-braces call to partprobe to make sure devices are found...
            get_string ("partprobe", res);

            // mount partitions
            if (sys_printf ("mount %s%d %s", partition_name (dst_dev, dev), parts[p].pnum, dst_mnt))
            {
                terminate_dialog (_("Could not mount partition."));
                return NULL;
            }
            CANCEL_CHECK;
            if (sys_printf ("mount %s%d %s", partition_name (src_dev, dev), parts[p].pnum, src_mnt))
            {
                terminate_dialog (_("Could not mount partition."));
                return NULL;
            }
            CANCEL_CHECK;

            // check there is enough space...
            sprintf (buffer, "df %s | tail -n 1 | tr -s \" \" \" \" | cut -d ' ' -f 3", src_mnt);
            get_string (buffer, res);
            sscanf (res, "%ld", &srcsz);

            sprintf (buffer, "df %s | tail -n 1 | tr -s \" \" \" \" | cut -d ' ' -f 4", dst_mnt);
            get_string (buffer, res);
            sscanf (res, "%ld", &dstsz);

            if (srcsz >= dstsz)
            {
                sys_printf ("umount %s", dst_mnt);
                sys_printf ("umount %s", src_mnt);
                terminate_dialog (_("Insufficient space. Backup aborted."));
                return NULL;
            }

            // start the copy itself in a new thread
            g_thread_new (NULL, copy_thread, NULL);

            // get the size to be copied
            sprintf (buffer, "du -s %s", src_mnt);
            get_string (buffer, res);
            sscanf (res, "%ld", &srcsz);
            if (srcsz < 50000) stime = 1;
            else if (srcsz < 500000) stime = 5;
            else stime = 10;

            // wait for the copy to complete, while updating the progress bar...
            sprintf (buffer, "du -s %s", dst_mnt);
            while (copying)
            {
                get_string (buffer, res);
                sscanf (res, "%ld", &dstsz);
                prog = dstsz;
                prog /= srcsz;
                update_progress (prog);
                sleep (stime);
                CANCEL_CHECK;
            }

            update_progress (1.0);

            // fix up relevant files if changing partition UUID
            if (puid && new_uuid)
            {
                // relevant files are dst_mnt/etc/fstab and dst_mnt/boot/cmdline.txt
                sys_printf ("if [ -e /%s/etc/fstab ] ; then sed -i s/%s/%s/g /%s/etc/fstab ; fi", dst_mnt, puuid, npuuid, dst_mnt);
                sys_printf ("if [ -e /%s/cmdline.txt ] ; then sed -i s/%s/%s/g /%s/cmdline.txt ; fi", dst_mnt, puuid, npuuid, dst_mnt);
            }

            // unmount partitions
            if (sys_printf ("umount %s", dst_mnt))
            {
                terminate_dialog (_("Could not unmount partition."));
                return NULL;
            }
            CANCEL_CHECK;
            if (sys_printf ("umount %s", src_mnt))
            {
                terminate_dialog (_("Could not unmount partition."));
                return NULL;
            }
            CANCEL_CHECK;
        }

        // set the flags
        if (!strcmp (parts[p].flags, "lba"))
        {
            if (sys_printf ("parted -s %s set %d lba on", dst_dev, parts[p].pnum))
            {
                terminate_dialog (_("Could not set flags."));
                return NULL;
            }
        }
        else
        {
            if (sys_printf ("parted -s %s set %d lba off", dst_dev, parts[p].pnum))
            {
                terminate_dialog (_("Could not set flags."));
                return NULL;
            }
        }
        CANCEL_CHECK;
    }

    terminate_dialog (_("Copy complete."));
    return NULL;
}


/*---------------------------------------------------------------------------*/
/* Progress dialog UI handlers */

static void kill_copy (void)
{
    FILE *fp;
    char buffer[256];
    int pid;

    if (copying)
    {
        sprintf (buffer, "ps ax | grep \"cp -ax %s/. %s/.\" | grep -v \"grep\"", src_mnt, dst_mnt);
        fp = popen (buffer, "r");
        if (fp != NULL)
        {
            while (1)
            {
                if (fgets (buffer, sizeof (buffer) - 1, fp) == NULL) break;
                if (sscanf (buffer, "%d", &pid) == 1) sys_printf ("kill %d", pid);
            }
            pclose (fp);
        }
        copying = 0;
    }
}

/* Handler for cancel button */


static gboolean cb_cancel (gpointer data)
{
    // hide the progress bar and disable the cancel button
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (progress));
    gtk_label_set_text (GTK_LABEL (status), _("Cancelling..."));
    gtk_widget_set_sensitive (GTK_WIDGET (cancel), FALSE);
    return FALSE;
}

static gboolean on_cancel (void)
{
    if (ended)
    {
        g_idle_add (close_msg, NULL);
        return FALSE;
    }

    gdk_threads_add_idle (cb_cancel, NULL);

    // kill copy processes if running
    kill_copy ();
    cancelled = 1;
    state = STATE_IDLE;
    return FALSE;
}


/*---------------------------------------------------------------------------*/
/* Confirm dialog UI handlers */

/* Handler for Yes button */

static gboolean on_start (void)
{
    GtkBuilder *builder;
    GtkWidget *wid;

    // close the confirm dialog
    gtk_widget_destroy (msg_dlg);
    state = STATE_COPY;

    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/piclone.ui");
    msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "modal");
    gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));

    // add message
    status = (GtkWidget *) gtk_builder_get_object (builder, "modal_msg");
    gtk_label_set_text (GTK_LABEL (status), _("Checking source..."));

    // add progress bar
    progress = (GtkWidget *) gtk_builder_get_object (builder, "modal_pb");

    // add cancel button
    cancel = (GtkWidget *) gtk_builder_get_object (builder, "modal_cancel");
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), NULL);

    wid = (GtkWidget *) gtk_builder_get_object (builder, "modal_ok");
    gtk_widget_hide (wid);

    gtk_widget_show (GTK_WIDGET (msg_dlg));

    // launch a thread with the system call to run the backup
    cancelled = 0;
    ended = 0;
    g_thread_new (NULL, backup_thread, NULL);
    return FALSE;
}


/* Handler for No button */

static gboolean on_close (void)
{
    gtk_widget_destroy (msg_dlg);
    state = STATE_IDLE;
    return FALSE;
}

/*---------------------------------------------------------------------------*/
/* Main dialog UI handlers */

/* Handler for Start button */

static gboolean on_confirm (void)
{
    char buffer[256], res[256];
    char *src, *dst;
    int len;
    GtkBuilder *builder;
    GtkWidget *wid;

    // set up source and target devices from combobox values
    dst = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb));
    strtok (dst, "(");
    strcpy (dst_dev, strtok (NULL, ")"));

    src = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb));
    strtok (src, "(");
    strcpy (src_dev, strtok (NULL, ")"));

    // read the UUID clone setting
    new_uuid = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (cpuidcheck));

    // basic sanity check - don't do anything if src == dest
    if (!strcmp (src_dev, dst_dev)) return FALSE;

    // create the confirm dialog
    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/piclone.ui");
    msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "modal");
    gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));

    // add message
    len = strlen (dst);
    if (len >= 2) dst[len - 2] = 0;
    sprintf (buffer, _("This will erase all content on the device '%s'. Are you sure?"), dst);
    status = (GtkWidget *) gtk_builder_get_object (builder, "modal_msg");
    gtk_label_set_text (GTK_LABEL (status), buffer);

    wid = (GtkWidget *) gtk_builder_get_object (builder, "modal_pb");
    gtk_widget_hide (wid);

    // add buttons
    no = (GtkWidget *) gtk_builder_get_object (builder, "modal_cancel");
    gtk_button_set_label (GTK_BUTTON (no), _("_No"));
    g_signal_connect (no, "clicked", G_CALLBACK (on_close), NULL);

    yes = (GtkWidget *) gtk_builder_get_object (builder, "modal_ok");
    gtk_button_set_label (GTK_BUTTON (yes), _("_Yes"));
    g_signal_connect (yes, "clicked", G_CALLBACK (on_start), NULL);

    g_free (src);
    g_free (dst);

    gtk_widget_show (GTK_WIDGET (msg_dlg));
    state = STATE_CONF;
    return FALSE;
}


/* Handler for Help button */

static gboolean on_help (void)
{
    GtkBuilder *builder;
    GtkWidget *dlg;

    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/piclone.ui");
    dlg = (GtkWidget *) gtk_builder_get_object (builder, "help");
    g_object_unref (builder);
    gtk_dialog_run (GTK_DIALOG (dlg));
    gtk_widget_destroy (dlg);
    return FALSE;
}


/* Handler for Close button */

static gboolean on_quit (void)
{
    gtk_main_quit ();
    return FALSE;
}


/* Handler for "changed" signal from comboboxes */

static void on_cb_changed (void)
{
    // set the start button to active only if boxes contain different strings
    if (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)) == 0 ||
        gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb)) == 0 ||
        !strcmp (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)),
            gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb))) ||
         !strcmp (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)),
           _("No devices available")))
        gtk_widget_set_sensitive (GTK_WIDGET (start_btn), FALSE);
    else
        gtk_widget_set_sensitive (GTK_WIDGET (start_btn), TRUE);
}


/* Handler for drives changed signal from volume monitor */

static void on_drives_changed (GVolumeMonitor *volume_monitor, GDrive *drive, gpointer user_data)
{
    char buffer[256], test[128];
    FILE *fp;
    int devlen;

    if (state == STATE_CONF)
    {
        gtk_label_set_text (GTK_LABEL (status), _("Drives changed - copy aborted"));
        gtk_button_set_label (GTK_BUTTON (no), _("OK"));
        gtk_widget_hide (yes);
        state = STATE_IDLE;
    }

    if (state == STATE_COPY && (int) user_data == 1)
    {
        // hide the progress bar and disable the cancel button
        gtk_progress_bar_pulse (GTK_PROGRESS_BAR (progress));
        gtk_label_set_text (GTK_LABEL (status), _("Drives changed - cancelling..."));
        gtk_widget_set_sensitive (GTK_WIDGET (cancel), FALSE);

        // kill copy processes if running
        kill_copy ();
        cancelled = 2;
        state = STATE_IDLE;
    }

    // empty the comboboxes
    while (src_count)
    {
        gtk_combo_box_text_remove (GTK_COMBO_BOX_TEXT (from_cb), 0);
        src_count--;
    }
    while (dst_count)
    {
        gtk_combo_box_text_remove (GTK_COMBO_BOX_TEXT (to_cb), 0);
        dst_count--;
    }

    // populate the comboboxes
    GList *iter, *drives = g_volume_monitor_get_connected_drives (monitor);
    for (iter = drives; iter != NULL; iter = g_list_next (iter))
    {
        GDrive *d = iter->data;
        char *id = g_drive_get_identifier (d, "unix-device");
        char *n = g_drive_get_name (d);
        sprintf (buffer, "%s  (%s)", n, id);
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (from_cb), buffer);
        src_count++;

        // do not allow the current root and boot devices as targets
        sprintf (test, "lsblk %s | grep -Eq \"part /(boot)?$\"", id);
        fp = popen (test, "r");
        if (fp && pclose (fp))
        {
            gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (to_cb), buffer);
            dst_count++;
        }
        g_free (id);
        g_free (n);
    }
    g_list_free_full (drives, g_object_unref);

    if (dst_count == 0)
    {
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (to_cb), _("No devices available"));
        gtk_combo_box_set_active (GTK_COMBO_BOX (to_cb), 0);
        gtk_widget_set_sensitive (GTK_WIDGET (to_cb), FALSE);
        dst_count++;
    }
    else gtk_widget_set_sensitive (GTK_WIDGET (to_cb), TRUE);
}


/*---------------------------------------------------------------------------*/
/* Main function - main dialog */

int main (int argc, char *argv[])
{
    GtkBuilder *builder;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    // GTK setup
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    // build the UI
    builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/piclone.ui");
    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "main_window");

    // set up the start button
    start_btn = (GtkWidget *) gtk_builder_get_object (builder, "btn_start");
    g_signal_connect (start_btn, "clicked", G_CALLBACK (on_confirm), NULL);
    gtk_widget_set_sensitive (GTK_WIDGET (start_btn), FALSE);

    // set up the close button
    close_btn = (GtkWidget *) gtk_builder_get_object (builder, "btn_close");
    g_signal_connect (close_btn, "clicked", G_CALLBACK (on_quit), NULL);

    // set up the help button
    help_btn = (GtkWidget *) gtk_builder_get_object (builder, "btn_help");
    g_signal_connect (help_btn, "clicked", G_CALLBACK (on_help), NULL);

    // get the new UUID checkbox - uncheck it by default
    cpuidcheck = (GtkWidget *) gtk_builder_get_object (builder, "cpcheck");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cpuidcheck), FALSE);

    // create and add the source combobox
    src_count = 0;
    from_cb = (GtkWidget *) gtk_builder_get_object (builder, "cb_from");
    g_signal_connect (from_cb, "changed", G_CALLBACK (on_cb_changed), NULL);

    // create and add the destination combobox
    dst_count = 0;
    to_cb = (GtkWidget *) gtk_builder_get_object (builder, "cb_to");
    g_signal_connect (to_cb, "changed", G_CALLBACK (on_cb_changed), NULL);

    // configure monitoring for drives being connected or disconnected
    monitor = g_volume_monitor_get ();

    // populate the comboboxes
    on_drives_changed (NULL, NULL, 0);

    g_signal_connect (monitor, "drive_changed", G_CALLBACK (on_drives_changed), (void *) 0);
    g_signal_connect (monitor, "drive_connected", G_CALLBACK (on_drives_changed), (void *) 1);
    g_signal_connect (monitor, "drive_disconnected", G_CALLBACK (on_drives_changed), (void *) 1);
    g_signal_connect (monitor, "mount_changed", G_CALLBACK (on_drives_changed), (void *) 0);
    g_signal_connect (monitor, "mount_added", G_CALLBACK (on_drives_changed), (void *) 0);
    g_signal_connect (monitor, "mount_removed", G_CALLBACK (on_drives_changed), (void *) 0);

    state = STATE_IDLE;

    g_object_unref (builder);

    gtk_widget_show (main_dlg);
    gtk_main ();

    gtk_widget_destroy (main_dlg);

    return 0;
}

/* End of file */
/*===========================================================================*/
