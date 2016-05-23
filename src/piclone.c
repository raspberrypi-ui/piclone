#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

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

/* control widget globals */
static GtkWidget *main_dlg, *msg_dlg, *status, *progress, *cancel, *to_cb, *from_cb, *start_btn, *help_btn;

/* combo box counters */
int src_count, dst_count;

/* device names */
char src_dev[32], dst_dev[32];

/* mount points */
char src_mnt[32], dst_mnt[32];

/* flag to show that copy thread is running */
char copying;

/* flag to show that copy has been interrupted */
char ended;

/* flag to show that backup has been cancelled by the user */
char cancelled;
#define CANCEL_CHECK if (cancelled) { g_idle_add (close_msg, NULL); return NULL; }

/* debug system function - remove when no longer needed !!!! */

void dsystem (char * cmd)
{
    printf ("%s\n", cmd);
    system (cmd);
}
//#define system dsystem

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

    fp = popen (cmd, "r");
    if (fp == NULL || fgets (buf, sizeof (buf) - 1, fp) == NULL) return 0;
    return sscanf (buf, "%s", name);
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

static void terminate_dialog (char *msg)
{
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 1.0);
    gtk_label_set_text (GTK_LABEL (status), msg);
    gtk_button_set_label (GTK_BUTTON (cancel), _("OK"));
    ended = 1;
}


/* Parse the partition table to get a device name */

static int get_dev_name (char *dev, char *name)
{
    char buffer[256];
    FILE *fp;

    sprintf (buffer, "sudo parted -l | grep -B 1 \"%s\" | head -n 1 | cut -d \":\" -f 2 | cut -d \"(\" -f 1", dev);
    fp = popen (buffer, "r");
    if (fp == NULL || fgets (buffer, sizeof (buffer) - 1, fp) == NULL) return 0;
    buffer[strlen (buffer) - 2] = 0;
    strcpy (name, buffer + 1);
    return 1;
}


static char *partition_name (char *device, char *buffer)
{
    if (!strncmp (device, "/dev/mmcblk", 11))
        sprintf (buffer, "%sp", device);
    else
        sprintf (buffer, "%s", device);
    return buffer;
}


/*---------------------------------------------------------------------------*/
/* Threads */

/* Thread which calls the system copy command to do the bulk of the work */

static gpointer copy_thread (gpointer data)
{
    copying = 1;
    sys_printf ("sudo cp -ax %s/. %s/.", src_mnt, dst_mnt);
    copying = 0;
    return NULL;
}


/* Thread which sets up all the partitions */

