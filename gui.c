// Minimal GTK keypad UI (digits + dot)
// Build: gcc gui.c -o gui $(pkg-config --cflags --libs gtk+-3.0)

#include <gtk/gtk.h>
#include <string.h>

static void append_digit(GtkButton *btn, gpointer user_data) {
    GtkEntry *entry = GTK_ENTRY(user_data);
    const char *txt = gtk_button_get_label(btn);
    const char *old = gtk_entry_get_text(entry);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", old, txt);
    gtk_entry_set_text(entry, buf);
}

static void clear_entry(GtkButton *btn, gpointer user_data) {
    (void)btn;
    gtk_entry_set_text(GTK_ENTRY(user_data), "");
}

static GtkWidget* build_keypad(GtkEntry *target) {
    GtkWidget *grid = gtk_grid_new();
    const char *labels[12] = { "1","2","3","4","5","6","7","8","9","0",".","CLR"};
    int row = 0, col = 0;
    for (int i = 0; i < 12; ++i) {
        GtkWidget *btn = gtk_button_new_with_label(labels[i]);
        if (strcmp(labels[i], "CLR") == 0) {
            g_signal_connect(btn, "clicked", G_CALLBACK(clear_entry), target);
        } else {
            g_signal_connect(btn, "clicked", G_CALLBACK(append_digit), target);
        }
        gtk_grid_attach(GTK_GRID(grid), btn, col, row, 1, 1);
        col++; if (col == 3) { col = 0; row++; }
    }
    return grid;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "CloudPhone Dialer");
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 480);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(win), box);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Eingabe");
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);

    GtkWidget *keypad = build_keypad(GTK_ENTRY(entry));
    gtk_box_pack_start(GTK_BOX(box), keypad, FALSE, FALSE, 4);

    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
