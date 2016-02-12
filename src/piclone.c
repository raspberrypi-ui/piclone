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
#include <gdk/gdkx.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>

/* Global variables for window values */

/* Controls */
static GtkWidget *main_dlg, *msg_dlg, *status, *progress, *cancel, *to_cb, *from_cb, *start_btn;

/* device names */
char src_dev[32], dst_dev[32];

/* mount points */
char src_mnt[32], dst_mnt[32];

/* flag to show that copy thread is running */
char copying;

/* partition data */

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



static void get_string (char *cmd, char *name)
{
    FILE *fp = popen (cmd, "r");
    char buf[64];

    if (fp == NULL) return;
    if (fgets (buf, sizeof (buf) - 1, fp) != NULL)
    {
        sscanf (buf, "%s", name);
        return;
    }
}

static gboolean close_msg (gpointer data)
{
	gtk_widget_destroy (GTK_WIDGET (msg_dlg));
	return FALSE;
}

static gpointer copy_thread (gpointer data)
{
	char ctbuffer[256];
	sprintf (ctbuffer, "sudo cp -ax %s/. %s/.", src_mnt, dst_mnt);
	system (ctbuffer);
	copying = 0;
	return NULL;
}

static void backup_thread_msg (char *msg)
{
    gtk_widget_set_visible (GTK_WIDGET (progress), FALSE);
    gtk_label_set_text (GTK_LABEL (status), msg);
    gtk_button_set_label (GTK_BUTTON (cancel), _("OK"));
}