static gpointer backup_thread (gpointer data)
{
    char buffer[256], res[256], dev[16];
    int n, p;
    long srcsz, dstsz, stime;
    double prog;
    FILE *fp;

    // check the source has an msdos partition table
    sprintf (buffer, "sudo parted %s unit s print | tail -n +4 | head -n 1", src_dev);  
    fp = popen (buffer, "r");
    if (fp == NULL || fgets (buffer, sizeof (buffer) - 1, fp) == NULL)
    {
        terminate_dialog (_("Unable to read source."));
        return;
    }
    if (strncmp (buffer, "Partition Table: msdos", 22))
    {
        terminate_dialog (_("Non-MSDOS partition table on source."));
        return;
    }
    else
    CANCEL_CHECK;

    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 1.0);
    gtk_label_set_text (GTK_LABEL (status), _("Preparing target..."));
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);

    // unmount any partitions on the target device
    for (n = 9; n >= 1; n--)
    {
        sys_printf ("sudo umount %s%d", partition_name (dst_dev, dev), n);
        CANCEL_CHECK;
    }

    // wipe the FAT on the target
    if (sys_printf ("sudo dd if=/dev/zero of=%s bs=512 count=1", dst_dev))
    {
        terminate_dialog (_("Could not write to destination."));
        return;
    }
    CANCEL_CHECK;
    
    // prepare temp mount points
    get_string ("mktemp -d", src_mnt);
    CANCEL_CHECK;
    get_string ("mktemp -d", dst_mnt);
    CANCEL_CHECK;
    
    // prepare the new FAT
    if (sys_printf ("sudo parted -s %s mklabel msdos", dst_dev))
    {
        terminate_dialog (_("Could not create FAT."));
        return;
    }
    CANCEL_CHECK;

    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 1.0);
    gtk_label_set_text (GTK_LABEL (status), _("Reading partitions..."));
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);

    // read in the source partition table
    n = 0;
    sprintf (buffer, "sudo parted %s unit s print | tail -n +8 | head -n -1", src_dev);
    fp = popen (buffer, "r");
    if (fp != NULL)
    {
        while (1)
        {
            if (fgets (buffer, sizeof (buffer) - 1, fp) == NULL) break;
            if (n >= MAXPART)
            {
                terminate_dialog (_("Too many partitions on source."));
                return;
            }
            sscanf (buffer, "%d %lds %lds %*lds %s %s %s", &(parts[n].pnum), &(parts[n].start),
                &(parts[n].end), &(parts[n].ptype), &(parts[n].ftype), &(parts[n].flags));
            n++;
        }
    }
    CANCEL_CHECK;

    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 1.0);
    gtk_label_set_text (GTK_LABEL (status), _("Preparing partitions..."));
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);
    
    // recreate the partitions on the target
    for (p = 0; p < n; p++)
    {
        // create the partition
        if (!strcmp (parts[p].ptype, "extended"))
        {
            if (sys_printf ("sudo parted -s %s -- mkpart extended %lds -1s", dst_dev, parts[p].start))
            {
                terminate_dialog (_("Could not create partition."));
                return;
            }
        }
        else
        {
            if (p == (n - 1))
            {
                if (sys_printf ("sudo parted -s %s -- mkpart %s %s %lds -1s", dst_dev,
                    parts[p].ptype, parts[p].ftype, parts[p].start))
                {
                    terminate_dialog (_("Could not create partition."));
                    return;
                }
            }
            else
            {
                if (sys_printf ("sudo parted -s %s mkpart %s %s %lds %lds", dst_dev,
                    parts[p].ptype, parts[p].ftype, parts[p].start, parts[p].end))
                {
                    terminate_dialog (_("Could not create partition."));
                    return;
                }
            }
        }
        CANCEL_CHECK;

        // refresh the kernel partion table
        system ("sudo partprobe");
        CANCEL_CHECK;

        // create file systems
        if (!strncmp (parts[p].ftype, "fat", 3))
        {
            if (sys_printf ("sudo mkfs.fat %s%d", partition_name (dst_dev, dev), parts[p].pnum))
            {
                terminate_dialog (_("Could not create file system."));
                return;
            }
        }
        CANCEL_CHECK;

        if (!strcmp (parts[p].ftype, "ext4"))
        {
            if (sys_printf ("sudo mkfs.ext4 -F %s%d", partition_name (dst_dev, dev), parts[p].pnum))
            {
                terminate_dialog (_("Could not create file system."));
                return;
            }
        }
        CANCEL_CHECK;

        // set the flags        
        if (!strcmp (parts[p].flags, "lba"))
        {
            if (sys_printf ("sudo parted -s %s set %d lba on", dst_dev, parts[p].pnum))
            {
                terminate_dialog (_("Could not set flags."));
                return;
            }
        }
        else
        {
            if (sys_printf ("sudo parted -s %s set %d lba off", dst_dev, parts[p].pnum))
            {
                terminate_dialog (_("Could not set flags."));
                return;
            }
        }
        CANCEL_CHECK;
        
        prog = p + 1;
        prog /= n;
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), prog);
    }

    // do the copy for each partition
    for (p = 0; p < n; p++)
    {
        // don't try to copy extended partitions
        if (!strcmp (parts[p].ptype, "extended")) continue;

        sprintf (buffer, _("Copying partition %d of %d..."), p + 1, n);
        gtk_label_set_text (GTK_LABEL (status), buffer);
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);

        // mount partitions
        if (sys_printf ("sudo mount %s%d %s", partition_name (dst_dev, dev), parts[p].pnum, dst_mnt))
        {
            terminate_dialog (_("Could not mount partition."));
            return;
        }
        CANCEL_CHECK;
        if (sys_printf ("sudo mount %s%d %s", partition_name (src_dev, dev), parts[p].pnum, src_mnt))
        {
            terminate_dialog (_("Could not mount partition."));
            return;
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
            sys_printf ("sudo umount %s", dst_mnt);
            sys_printf ("sudo umount %s", src_mnt);
            terminate_dialog (_("Insufficient space. Backup aborted."));
            return NULL;
        }

        // start the copy itself in a new thread
        g_thread_new (NULL, copy_thread, NULL);

        // get the size to be copied
        sprintf (buffer, "sudo du -s %s", src_mnt);
        get_string (buffer, res);
        sscanf (res, "%ld", &srcsz);
        if (srcsz < 50000) stime = 1;
        else if (srcsz < 500000) stime = 5;
        else stime = 10;

        // wait for the copy to complete, while updating the progress bar...
        sprintf (buffer, "sudo du -s %s", dst_mnt);
        while (copying)
        {
            get_string (buffer, res);
            sscanf (res, "%ld", &dstsz);
            prog = dstsz;
            prog /= srcsz;
            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), prog);
            sleep (stime);
            CANCEL_CHECK;
        }

        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 1.0);

        // unmount partitions
        if (sys_printf ("sudo umount %s", dst_mnt))
        {
            terminate_dialog (_("Could not unmount partition."));
            return;
        }
        CANCEL_CHECK;
        if (sys_printf ("sudo umount %s", src_mnt))
        {
            terminate_dialog (_("Could not unmount partition."));
            return;
        }
        CANCEL_CHECK;
    }

    terminate_dialog (_("Copy complete."));
    return NULL;
}


