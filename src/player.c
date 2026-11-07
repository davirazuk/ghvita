#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "player.h"
#include "plat.h"

static char names[PLAYER_MAX][PLAYER_NAME];
static int colors[PLAYER_MAX];
static int count, current;
static char dir[400];

static void save(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%splayers.txt", plat_data_dir());
    FILE * f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d\n", current);
    for (int i = 0; i < count; i++)
        fprintf(f, "%s\t%d\n", names[i], colors[i]);
    fclose(f);
}

static void set_dir(void)
{
    if (current == 0) {
        snprintf(dir, sizeof(dir), "%s", plat_data_dir());
        return;
    }
    char base[400];
    snprintf(base, sizeof(base), "%splayers", plat_data_dir());
    mkdir(base, 0777);
    // the folder by number: a name may have anything in it
    snprintf(dir, sizeof(dir), "%s/%d/", base, current);
    char d[400];
    snprintf(d, sizeof(d), "%s/%d", base, current);
    mkdir(d, 0777);
}

void player_init(void)
{
    count = current = 0;
    char path[400], line[64];
    snprintf(path, sizeof(path), "%splayers.txt", plat_data_dir());
    FILE * f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            current = atoi(line);
        while (count < PLAYER_MAX && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char * tab = strchr(line, '\t');
            int c = count % PLAYER_COLORS;
            if (tab) {
                *tab = 0;
                c = atoi(tab + 1) % PLAYER_COLORS;
            }
            if (line[0]) {
                colors[count] = c;
                snprintf(names[count++], PLAYER_NAME, "%s", line);
            }
        }
        fclose(f);
    }
    if (!count)
        snprintf(names[count++], PLAYER_NAME, "Player 1");
    if (current < 0 || current >= count)
        current = 0;
    set_dir();
}

const char * player_dir(void)
{
    if (!dir[0])
        player_init();
    return dir;
}

int player_count(void)
{
    return count;
}

int player_current(void)
{
    return current;
}

const char * player_name(int i)
{
    return i >= 0 && i < count ? names[i] : "";
}

void player_select(int i)
{
    if (i < 0 || i >= count)
        return;
    current = i;
    set_dir();
    save();
}

int player_add(const char * name)
{
    while (*name == ' ')
        name++;
    if (count >= PLAYER_MAX || !*name)
        return -1;
    for (int i = 0; i < count; i++)
        if (!strcasecmp(names[i], name))
            return -1;
    snprintf(names[count], PLAYER_NAME, "%s", name);
    colors[count] = count % PLAYER_COLORS;
    // a name typed on the Vita's keyboard is lower case: it reads better capitalised
    if (names[count][0] >= 'a' && names[count][0] <= 'z')
        names[count][0] -= 32;
    count++;
    save();
    return count - 1;
}

int player_color(int i)
{
    return i >= 0 && i < count ? colors[i] : 0;
}

void player_set_color(int i, int color)
{
    if (i < 0 || i >= count)
        return;
    colors[i] = (color % PLAYER_COLORS + PLAYER_COLORS) % PLAYER_COLORS;
    save();
}