static gpointer backup_thread (gpointer data)
{
    char buffer[256], res[256];
    int n, p;
    long srcsz, dstsz;
    double prog;
    FILE *fp;

    // check the source has an msdos partition table, or all else is for naught...
    sprintf (buffer, "sudo parted %s unit s print | tail -n +4 | head -n 1", src_dev);  
    fp = popen (buffer, "r");
    if (fp == NULL)
    {
        backup_thread_msg (_("Unable to read source"));
        return;
    }
    if (fgets (buffer, sizeof (buffer) - 1, fp) != NULL)
    {
        if (strncmp (buffer, "Partition Table: msdos", 22))
        {
            backup_thread_msg (_("Non-MSDOS partition table on source"));
            return;
        }
    }
    else
    {
        backup_thread_msg (_("Unable to read source"));
        return;
    }

    gtk_label_set_text (GTK_LABEL (status), _("Preparing target..."));

    // unmount any partitions on the target device
    for (n = 9; n >= 1; n--)
    {
        sprintf (buffer, "sudo umount %s%d", dst_dev, n);
        system (buffer);
    }

    // wipe the FAT on the target
    sprintf (buffer, "sudo dd if=/dev/zero of=%s bs=512 count=1", dst_dev);
    system (buffer);
    
    // prepare temp mount points
    get_string ("mktemp -d", src_mnt);
    get_string ("mktemp -d", dst_mnt);
    //printf ("srcmnt = %s dstmnt = %s\n", src_mnt, dst_mnt);
    
    // prepare the new FAT
    sprintf (buffer, "sudo parted %s mklabel msdos", dst_dev);
    system (buffer);
    
    // read in the source partition table
    gtk_label_set_text (GTK_LABEL (status), _("Reading partitions..."));
    n = 0;
    while (1)
    {
        sprintf (buffer, "sudo parted %s unit s print | tail -n +%d | head -n 1", src_dev, n + 8);
        fp = popen (buffer, "r");
        if (fp == NULL) break;
        if (fgets (buffer, sizeof (buffer) - 1, fp) != NULL)
        {
            if (buffer[0] == 0x0A) break;
            //printf ("%s", buffer);
            sscanf (buffer, "%d %lds %lds %*lds %s %s %s", &(parts[n].pnum), &(parts[n].start), &(parts[n].end), &(parts[n].ptype), &(parts[n].ftype), &(parts[n].flags));
            //printf ("%d %ld %ld %s %s %s\n", parts[n].pnum, parts[n].start, parts[n].end, parts[n].ptype, parts[n].ftype, parts[n].flags);
        }
        else break;
        n++;
    }
    
    //printf ("Partition table read - %d partitions found\n", n);
    
    // recreate the partitions on the target
    for (p = 0; p < n; p++)
    {
        gtk_label_set_text (GTK_LABEL (status), _("Preparing partitions..."));
	
	    // create the partition
	    if (!strcmp (parts[p].ptype, "extended"))
	    {
		    sprintf (buffer, "sudo parted -s %s -- mkpart extended %lds -1s", dst_dev, parts[p].start);
		}
	    else
	    {
		    if (p == (n - 1))
			    sprintf (buffer, "sudo parted -s %s -- mkpart %s %s %lds -1s", dst_dev, parts[p].ptype, parts[p].ftype, parts[p].start);
		    else
			    sprintf (buffer, "sudo parted -s %s mkpart %s %s %lds %lds", dst_dev, parts[p].ptype, parts[p].ftype, parts[p].start, parts[p].end);
		}
		system (buffer);
		
		// refresh the kernel partion table
		system ("sudo partprobe");

		// create file systems
        if (!strncmp (parts[p].ftype, "fat", 3))
        {
            sprintf (buffer, "sudo mkfs.fat %s%d", dst_dev, parts[p].pnum);
            system (buffer);
        } 
        if (!strcmp (parts[p].ftype, "ext4"))
        {
            sprintf (buffer, "sudo mkfs.ext4 -F %s%d", dst_dev, parts[p].pnum);
            system (buffer);
        } 

        // set the flags        
        if (!strcmp (parts[p].flags, "lba"))
            sprintf (buffer, "sudo parted %s set %d lba on", dst_dev, parts[p].pnum);
        else
            sprintf (buffer, "sudo parted %s set %d lba off", dst_dev, parts[p].pnum);
        system (buffer);
        
        prog = p + 1;
        prog /= n;
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), prog);
    }
    
    // do the copy for each partition
    for (p = 0; p < n; p++)
    {
        sprintf (buffer, _("Copying partition %d of %d..."), p + 1, n);
 		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);
		gtk_label_set_text (GTK_LABEL (status), buffer);
	
		// mount partitions
		sprintf (buffer, "sudo mount %s%d %s", dst_dev, parts[p].pnum, dst_mnt);
		system (buffer);
		if (!strcmp (src_dev, "/dev/mmcblk0"))
		    sprintf (buffer, "sudo mount %sp%d %s", src_dev, parts[p].pnum, src_mnt);
		else
		    sprintf (buffer, "sudo mount %s%d %s", src_dev, parts[p].pnum, src_mnt);
		system (buffer);
 		
		// start the copy itself in a new thread
		copying = 1;
		g_thread_new (NULL, copy_thread, NULL);	
		
		// wait for the copy to complete, while updating the progress bar...
		sprintf (buffer, "sudo du -s %s", src_mnt);
		get_string (buffer, res);
		sscanf (res, "%ld", &srcsz);
		sprintf (buffer, "sudo du -s %s", dst_mnt);
		while (copying) //!!! check the cancel button in here somehow?
		{
			get_string (buffer, res);
			sscanf (res, "%ld", &dstsz);
			prog = dstsz;
			prog /= srcsz;
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), prog);
			//printf ("%ld %ld %f\n", dstsz, srcsz, prog);
			sleep (10);
		}
			
		// unmount partitions
		sprintf (buffer, "sudo umount %s", dst_mnt);
		system (buffer);
		sprintf (buffer, "sudo umount %s", src_mnt);
		system (buffer);
    }

    backup_thread_msg (_("Copy complete"));
    return NULL;
}

static void on_cancel (void)
{
    g_idle_add (close_msg, NULL);
}