/*---------------------------------------------------------------------------*/
/* Progress dialog UI handlers */

/* Handler for cancel button */

static void on_cancel (void)
{
    FILE *fp;
    char buffer[256];
    int pid;

    if (ended)
    {
        g_idle_add (close_msg, NULL);
        return;
    }

    // hide the progress bar and disable the cancel button
    gtk_progress_bar_pulse (GTK_PROGRESS_BAR (progress));
    gtk_label_set_text (GTK_LABEL (status), "Cancelling...");
    gtk_widget_set_sensitive (GTK_WIDGET (cancel), FALSE);

    // kill copy processes if running
    if (copying)
    {
        sprintf (buffer, "ps ax | grep \"sudo cp -ax %s/. %s/.\" | grep -v \"grep\"", src_mnt, dst_mnt);
        fp = popen (buffer, "r");
        if (fp != NULL)
        {
            while (1)
            {
                if (fgets (buffer, sizeof (buffer) - 1, fp) == NULL) break;
                if (sscanf (buffer, "%d", &pid) == 1) sys_printf ("sudo kill %d", pid);
            }
        }
        copying = 0;
    }
    cancelled = 1;
}


/*---------------------------------------------------------------------------*/
/* Confirm dialog UI handlers */

/* Handler for Yes button */

static void on_start (void)
{
    // close the confirm dialog
    gtk_widget_destroy (msg_dlg);

    // create the progress dialog
    msg_dlg = (GtkWidget *) gtk_dialog_new ();
    gtk_window_set_title (GTK_WINDOW (msg_dlg), "");
    gtk_window_set_modal (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_decorated (GTK_WINDOW (msg_dlg), FALSE);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));

    // add border
    GtkWidget *frame = gtk_frame_new (NULL);
    gtk_container_add (GTK_CONTAINER (gtk_dialog_get_action_area (GTK_DIALOG (msg_dlg))), frame);

    // add container
    GtkWidget *box = (GtkWidget *) gtk_vbox_new (TRUE, 5);
    gtk_container_set_border_width (GTK_CONTAINER (box), 10);
    gtk_container_add (GTK_CONTAINER (frame), box);

    // add message
    status = (GtkWidget *) gtk_label_new (_("Checking source..."));
    gtk_label_set_width_chars (GTK_LABEL (status), 30);
    gtk_box_pack_start (GTK_BOX (box), status, FALSE, FALSE, 5);

    // add progress bar
    progress = (GtkWidget *) gtk_progress_bar_new ();
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);
    gtk_box_pack_start (GTK_BOX (box), progress, FALSE, FALSE, 5);

    // add cancel button
    cancel = (GtkWidget *) gtk_button_new_with_label (_("Cancel"));
    gtk_box_pack_start (GTK_BOX (box), cancel, FALSE, FALSE, 5);
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), NULL);

    gtk_widget_show_all (GTK_WIDGET (msg_dlg));

    // launch a thread with the system call to run the backup
    cancelled = 0;
    ended = 0;
    g_thread_new (NULL, backup_thread, NULL);
}


