/* MediEvil save details for the Load list: what a save holds (levels, chalices, play time) and
 * the level names and loading-screen pictures the launcher extracted from the game's paks
 * (scripts/game_ui_assets.py: <user>/cache/game_ui/levels.tsv, levels/level_NN.png).
 *
 * A save (ue4savegame.ps4.sav) is UE4 GVAS: tagged properties, each "name, type, int64 size,
 * value". The scanner walks it byte by byte and reads the properties it knows; structs, arrays and
 * maps are entered, not skipped, so nested values are seen in file order. LevelData is a map from
 * "LevelsEnum::NewEnumeratorN" (written as a plain string) to a struct of Unlocked / Completed /
 * ChaliceCollected / LifeBottleCollected; BestiaryData is an array of structs with Unlocked.
 * Property names carry a "_N_<GUID>" suffix from the Blueprint (ChaliceCollected_9_A20C...). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { REGION_NONE, REGION_LEVELS, REGION_BESTIARY };

static int fstring(const unsigned char *b, size_t n, size_t at, const char **text, size_t *next) {
    if (at+4>n) return 0;
    int32_t len; memcpy(&len,b+at,4);
    if (len<2 || len>256 || at+4+(size_t)len>n || b[at+3+len]) return 0;
    for (int32_t i=0;i<len-1;++i) if (b[at+4+i]<32 || b[at+4+i]>126) return 0;
    *text=(const char *)b+at+4; *next=at+4+(size_t)len;
    return 1;
}
/* "Gold" matches "Gold" and "Gold_12_ABC...", not "GoldBag". */
static int named(const char *name, const char *want) {
    size_t k=strlen(want);
    return !strncmp(name,want,k) && (!name[k] || name[k]=='_');
}

int runtime_saveinfo_read(int32_t user, const char *dir, RuntimeSaveInfo *info) {
    memset(info,0,sizeof(*info));
    info->last_level=-1;
    char path[720];
    if (runtime_savedata_dir(user,dir,path,sizeof(path))) return -1;
    size_t used=strlen(path);
    snprintf(path+used,sizeof(path)-used,"/ue4savegame.ps4.sav");
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    unsigned char *b=NULL; size_t n=0, cap=0;
    for (;;) {
        if (n==cap) { cap=cap ? cap*2 : 65536; unsigned char *grown=realloc(b,cap); if (!grown) break; b=grown; }
        size_t got=fread(b+n,1,cap-n,f);
        if (!got) break;
        n+=got;
    }
    fclose(f);
    if (!b) return -1;
    int region=REGION_NONE, level=-1;
    for (size_t i=0;i<n;) {
        const char *name, *type; size_t at, p;
        if (!fstring(b,n,i,&name,&at)) { ++i; continue; }
        /* A map's or array's inner types ("EnumProperty", "StructProperty") are not properties. */
        if (strstr(name,"Property")) { i=at; continue; }
        if (!fstring(b,n,at,&type,&p) || !strstr(type,"Property") || p+8>n) {
            /* A plain string: inside LevelData, the map key of the next struct. */
            if (region==REGION_LEVELS && !strncmp(name,"LevelsEnum::NewEnumerator",25)) {
                level=atoi(name+25);
                if (level>=0 && level<RUNTIME_SAVE_LEVELS) info->levels_seen|=1u<<level; else level=-1;
            }
            i=at; continue;
        }
        const size_t v=p+8; /* after the int64 size */
        if (!strcmp(type,"BoolProperty") && v<n) {
            const int on=b[v]!=0;
            if (region==REGION_LEVELS && level>=0) {
                const uint32_t bit=1u<<level;
                if (named(name,"Unlocked")) { if (on) info->unlocked|=bit; }
                else if (named(name,"Completed")) { if (on) info->completed|=bit; }
                else if (named(name,"ChaliceCollected")) { if (on) info->chalices|=bit; }
                /* LifeBottleCollected is 0 in every save seen, with 5 bottles owned: not shown. */
                else if (!named(name,"LifeBottleCollected") && !named(name,"RevealedOnMap")) region=REGION_NONE;
            } else if (region==REGION_BESTIARY && named(name,"Unlocked")) {
                info->bestiary+=on;
            } else if (region==REGION_BESTIARY && named(name,"New")) {
            } else {
                region=REGION_NONE;
            }
            i=v+2; continue;
        }
        if ((!strcmp(type,"IntProperty") || !strcmp(type,"FloatProperty")) && v+5<=n) {
            int32_t whole; float real; memcpy(&whole,b+v+1,4); memcpy(&real,b+v+1,4);
            if (named(name,"Gold")) info->gold=whole;
            /* "HealthVials" is the life bottle count (the user, 2026-11-10: 5 owned, the save says 5). */
            else if (named(name,"HealthVials")) info->life_bottles=whole;
            else if (named(name,"PlayTime") && !info->play_seconds) info->play_seconds=real;
            region=REGION_NONE;
            i=v+5; continue;
        }
        /* An enum value: EnumProperty, or (4.21, LastLevelPlayed) a ByteProperty with an enum name. */
        if (!strcmp(type,"EnumProperty") || !strcmp(type,"ByteProperty")) {
            const char *enum_name, *value; size_t q, r;
            if (fstring(b,n,v,&enum_name,&q) && strcmp(enum_name,"None") && fstring(b,n,q+1,&value,&r)) {
                if (named(name,"LastLevelPlayed") && !strncmp(value,"LevelsEnum::NewEnumerator",25))
                    info->last_level=atoi(value+25);
                region=REGION_NONE;
                i=r; continue;
            }
        }
        /* Containers are entered: their contents follow in the file. */
        region=named(name,"LevelData") ? REGION_LEVELS : named(name,"BestiaryData") ? REGION_BESTIARY : REGION_NONE;
        level=-1;
        i=v;
    }
    free(b);
    info->found=1;
    return 0;
}