static void on_start (void)
{
    // set up source and target devices
    strcpy (dst_dev, gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)));

    if (!strcmp (_("Internal SD card"), gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb))))
        sprintf (src_dev, "/dev/mmcblk0");
    else
        strcpy (src_dev, gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb)));  

    // don't do anything if src == dest
    if (!strcmp (src_dev, dst_dev)) return;

    // show the progress dialog
    msg_dlg = (GtkWidget *) gtk_dialog_new ();
    gtk_window_set_title (GTK_WINDOW (msg_dlg), "");
    gtk_window_set_modal (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_decorated (GTK_WINDOW (msg_dlg), FALSE);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (msg_dlg), TRUE);
    gtk_window_set_transient_for (GTK_WINDOW (msg_dlg), GTK_WINDOW (main_dlg));
    GtkWidget *frame = gtk_frame_new (NULL);
    gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (msg_dlg))), frame);
        
    GtkWidget *box = (GtkWidget *) gtk_vbox_new (TRUE, 5);
    gtk_container_set_border_width (GTK_CONTAINER (box), 10);
    gtk_container_add (GTK_CONTAINER (frame), box);
        
    status = (GtkWidget *) gtk_label_new (_("Checking source..."));
    gtk_box_pack_start (GTK_BOX (box), status, FALSE, FALSE, 5);
    progress = (GtkWidget *) gtk_progress_bar_new ();
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);
    gtk_box_pack_start (GTK_BOX (box), progress, FALSE, FALSE, 5);
    cancel = (GtkWidget *) gtk_button_new_with_label (_("Cancel"));
    gtk_box_pack_start (GTK_BOX (box), cancel, FALSE, FALSE, 5);
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), NULL);
    
    gtk_widget_show_all (GTK_WIDGET (msg_dlg));

    // launch a thread with the system call to run the backup
    g_thread_new (NULL, backup_thread, NULL);
}

static void on_cb_changed (void)
{
    if (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)) == 0 ||
        gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb)) == 0 ||
        !strcmp (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (to_cb)), gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (from_cb))))
        gtk_widget_set_sensitive (GTK_WIDGET (start_btn), FALSE);
    else
        gtk_widget_set_sensitive (GTK_WIDGET (start_btn), TRUE);
}

/* The dialog... */

int main (int argc, char *argv[])
{
	GtkBuilder *builder;
	DIR *dip;
	struct dirent *dit;
	int found = 0;
	char buffer[32];

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

	GtkWidget *table = (GtkWidget *) gtk_builder_get_object (builder, "table1");

	from_cb = (GtkWidget *)  (GObject *) gtk_combo_box_text_new ();
	gtk_table_attach (GTK_TABLE (table), GTK_WIDGET (from_cb), 1, 2, 0, 1, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);
	gtk_widget_show_all (GTK_WIDGET (from_cb));
	gtk_combo_box_append_text (GTK_COMBO_BOX (from_cb), _("Internal SD card"));
	if (dip = opendir ("/sys/block"))
	{
	    while (dit = readdir (dip))
        {
            if (!strncmp (dit->d_name, "sd", 2))
            {
                // might want to do something with g_drive_get_name here at some point...
                sprintf (buffer, "/dev/%s",  dit->d_name);
                gtk_combo_box_append_text (GTK_COMBO_BOX (from_cb), buffer);
                found = 1;
            }
        }
	    closedir (dip);
	}
	gtk_combo_box_set_active (GTK_COMBO_BOX (from_cb), 0);
	g_signal_connect (from_cb, "changed", G_CALLBACK (on_cb_changed), NULL);

	to_cb = (GtkWidget *)  (GObject *) gtk_combo_box_text_new ();
	gtk_table_attach (GTK_TABLE (table), GTK_WIDGET (to_cb), 1, 2, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);
	gtk_widget_show_all (GTK_WIDGET (to_cb));

	if (dip = opendir ("/sys/block"))
	{
	    while (dit = readdir (dip))
        {
            if (!strncmp (dit->d_name, "sd", 2))
            {
                // might want to do something with g_drive_get_name here at some point...
                sprintf (buffer, "/dev/%s",  dit->d_name);
                gtk_combo_box_append_text (GTK_COMBO_BOX (to_cb), buffer);
                found = 1;
            }
        }
	    closedir (dip);
	}
	if (found) gtk_combo_box_set_active (GTK_COMBO_BOX (to_cb), 0);
	g_signal_connect (to_cb, "changed", G_CALLBACK (on_cb_changed), NULL);

	start_btn = (GtkWidget *) gtk_builder_get_object (builder, "button1");
	g_signal_connect (start_btn, "clicked", G_CALLBACK (on_start), NULL);
	on_cb_changed ();
	
	g_object_unref (builder);

	gtk_dialog_run (GTK_DIALOG (main_dlg));
	gtk_widget_destroy (main_dlg);

	return 0;
}