/* Handler for No button */

static void on_close (void)
{
    gtk_widget_destroy (msg_dlg);
}

/*---------------------------------------------------------------------------*/
/* Main dialog UI handlers */

/* Handler for Start button */

static void on_confirm (void)
{
    char buffer[256], res[256];
    char *ptr;

    // set up source and target devices from combobox values
    ptr = strrchr (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)), '(');
    strcpy (dst_dev, ptr + 1);
    dst_dev[strlen (dst_dev) - 1] = 0;

    ptr = strrchr (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb)), '(');
    strcpy (src_dev, ptr + 1);
    src_dev[strlen (src_dev) - 1] = 0;

    // basic sanity check - don't do anything if src == dest
    if (!strcmp (src_dev, dst_dev)) return;

    // create the confirm dialog
    msg_dlg = (GtkWidget *) gtk_dialog_new ();
    gtk_window_set_title (GTK_WINDOW (msg_dlg), "");
    gtk_window_set_modal (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_decorated (GTK_WINDOW (msg_dlg), FALSE);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));

    // add border
    GtkWidget *frame = gtk_frame_new (NULL);
    gtk_container_add (GTK_CONTAINER (gtk_dialog_get_action_area (GTK_DIALOG (msg_dlg))), frame);

    // add container
    GtkWidget *box = (GtkWidget *) gtk_vbox_new (TRUE, 5);
    gtk_container_set_border_width (GTK_CONTAINER (box), 10);
    gtk_container_add (GTK_CONTAINER (frame), box);

    // add message
    get_dev_name (dst_dev, res);
    sprintf (buffer, _("This will erase all content on the device '%s'. Are you sure?"), res);
    GtkWidget* status = (GtkWidget *) gtk_label_new (buffer);
    gtk_box_pack_start (GTK_BOX (box), status, FALSE, FALSE, 5);

    GtkWidget *hbox = gtk_hbox_new (TRUE, 10);
    gtk_box_pack_start (GTK_BOX (box), hbox, FALSE, FALSE, 5);

    // add buttons
    GtkWidget *no = (GtkWidget *) gtk_button_new_with_label (_("No"));
    gtk_box_pack_start (GTK_BOX (hbox), no, FALSE, TRUE, 40);
    g_signal_connect (no, "clicked", G_CALLBACK (on_close), NULL);

    GtkWidget *yes = (GtkWidget *) gtk_button_new_with_label (_("Yes"));
    gtk_box_pack_start (GTK_BOX (hbox), yes, FALSE, TRUE, 40);
    g_signal_connect (yes, "clicked", G_CALLBACK (on_start), NULL);

    gtk_widget_show_all (GTK_WIDGET (msg_dlg));
 }


/* Handler for Help button */

static void on_help (void)
{
    GtkBuilder *builder;
    GtkWidget *dlg;

    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/piclone.ui", NULL);
    dlg = (GtkWidget *) gtk_builder_get_object (builder, "dialog2");
    g_object_unref (builder);
    gtk_dialog_run (GTK_DIALOG (dlg));
    gtk_widget_destroy (dlg);
}


/* Handler for "changed" signal from comboboxes */

static void on_cb_changed (void)
{
    // set the start button to active only if boxes contain different strings
    if (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)) == 0 ||
        gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb)) == 0 ||
        !strcmp (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)),
            gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb))))
        gtk_widget_set_sensitive (GTK_WIDGET (start_btn), FALSE);
    else
        gtk_widget_set_sensitive (GTK_WIDGET (start_btn), TRUE);
}


/* Handler for drives changed signal from volume monitor */

