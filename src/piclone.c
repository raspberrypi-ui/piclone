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
static GtkWidget *main_dlg, *msg_dlg, *status, *progress, *cancel, *dev_cb, *start_btn;

/* mount points */
char src_mnt[32], dst_mnt[32];

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
	char buffer[256];
	sprintf (buffer, "sudo cp -ax %s/. %s/.", src_mnt, dst_mnt);
	system (buffer);
	copying = 0;
	return NULL;
}

static gpointer backup_thread (gpointer data)
{
    char buffer[256], res[256];
    int n, p;
    long srcsz, dstsz;
    double prog;
    FILE *fp;

    gtk_label_set_text (GTK_LABEL (status), _("Preparing card..."));

    // unmount any partitions on the target device
    for (n = 9; n >= 1; n--)
    {
        sprintf (buffer, "sudo umount %s%d", (char *) data, n);
        system (buffer);
    }

    // wipe the FAT on the target
    sprintf (buffer, "sudo dd if=/dev/zero of=%s bs=512 count=1", (char *) data);
    system (buffer);
    
    // prepare temp mount points
    get_string ("mktemp -d", src_mnt);
    get_string ("mktemp -d", dst_mnt);
    //printf ("srcmnt = %s dstmnt = %s\n", src_mnt, dst_mnt);
    
    // prepare the new FAT
    sprintf (buffer, "sudo parted %s mklabel msdos", (char *) data);
    system (buffer);
    
    // read in the source partition table
    gtk_label_set_text (GTK_LABEL (status), _("Reading partitions..."));
    n = 0;
    while (1)
    {
        sprintf (buffer, "sudo parted /dev/mmcblk0 unit s print | tail -n +%d | head -n 1", n + 8);  
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
		    sprintf (buffer, "sudo parted -s %s -- mkpart extended %lds -1s", (char *) data, parts[p].start);
		}
	    else
	    {
		    if (p == (n - 1))
			    sprintf (buffer, "sudo parted -s %s -- mkpart %s %s %lds -1s", (char *) data, parts[p].ptype, parts[p].ftype, parts[p].start);
		    else
			    sprintf (buffer, "sudo parted -s %s mkpart %s %s %lds %lds", (char *) data, parts[p].ptype, parts[p].ftype, parts[p].start, parts[p].end);
		}
		system (buffer);
		
		// refresh the kernel partion table
		system ("sudo partprobe");

		// create file systems
        if (!strncmp (parts[p].ftype, "fat", 3))
        {
            sprintf (buffer, "sudo mkfs.fat %s%d", (char *) data, parts[p].pnum);
            system (buffer);
        } 
        if (!strcmp (parts[p].ftype, "ext4"))
        {
            sprintf (buffer, "sudo mkfs.ext4 -F %s%d", (char *) data, parts[p].pnum);
            system (buffer);
        } 

        // set the flags        
        if (!strcmp (parts[p].flags, "lba"))
            sprintf (buffer, "sudo parted %s set %d lba on", (char *) data, parts[p].pnum);
        else
            sprintf (buffer, "sudo parted %s set %d lba off", (char *) data, parts[p].pnum);
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
		sprintf (buffer, "sudo mount %s%d %s", (char *) data, parts[p].pnum, dst_mnt);
		system (buffer);
		sprintf (buffer, "sudo mount /dev/mmcblk0p%d %s", parts[p].pnum, src_mnt);
		system (buffer);
 		
		//sudo cp -axv $src/. $dst/.
		copying = 1;
		g_thread_new (NULL, copy_thread, NULL);	
		
		// wait for the copy to complete, while updating the progress bar...
		sprintf (buffer, "sudo du -s %s", src_mnt);
		get_string (buffer, res);
		sscanf (res, "%ld", &srcsz);
		while (copying) //!!! check the cancel button in here....
		{
			sprintf (buffer, "sudo du -s %s", dst_mnt);
			get_string (buffer, res);
			sscanf (res, "%ld", &dstsz);
			prog = dstsz;
			prog /= srcsz;
			gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), prog);
			printf ("%ld %ld %f\n", dstsz, srcsz, prog);
			if (dstsz >= srcsz) break;
			sleep (5);
		}
			
		// unmount partitions
		sprintf (buffer, "sudo umount %s", dst_mnt);
		system (buffer);
		sprintf (buffer, "sudo umount %s", src_mnt);
		system (buffer);
   }
    g_idle_add (close_msg, NULL);
    return NULL;
}

static void on_cancel ()
{
    g_idle_add (close_msg, NULL);
}

static void on_start ()
{
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
        
    status = (GtkWidget *) gtk_label_new (_("Copying partition 1..."));
    gtk_box_pack_start (GTK_BOX (box), status, FALSE, FALSE, 5);
    progress = (GtkWidget *) gtk_progress_bar_new ();
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progress), 0.0);
    gtk_box_pack_start (GTK_BOX (box), progress, FALSE, FALSE, 5);
    cancel = (GtkWidget *) gtk_button_new_with_label (_("Cancel"));
    gtk_box_pack_start (GTK_BOX (box), cancel, FALSE, FALSE, 5);
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), NULL);
    
    gtk_widget_show_all (GTK_WIDGET (msg_dlg));

    // launch a thread with the system call to run the backup
    g_thread_new (NULL, backup_thread, gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (dev_cb)));
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

	//dev_cb = (GtkWidget *) gtk_builder_get_object (builder, "comboboxtext1");



	GtkWidget *table = (GtkWidget *) gtk_builder_get_object (builder, "table1");
	dev_cb = (GtkWidget *)  (GObject *) gtk_combo_box_text_new ();
	gtk_table_attach (GTK_TABLE (table), GTK_WIDGET (dev_cb), 1, 2, 1, 2, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);
	gtk_widget_show_all (GTK_WIDGET (dev_cb));




	
	if (dip = opendir ("/sys/block"))
	{
	    while (dit = readdir (dip))
        {
            if (!strncmp (dit->d_name, "sd", 2))
            {
                // might want to do something with g_drive_get_name here at some point...
                sprintf (buffer, "/dev/%s",  dit->d_name);
                gtk_combo_box_append_text (GTK_COMBO_BOX (dev_cb), buffer);
                found = 1;
            }
        }	
	    closedir (dip);
	}
	if (found) gtk_combo_box_set_active (GTK_COMBO_BOX (dev_cb), 0);

	start_btn = (GtkWidget *) gtk_builder_get_object (builder, "button1");
	g_signal_connect (start_btn, "clicked", G_CALLBACK (on_start), NULL);
	
	g_object_unref (builder);

	gtk_dialog_run (GTK_DIALOG (main_dlg));
	gtk_widget_destroy (main_dlg);

	return 0;
}