/* Level names from the launcher's cache, index = LevelsEnum number. */
static char level_names[RUNTIME_SAVE_LEVELS][64];
static int level_names_read;
static void read_level_names(void) {
    if (level_names_read) return;
    level_names_read=1;
    char path[720];
    snprintf(path,sizeof(path),"%s/cache/game_ui/levels.tsv",runtime_file_user_dir());
    FILE *f=fopen(path,"rb");
    if (!f) return;
    char line[256];
    while (fgets(line,sizeof(line),f)) {
        char *code=strchr(line,'\t'), *name=code ? strchr(code+1,'\t') : NULL;
        if (!name) continue;
        const int index=atoi(line);
        name[strcspn(name+1,"\r\n")+1]=0;
        if (index>=0 && index<RUNTIME_SAVE_LEVELS) snprintf(level_names[index],sizeof(level_names[index]),"%s",name+1);
    }
    fclose(f);
}
const char *runtime_saveinfo_level_name(int index) {
    read_level_names();
    return index>=0 && index<RUNTIME_SAVE_LEVELS && level_names[index][0] ? level_names[index] : NULL;
}
int runtime_saveinfo_level_index(const char *name) {
    read_level_names();
    for (int i=0;name && name[0] && i<RUNTIME_SAVE_LEVELS;++i) if (!strcmp(level_names[i],name)) return i;
    return -1;
}
int runtime_saveinfo_picture(int index, char *out, size_t size) {
    if (index<0 || index>=RUNTIME_SAVE_LEVELS) return -1;
    snprintf(out,size,"%s/cache/game_ui/levels/level_%02d.png",runtime_file_user_dir(),index);
    FILE *f=fopen(out,"rb");
    if (!f) return -1;
    fclose(f);
    return 0;
}

static int bits(uint32_t v) { int c=0; for (;v;v&=v-1) ++c; return c; }
static void play_time(char *out, size_t size, float seconds) {
    const long minutes=(long)(seconds/60.0f);
    if (minutes>=60) snprintf(out,size,"%ldh %02ldm",minutes/60,minutes%60);
    else snprintf(out,size,"%ldm",minutes);
}
static void level_label(char *out, size_t size, int index) {
    const char *name=runtime_saveinfo_level_name(index);
    if (name) snprintf(out,size,"%s",name); else snprintf(out,size,"Level %d",index);
}

void runtime_saveinfo_text(const RuntimeSaveInfo *info, int level, char *line, size_t line_size,
                           char *details, size_t details_size, char *more, size_t more_size) {
    char time[32]; play_time(time,sizeof(time),info->play_seconds);
    const int open=bits(info->unlocked & ~info->completed);
    snprintf(line,line_size,"%d chalice%s, %d level%s done, %s",bits(info->chalices),bits(info->chalices)==1 ? "" : "s",
             bits(info->completed),bits(info->completed)==1 ? "" : "s",time);
    char where[64]="-", last[64]="-";
    if (level>=0) level_label(where,sizeof(where),level);
    if (info->last_level>=0) level_label(last,sizeof(last),info->last_level);
    snprintf(details,details_size,
             "Saved in: %s\nLast level played: %s\n\nChalices: %d\nLevels completed: %d (%d more open)\n"
             "Life bottles: %d\nGold: %d\nBestiary entries: %d\nPlay time: %s",
             where,last,bits(info->chalices),bits(info->completed),open,info->life_bottles,info->gold,
             info->bestiary,time);
    size_t used=(size_t)snprintf(more,more_size,"Levels in this save:\n");
    for (int i=0;i<RUNTIME_SAVE_LEVELS && used<more_size;++i) {
        const uint32_t bit=1u<<i;
        if (!(info->levels_seen & bit)) continue;
        char name[64]; level_label(name,sizeof(name),i);
        const char *state=info->completed & bit ? "completed" : info->unlocked & bit ? "open" : "found";
        used+=(size_t)snprintf(more+used,more_size-used,"\n%s: %s%s",name,state,info->chalices & bit ? ", chalice" : "");
    }
}
