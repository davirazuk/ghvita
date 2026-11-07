#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "player.h"
#include "plat.h"
#include "dl.h"

static struct {
    char name[PLAYER_NAME];
    int color, id;              // id: the folder's number, 0 the data folder itself
} players[PLAYER_MAX];
static int count, current, pic_version;
static char dir[400];

// players.txt: the current one's index, then "name<TAB>colour<TAB>id" a line each
static void save(void)
{
    char path[400];
    snprintf(path, sizeof(path), "%splayers.txt", plat_data_dir());
    FILE * f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d\n", current);
    for (int i = 0; i < count; i++)
        fprintf(f, "%s\t%d\t%d\n", players[i].name, players[i].color, players[i].id);
    fclose(f);
}

void player_dir_of(int i, char * out, int size)
{
    int id = i >= 0 && i < count ? players[i].id : 0;
    if (id == 0)
        snprintf(out, size, "%s", plat_data_dir());
    else
        snprintf(out, size, "%splayers/%d/", plat_data_dir(), id);
}

static void set_dir(void)
{
    player_dir_of(current, dir, sizeof(dir));
    if (players[current].id) {
        char d[400];
        snprintf(d, sizeof(d), "%splayers", plat_data_dir());
        mkdir(d, 0777);
        snprintf(d, sizeof(d), "%splayers/%d", plat_data_dir(), players[current].id);
        mkdir(d, 0777);
    }
}

void player_init(void)
{
    count = current = 0;
    char path[400], line[96];
    snprintf(path, sizeof(path), "%splayers.txt", plat_data_dir());
    FILE * f = fopen(path, "r");
    if (f) {
        if (fgets(line, sizeof(line), f))
            current = atoi(line);
        while (count < PLAYER_MAX && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            // older lines: "name" or "name<TAB>colour", the folder then by position
            char * t1 = strchr(line, '\t'), * t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            int color = count % PLAYER_COLORS, id = count;
            if (t1) {
                *t1 = 0;
                color = atoi(t1 + 1) % PLAYER_COLORS;
            }
            if (t2)
                id = atoi(t2 + 1);
            if (!line[0])
                continue;
            snprintf(players[count].name, PLAYER_NAME, "%s", line);
            players[count].color = color;
            players[count].id = id;
            count++;
        }
        fclose(f);
    }
    if (!count) {
        snprintf(players[0].name, PLAYER_NAME, "Player 1");
        players[0].color = players[0].id = 0;
        count = 1;
    }
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
    return i >= 0 && i < count ? players[i].name : "";
}

void player_select(int i)
{
    if (i < 0 || i >= count)
        return;
    current = i;
    set_dir();
    save();
}

static bool name_ok(const char * name, int except)
{
    if (!*name)
        return false;
    for (int i = 0; i < count; i++)
        if (i != except && !strcasecmp(players[i].name, name))
            return false;
    return true;
}

static void tidy_name(char * out, const char * in)
{
    while (*in == ' ')
        in++;
    snprintf(out, PLAYER_NAME, "%s", in);
    // a name typed on the Vita's keyboard is lower case: it reads better capitalised
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] -= 32;
}

int player_add(const char * name)
{
    char n[PLAYER_NAME];
    tidy_name(n, name);
    if (count >= PLAYER_MAX || !name_ok(n, -1))
        return -1;
    int id = 1;
    for (int i = 0; i < count; i++)
        if (players[i].id >= id)
            id = players[i].id + 1;
    snprintf(players[count].name, PLAYER_NAME, "%s", n);
    players[count].color = count % PLAYER_COLORS;
    players[count].id = id;
    count++;
    save();
    return count - 1;
}

bool player_rename(int i, const char * name)
{
    char n[PLAYER_NAME];
    tidy_name(n, name);
    if (i < 0 || i >= count || !name_ok(n, i))
        return false;
    snprintf(players[i].name, PLAYER_NAME, "%s", n);
    save();
    return true;
}

bool player_delete(int i)
{
    if (i < 0 || i >= count || count <= 1)
        return false;
    char d[400];
    player_dir_of(i, d, sizeof(d));
    if (players[i].id) {
        d[strlen(d) - 1] = 0;
        dl_remove_tree(d);
    } else {
        // the first player's files sit in the data folder with everyone's: only theirs go
        static const char * const mine[] = {"scores.txt", "favourites.txt", "settings.txt", "career.txt", "tour.txt",
                                            "daily.txt", "achievements.txt", "avatar.png", "avatar.jpg", NULL};
        char p[460];
        for (int k = 0; mine[k]; k++) {
            snprintf(p, sizeof(p), "%s%s", d, mine[k]);
            remove(p);
        }
        snprintf(p, sizeof(p), "%sghosts", d);
        dl_remove_tree(p);
    }
    memmove(&players[i], &players[i + 1], (count - i - 1) * sizeof(players[0]));
    count--;
    if (current > i || current >= count)
        current = current > 0 ? current - 1 : 0;
    set_dir();
    save();
    pic_version++;
    return true;
}

int player_color(int i)
{
    return i >= 0 && i < count ? players[i].color : 0;
}

void player_set_color(int i, int color)
{
    if (i < 0 || i >= count)
        return;
    players[i].color = (color % PLAYER_COLORS + PLAYER_COLORS) % PLAYER_COLORS;
    save();
}

void player_picture(int i, char * out, int size)
{
    char d[400];
    player_dir_of(i, d, sizeof(d));
    static const char * const names[] = {"avatar.png", "avatar.jpg"};
    for (int k = 0; k < 2; k++) {
        snprintf(out, size, "%s%s", d, names[k]);
        struct stat st;
        if (stat(out, &st) == 0 && st.st_size > 0)
            return;
    }
    out[0] = 0;
}

void player_clear_picture(int i)
{
    char p[460];
    player_picture(i, p, sizeof(p));
    while (p[0]) {
        remove(p);
        player_picture(i, p, sizeof(p));
    }
    pic_version++;
}

bool player_set_picture(int i, const unsigned char * data, int size)
{
    if (i < 0 || i >= count || size < 8)
        return false;
    bool png = !memcmp(data, "\x89PNG", 4), jpg = data[0] == 0xFF && data[1] == 0xD8;
    if (!png && !jpg)
        return false;
    player_clear_picture(i);
    char d[400], p[460];
    // their folder may not be there yet (it's made when they first play)
    snprintf(d, sizeof(d), "%splayers", plat_data_dir());
    mkdir(d, 0777);
    player_dir_of(i, d, sizeof(d));
    d[strlen(d) - 1] = 0;
    mkdir(d, 0777);
    strcat(d, "/");
    snprintf(p, sizeof(p), "%savatar.%s", d, png ? "png" : "jpg");
    FILE * f = fopen(p, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, size, f) == (size_t)size;
    fclose(f);
    pic_version++;
    return ok;
}

int player_picture_version(void)
{
    return pic_version;
}
