/* GOW3_PAD_MAP / GOW3_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

int gow3gpu_overlay_captures_input(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}
/* The rest of the runtime runtime_pad.c calls (the GPU library, the mouse reader, the UE4 poll). */
void gow3gpu_choice_set_input(uint32_t (*buttons)(void)) { (void)buttons; }
void gow3gpu_menu_set_input(uint32_t (*buttons)(void)) { (void)buttons; }
void runtime_ue4_setres_poll(void) {}
int runtime_mouse_enabled(void) { return 1; }
uint32_t runtime_mouse_buttons(void) { return 0; }

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* Swap: Q presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys," cross = Q , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Q)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_E)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; both touchpad sources go when touchpad is remapped. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==0);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */

    /* "none" (the launcher's unbound box) takes the default away and adds nothing, keys and gamepad. */
    ButtonMap unbound=key_map;
    assert(apply_button_map(&unbound,"cross=None,circle=none",MAP_KEYS)==2);
    assert(pressed_by(&unbound,SDL_SCANCODE_SPACE)==0 && pressed_by(&unbound,SDL_SCANCODE_LSHIFT)==0);
    assert(unbound.count==key_map.count-2 && pressed_by(&unbound,SDL_SCANCODE_E)==BTN_SQUARE);
    ButtonMap no_pad=pad_map;
    assert(apply_button_map(&no_pad,"cross=none",MAP_PAD)==1 && pressed_by(&no_pad,SDL_GAMEPAD_BUTTON_SOUTH)==0);
    ButtonMap mouse={{{0,0}},0};
    assert(apply_button_map(&mouse,"cross=none",MAP_MOUSE)==0 && mouse.count==0); /* mouse: no defaults to take */

    /* The settings menu's keys: the bound ones as mapped, the fixed ones while not bound to a menu button. */
    static bool k[SDL_SCANCODE_COUNT];
    k[SDL_SCANCODE_RETURN]=true; /* Options by default: Cross in the menu */
    assert(menu_key_buttons(k,&key_map)==BTN_CROSS);
    k[SDL_SCANCODE_RETURN]=false; k[SDL_SCANCODE_SPACE]=true; /* bound to Cross */
    assert(menu_key_buttons(k,&key_map)==BTN_CROSS);
    k[SDL_SCANCODE_SPACE]=false; k[SDL_SCANCODE_Q]=true; k[SDL_SCANCODE_E]=true; /* Q Triangle: L1; E Square: Square */
    assert(menu_key_buttons(k,&key_map)==(BTN_L1|BTN_SQUARE));
    k[SDL_SCANCODE_E]=false; k[SDL_SCANCODE_TAB]=true; /* the touchpad by default: Square here, as in the Load list */
    assert(menu_key_buttons(k,&key_map)==(BTN_L1|BTN_SQUARE));
    k[SDL_SCANCODE_Q]=k[SDL_SCANCODE_TAB]=false;
    ButtonMap tabs=key_map; /* E bound to nothing: the fixed R1 again */
    assert(apply_button_map(&tabs,"square=none",MAP_KEYS)==1);
    k[SDL_SCANCODE_E]=true;
    assert(menu_key_buttons(k,&tabs)==BTN_R1);
    k[SDL_SCANCODE_E]=false; k[SDL_SCANCODE_I]=true; k[SDL_SCANCODE_LEFT]=true; /* D-pad up, arrow */
    assert(menu_key_buttons(k,&key_map)==(BTN_UP|BTN_LEFT));
    k[SDL_SCANCODE_I]=k[SDL_SCANCODE_LEFT]=false; k[SDL_SCANCODE_R]=true; /* L2: not a menu button */
    assert(menu_key_buttons(k,&key_map)==0);
    k[SDL_SCANCODE_R]=false;
    ButtonMap bound=key_map; /* Enter bound to Circle: only Circle (no Cross as well) */
    assert(apply_button_map(&bound,"circle=Return",MAP_KEYS)==1);
    k[SDL_SCANCODE_RETURN]=true;
    assert(menu_key_buttons(k,&bound)==BTN_CIRCLE);
    k[SDL_SCANCODE_RETURN]=false; k[SDL_SCANCODE_ESCAPE]=true;
    assert(menu_key_buttons(k,&bound)==BTN_CIRCLE);
    k[SDL_SCANCODE_ESCAPE]=false;

    /* Mouse binds in the menu: Circle / L1 / R1 only, never from the left button. */
    assert(apply_button_map(&mouse_map,"circle=left,l1=right,cross=middle,r1=x1,square=x2",MAP_MOUSE)==5);
    assert(menu_mouse_buttons(1u)==0);       /* left: the pointer's */
    assert(menu_mouse_buttons(2u)==BTN_L1);  /* right */
    assert(menu_mouse_buttons(4u)==0);       /* middle: Cross, not taken */
    assert(menu_mouse_buttons(1u|8u)==BTN_R1);
    assert(menu_mouse_buttons(16u)==BTN_SQUARE); /* x2: APPLY & RESTART */
    puts("PASS: pad and key maps");
}
