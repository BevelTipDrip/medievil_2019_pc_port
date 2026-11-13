/* src/pc_settings_hook.h: the build check of PostEventAtLocationWithMotion's slot and the pause menu's click site
 * (synthetic image and bytecode laid out as in both builds). Build: build.sh --test. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pc_settings_hook.h"

static unsigned char *image_for(const MedievilPcSettingsSites *s, uint64_t size) {
    unsigned char *image = calloc(1, size);
    assert(image);
    const uint64_t exec = (uint64_t)(uintptr_t)(image + s->exec), name = (uint64_t)(uintptr_t)(image + s->name);
    memcpy(image + s->slot, &exec, 8);
    memcpy(image + s->slot - 8, &name, 8);
    memcpy(image + s->name, "PostEventAtLocationWithMotion", 30);
    memcpy(image + s->exec, medievil_pc_settings_prologue, sizeof(medievil_pc_settings_prologue));
    return image;
}

/* The ubergraph's bytes the check reads, with the jump pointing at `target`. */
static unsigned char *script_for(const MedievilPcSettingsSites *s, uint32_t target) {
    unsigned char *script = calloc(1, (size_t)s->script_size);
    assert(script);
    script[s->let_at] = 0x0f;
    script[s->let_at + 0x12] = 0x19;
    script[s->call_at] = 0x1c;
    script[s->call_at + 9] = 0x20;
    script[s->jump_at] = 0x07;
    memcpy(script + s->jump_at + 1, &target, 4);
    script[s->jump_at + 5] = 0x01;
    script[s->pop_at] = 0x4d;
    return script;
}

int main(void) {
    const uint64_t size = 0x5000000;
    for (int b = 0; b < 2; ++b) {
        const MedievilPcSettingsSites *s = &medievil_pc_settings_builds[b];
        unsigned char *image = image_for(s, size);
        assert(medievil_pc_settings_find(image, size) == s);
        image[s->exec + 3] ^= 1; /* another prologue */
        assert(!medievil_pc_settings_find(image, size));
        image[s->exec + 3] ^= 1;
        image[s->name] = 'X'; /* another native's name */
        assert(!medievil_pc_settings_find(image, size));
        image[s->name] = 'P';
        assert(!medievil_pc_settings_find(image, s->slot)); /* image too small */
        free(image);

        unsigned char *edited = script_for(s, s->pop_at);
        const unsigned char *code = edited + s->call_at + 9;
        assert(medievil_pc_settings_call(s, edited, s->script_size, code) == MEDIEVIL_PC_SETTINGS_CLICK);
        assert(medievil_pc_settings_call(s, edited, s->script_size + 1, code) == MEDIEVIL_PC_SETTINGS_OTHER);
        assert(medievil_pc_settings_call(s, edited, s->script_size, code + 1) == MEDIEVIL_PC_SETTINGS_OTHER);
        assert(medievil_pc_settings_call(s, NULL, s->script_size, code) == MEDIEVIL_PC_SETTINGS_OTHER);
        edited[s->let_at + 0x12] = 0x1b; /* another statement there */
        assert(medievil_pc_settings_call(s, edited, s->script_size, code) == MEDIEVIL_PC_SETTINGS_OTHER);
        free(edited);
        unsigned char *original = script_for(s, s->pop_at + 1); /* no pak edit: the game's handler */
        assert(medievil_pc_settings_call(s, original, s->script_size, original + s->call_at + 9) ==
               MEDIEVIL_PC_SETTINGS_ORIGINAL);
        free(original);
        unsigned char *other = script_for(s, 0x10); /* some other jump */
        assert(medievil_pc_settings_call(s, other, s->script_size, other + s->call_at + 9) ==
               MEDIEVIL_PC_SETTINGS_OTHER);
        free(other);
    }
    /* The two builds' sites differ (one table entry each). */
    assert(medievil_pc_settings_builds[0].slot != medievil_pc_settings_builds[1].slot);
    printf("pc settings hook: ok\n");
    return 0;
}