static void on_drives_changed (void)
{
    char buffer[256], name[128], device[32];
    FILE *fp;

    // empty the comboboxes
    while (src_count)
    {
        gtk_combo_box_remove_text (GTK_COMBO_BOX (from_cb), 0);
        src_count--;
    }
    while (dst_count)
    {
        gtk_combo_box_remove_text (GTK_COMBO_BOX (to_cb), 0);
        dst_count--;
    }

    // populate the comboboxes
    gtk_combo_box_append_text (GTK_COMBO_BOX (from_cb), _("Internal SD card  (/dev/mmcblk0)"));
    gtk_combo_box_set_active (GTK_COMBO_BOX (from_cb), 0);
    src_count++;

    fp = popen ("sudo parted -l | grep \"^Disk /dev/\" | cut -d ' ' -f 2 | cut -d ':' -f 1", "r");
    if (fp != NULL)
    {
        while (1)
        {
            if (fgets (device, sizeof (device) - 1, fp) == NULL) break;

            if (!strncmp (device + 5, "sd", 2) || !strncmp (device + 5, "mmcblk1", 7) )
            {
                device[strlen (device) - 1] = 0;
                get_dev_name (device, name);
                sprintf (buffer, "%s  (%s)", name, device);
                gtk_combo_box_append_text (GTK_COMBO_BOX (from_cb), buffer);
                gtk_combo_box_append_text (GTK_COMBO_BOX (to_cb), buffer);
                src_count++;
                dst_count++;
            }
        }
    }

    if (dst_count == 0)
    {
        gtk_combo_box_append_text (GTK_COMBO_BOX (to_cb), _("No devices available"));
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
    GVolumeMonitor *monitor;

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    // GTK setup
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    // build the UI
    builder = gtk_builder_new ();
    gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/piclone.ui", NULL);
    main_dlg = (GtkWidget *) gtk_builder_get_object (builder, "dialog1");

    // set up the start button
    start_btn = (GtkWidget *) gtk_builder_get_object (builder, "button1");
    g_signal_connect (start_btn, "clicked", G_CALLBACK (on_confirm), NULL);

    // set up the help button
    help_btn = (GtkWidget *) gtk_builder_get_object (builder, "button4");
    g_signal_connect (help_btn, "clicked", G_CALLBACK (on_help), NULL);

    // get the table which holds the other elements
    GtkWidget *table = (GtkWidget *) gtk_builder_get_object (builder, "table1");

    // create and add the source combobox
    src_count = 0;
    from_cb = (GtkWidget *)  (GObject *) gtk_combo_box_text_new ();
    gtk_widget_set_tooltip_text (from_cb, _("Select the device to copy from"));
    gtk_table_attach (GTK_TABLE (table), GTK_WIDGET (from_cb), 1, 2, 0, 1, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 5);
    gtk_widget_show_all (GTK_WIDGET (from_cb));
    g_signal_connect (from_cb, "changed", G_CALLBACK (on_cb_changed), NULL);

    // create and add the destination combobox
    dst_count = 0;
    to_cb = (GtkWidget *)  (GObject *) gtk_combo_box_text_new ();
    gtk_widget_set_tooltip_text (to_cb, _("Select the device to copy to"));
    gtk_table_attach (GTK_TABLE (table), GTK_WIDGET (to_cb), 1, 2, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 5);
    gtk_widget_show_all (GTK_WIDGET (to_cb));
    g_signal_connect (to_cb, "changed", G_CALLBACK (on_cb_changed), NULL);

    // populate the comboboxes
    on_drives_changed ();

    // configure monitoring for drives being connected or disconnected
    monitor = g_volume_monitor_get ();
    g_signal_connect (monitor, "drive_changed", G_CALLBACK (on_drives_changed), NULL);
    g_signal_connect (monitor, "drive_connected", G_CALLBACK (on_drives_changed), NULL);
    g_signal_connect (monitor, "drive_disconnected", G_CALLBACK (on_drives_changed), NULL);

    g_object_unref (builder);
    gtk_dialog_run (GTK_DIALOG (main_dlg));
    gtk_widget_destroy (main_dlg);

    return 0;
}

/* End of file */
/*===========================================================================*/
