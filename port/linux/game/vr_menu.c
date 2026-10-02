/*
VR_MENU.C

The pause menu's VR settings, in the VR build (HALO_VR): the headset's
options changed in play, in the pause menu's own boxes, font and buttons.

Halo's menus are widget tags in each map. When a level's tags load, this
adds a "VR SETTINGS" item to the solo pause menu's list (its button hints
move from under the list to under the mission objectives, making room), and
a screen of its own, cloned from the pause menu: the same dimmed backdrop
and boxes, with a list of settings in two columns where the list and the
mission objectives were. Each setting is a button cloned from "RESUME
GAME": A or right steps it to its next value, left to its previous; B goes
back to the pause menu. The settings are written into config.toml as they
change, and the VR layer takes them up at once (vr_reload_settings).

The widget code calls back here for the text of these buttons (a game data
input function: VR_MENU_GAME_DATA_FUNCTION) and for their changes (event
handler functions: VR_MENU_NEXT_FUNCTION, VR_MENU_PREVIOUS_FUNCTION).
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/integer_math.h"
#include "math/real_math.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"

#include "halo_vr.h"
#include "../src/vr.h"
#include "../src/port_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src/platform.h (a variadic call needs its prototype in scope
on the Android guest's ABI) */
void platform_log(const char *format, ...);
/* source/cache/cache_files.c */
long cache_file_add_tag(unsigned long group_tag, unsigned long parent_group_tag, char *name, void *base_address);

/* ---------- the widget definition tag ('DeLa'), as ui_widget.c views it */

#define VR_MENU_WIDGET_TAG 0x44654C61 /* 'DeLa' */

struct vr_menu_event_handler
{
	long flags;
	short event_type;
	short function;
	struct tag_reference widget_tag;
	struct tag_reference sound_effect;
	char script[32];
};

struct vr_menu_child
{
	struct tag_reference widget_tag;
	char name[32];
	long flags;
	short custom_controller_index;
	short vertical_offset;
	short horizontal_offset;
	byte unknown03A[0x50 - 0x3A];
};

struct vr_menu_game_data_input
{
	short function;
	byte unknown002[0x24 - 0x02];
};

struct vr_menu_widget
{
	short type;
	short controller_index;
	char name[32];
	rectangle2d bounds;
	long flags;
	long milliseconds_to_auto_close;
	long auto_close_fade_time;
	struct tag_reference background_bitmap;
	struct tag_block game_data_inputs;
	struct tag_block event_handlers;
	struct tag_block search_and_replace_functions;
	byte unknown06C[0xEC - 0x6C];
	struct tag_reference text_label_string_list;
	byte unknown0FC[0x3E0 - 0xFC];
	struct tag_block child_widgets;
};

typedef char vr_menu_event_handler_size[sizeof(struct vr_menu_event_handler) == 0x48 ? 1 : -1];
typedef char vr_menu_child_size[sizeof(struct vr_menu_child) == 0x50 ? 1 : -1];
typedef char vr_menu_game_data_input_size[sizeof(struct vr_menu_game_data_input) == 0x24 ? 1 : -1];
typedef char vr_menu_widget_size[sizeof(struct vr_menu_widget) == 0x3EC ? 1 : -1];
typedef char vr_menu_widget_bounds[offsetof(struct vr_menu_widget, bounds) == 0x24 ? 1 : -1];
typedef char vr_menu_widget_game_data_inputs[offsetof(struct vr_menu_widget, game_data_inputs) == 0x48 ? 1 : -1];
typedef char vr_menu_widget_event_handlers[offsetof(struct vr_menu_widget, event_handlers) == 0x54 ? 1 : -1];
typedef char vr_menu_widget_text[offsetof(struct vr_menu_widget, text_label_string_list) == 0xEC ? 1 : -1];

/* event handler flags and events (ui_widget.c) */
#define VR_MENU_CLOSE_CURRENT 0x1
#define VR_MENU_CLOSE_ALL 0x4
#define VR_MENU_OPEN_WIDGET 0x8
#define VR_MENU_RUN_FUNCTION 0x80
#define VR_MENU_EVENT_A 0
#define VR_MENU_EVENT_B 1
#define VR_MENU_EVENT_DPAD_LEFT 10
#define VR_MENU_EVENT_DPAD_RIGHT 11
#define VR_MENU_EVENT_START 12
#define VR_MENU_EVENT_BACK 13

/* ---------- the settings */

enum
{
	_vr_setting_boolean,
	_vr_setting_real,
	_vr_setting_string,
};

static struct vr_menu_setting
{
	char const *label, *key;
	short type, value_count;
	struct
	{
		char const *label, *value;
	} values[3];
} const vr_menu_settings[] =
{
	/* the left box */
	{ "CONTROLS", "vr.controls", _vr_setting_string, 2, { { "VR", "vr" }, { "XBOX", "pad" } } },
	{ "AIM", "vr.aim", _vr_setting_string, 2, { { "HAND", "hand" }, { "HEAD", "head" } } },
	{ "GUN HAND", "vr.left_handed", _vr_setting_boolean, 2, { { "RIGHT", "false" }, { "LEFT", "true" } } },
	{ "TURNING", "vr.snap_turn", _vr_setting_real, 3, { { "SNAP 30", "30" }, { "SNAP 45", "45" }, { "SMOOTH", "0" } } },
	{ "ROOM-SCALE", "vr.roomscale", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	/* the right box */
	{ "SCOPE", "vr.scope", _vr_setting_boolean, 2, { { "ON", "true" }, { "OFF", "false" } } },
	{ "VEHICLES", "vr.vehicle_view", _vr_setting_string, 2, { { "INSIDE", "first_person" }, { "CHASE", "chase" } } },
	{ "STEERING", "vr.vehicle_steering", _vr_setting_string, 3, { { "STICK", "stick" }, { "HEAD", "head" }, { "HAND", "hand" } } },
	{ "CUTSCENES", "vr.cinema_3d", _vr_setting_boolean, 2, { { "3D", "true" }, { "FLAT", "false" } } },
};

#define VR_MENU_SETTING_COUNT ((long)NUMBEROF(vr_menu_settings))
/* the settings in the left box; the rest go in the right */
#define VR_MENU_LEFT_COUNT 5

/* the setting's value now, as an index into its values (NONE: none of them) */
static long vr_menu_value_index(
	struct vr_menu_setting const *setting)
{
	long index;

	for (index = 0; index < setting->value_count; index++)
	{
		char const *value = setting->values[index].value;

		switch (setting->type)
		{
		case _vr_setting_boolean:
			if (config_boolean(setting->key) == !strcmp(value, "true"))
				return index;
			break;
		case _vr_setting_real:
			if (fabs(config_real(setting->key) - atof(value)) < 0.5)
				return index;
			break;
		case _vr_setting_string:
			if (!strcmp(config_string(setting->key), value))
				return index;
			break;
		}
	}
	return NONE;
}

/* ---------- the tags */

static struct
{
	/* the tags this made for the map loaded, and what they point at */
	long button_tag_index, screen_tag_index, list_tag_index;
	long setting_tag_indices[NUMBEROF(vr_menu_settings)];
	void *allocations[64];
	long allocation_count;
} vr_menu;

static void *vr_menu_allocate(
	long size)
{
	void *memory;

	if (vr_menu.allocation_count >= (long)NUMBEROF(vr_menu.allocations))
		return NULL;
	memory = calloc(1, (size_t)size);
	if (memory)
		vr_menu.allocations[vr_menu.allocation_count++] = memory;
	return memory;
}

static struct vr_menu_widget *vr_menu_widget_get(
	long tag_index)
{
	return tag_index != NONE ? tag_get(VR_MENU_WIDGET_TAG, tag_index) : NULL;
}

static void vr_menu_reference(
	struct tag_reference *reference,
	long tag_index)
{
	reference->group_tag = VR_MENU_WIDGET_TAG;
	reference->name = tag_index != NONE ? tag_get_name(tag_index) : "";
	reference->name_length = (long)strlen(reference->name);
	reference->index = tag_index;
}

/* a widget cloned from `template_index`, named, added as a tag of its own */
static long vr_menu_clone(
	long template_index,
	char *tag_name,
	char const *widget_name,
	struct vr_menu_widget **out_widget)
{
	struct vr_menu_widget *widget = vr_menu_allocate(sizeof(*widget));
	long tag_index;

	*out_widget = NULL;
	if (!widget)
		return NONE;
	memcpy(widget, vr_menu_widget_get(template_index), sizeof(*widget));
	memset(widget->name, 0, sizeof(widget->name));
	strncpy(widget->name, widget_name, sizeof(widget->name) - 1);
	tag_index = cache_file_add_tag(VR_MENU_WIDGET_TAG, (unsigned long)NONE, tag_name, widget);
	if (tag_index != NONE)
		*out_widget = widget;
	return tag_index;
}

static boolean vr_menu_handlers(
	struct vr_menu_widget *widget,
	struct vr_menu_event_handler const *handlers,
	long count)
{
	struct vr_menu_event_handler *copy = vr_menu_allocate(count * (long)sizeof(*copy));

	if (!copy)
		return FALSE;
	memcpy(copy, handlers, (size_t)count * sizeof(*copy));
	widget->event_handlers.count = count;
	widget->event_handlers.address = copy;
	return TRUE;
}

/* a button cloned from the pause menu's "RESUME GAME", its text from code */
static long vr_menu_button(
	long template_index,
	char *tag_name,
	char const *widget_name,
	struct vr_menu_event_handler const *handlers,
	long handler_count)
{
	struct vr_menu_widget *widget;
	struct vr_menu_game_data_input *input;
	long tag_index = vr_menu_clone(template_index, tag_name, widget_name, &widget);

	if (tag_index == NONE || !(input = vr_menu_allocate(sizeof(*input))) ||
		!vr_menu_handlers(widget, handlers, handler_count))
	{
		return NONE;
	}
	input->function = VR_MENU_GAME_DATA_FUNCTION;
	widget->game_data_inputs.count = 1;
	widget->game_data_inputs.address = input;
	widget->text_label_string_list.index = NONE;
	widget->text_label_string_list.name = "";
	widget->text_label_string_list.name_length = 0;
	return tag_index;
}

void vr_menu_tags_loaded(
	void)
{
	static char screen_name[] = "ui\\shell\\solo_game\\pause_game\\vr_settings";
	static char list_name[] = "ui\\shell\\solo_game\\pause_game\\vr_settings_list";
	static char button_name[] = "ui\\shell\\solo_game\\pause_game\\vr_settings_button";
	static char setting_names[NUMBEROF(vr_menu_settings)][64];
	/* the button hints, centred under the mission objectives (from under the
	list); the right column of settings above them, as wide as the list's
	buttons (the list is 72 from the screen's left) */
	static short const hints_x = 328, right_column_x = 328 - 72;
	long pause_index = tag_loaded(VR_MENU_WIDGET_TAG, "ui\\shell\\solo_game\\pause_game\\pause_game");
	long list_index = tag_loaded(VR_MENU_WIDGET_TAG, "ui\\shell\\solo_game\\pause_game\\pause_list");
	long resume_index = tag_loaded(VR_MENU_WIDGET_TAG, "ui\\shell\\solo_game\\pause_game\\resume_game_button");
	struct vr_menu_widget *pause, *pause_list, *screen, *list;
	struct vr_menu_child *children;
	struct vr_menu_event_handler handlers[3];
	long index;

	/* (the last map's) */
	for (index = 0; index < vr_menu.allocation_count; index++)
		free(vr_menu.allocations[index]);
	memset(&vr_menu, 0, sizeof(vr_menu));
	vr_menu.button_tag_index = vr_menu.screen_tag_index = vr_menu.list_tag_index = NONE;
	for (index = 0; index < VR_MENU_SETTING_COUNT; index++)
		vr_menu.setting_tag_indices[index] = NONE;
	if (pause_index == NONE || list_index == NONE || resume_index == NONE)
		return;
	pause = vr_menu_widget_get(pause_index);
	pause_list = vr_menu_widget_get(list_index);
	/* the pause menu as this expects it: its backdrop's boxes, its list
	(four buttons, 28 apart), the mission objectives and the button hints */
	if (pause->child_widgets.count != 5 || pause_list->child_widgets.count != 4)
	{
		platform_log("vr: the pause menu is not as expected; no VR settings in it");
		return;
	}

	/* each setting: A or right steps on, left back */
	memset(handlers, 0, sizeof(handlers));
	handlers[0].flags = VR_MENU_RUN_FUNCTION;
	handlers[0].event_type = VR_MENU_EVENT_A;
	handlers[0].function = VR_MENU_NEXT_FUNCTION;
	handlers[1] = handlers[0];
	handlers[1].event_type = VR_MENU_EVENT_DPAD_RIGHT;
	handlers[2] = handlers[0];
	handlers[2].event_type = VR_MENU_EVENT_DPAD_LEFT;
	handlers[2].function = VR_MENU_PREVIOUS_FUNCTION;
	for (index = 0; index < 3; index++)
	{
		vr_menu_reference(&handlers[index].widget_tag, NONE);
		vr_menu_reference(&handlers[index].sound_effect, NONE);
	}
	for (index = 0; index < VR_MENU_SETTING_COUNT; index++)
	{
		char widget_name[32];

		snprintf(setting_names[index], sizeof(setting_names[index]),
			"ui\\shell\\solo_game\\pause_game\\vr_setting_%ld", index);
		snprintf(widget_name, sizeof(widget_name), "vr_setting_%ld", index);
		vr_menu.setting_tag_indices[index] = vr_menu_button(resume_index, setting_names[index], widget_name,
			handlers, 3);
		if (vr_menu.setting_tag_indices[index] == NONE)
			return;
	}

	/* the list: the pause menu's, wide enough for both columns */
	vr_menu.list_tag_index = vr_menu_clone(list_index, list_name, "vr settings list", &list);
	if (vr_menu.list_tag_index == NONE || !(children = vr_menu_allocate(VR_MENU_SETTING_COUNT * (long)sizeof(*children))))
		return;
	for (index = 0; index < VR_MENU_SETTING_COUNT; index++)
	{
		long row = index < VR_MENU_LEFT_COUNT ? index : index - VR_MENU_LEFT_COUNT;

		memcpy(&children[index], (struct vr_menu_child *)pause_list->child_widgets.address, sizeof(children[index]));
		vr_menu_reference(&children[index].widget_tag, vr_menu.setting_tag_indices[index]);
		snprintf(children[index].name, sizeof(children[index].name), "vr_setting_%ld", index);
		children[index].vertical_offset = (short)(row * 28);
		children[index].horizontal_offset = index < VR_MENU_LEFT_COUNT ? 0 : right_column_x;
	}
	list->bounds.x1 = (short)(list->bounds.x1 + right_column_x);
	list->child_widgets.count = VR_MENU_SETTING_COUNT;
	list->child_widgets.address = children;

	/* the screen: the pause menu's backdrop and boxes, the list, and the
	button hints; B (or back) returns to the pause menu, start resumes */
	vr_menu.screen_tag_index = vr_menu_clone(pause_index, screen_name, "vr_settings", &screen);
	if (vr_menu.screen_tag_index == NONE || !(children = vr_menu_allocate(3 * (long)sizeof(*children))))
		return;
	memcpy(&children[0], (struct vr_menu_child *)pause->child_widgets.address + 0, sizeof(children[0]));
	memcpy(&children[1], (struct vr_menu_child *)pause->child_widgets.address + 1, sizeof(children[1]));
	memcpy(&children[2], (struct vr_menu_child *)pause->child_widgets.address + 4, sizeof(children[2]));
	vr_menu_reference(&children[1].widget_tag, vr_menu.list_tag_index);
	strcpy(children[1].name, "vr_settings_list");
	children[2].horizontal_offset = hints_x;
	screen->child_widgets.count = 3;
	screen->child_widgets.address = children;
	memset(handlers, 0, sizeof(handlers));
	handlers[0].flags = VR_MENU_CLOSE_CURRENT;
	handlers[0].event_type = VR_MENU_EVENT_B;
	handlers[1] = handlers[0];
	handlers[1].event_type = VR_MENU_EVENT_BACK;
	handlers[2].flags = VR_MENU_CLOSE_ALL;
	handlers[2].event_type = VR_MENU_EVENT_START;
	for (index = 0; index < 3; index++)
	{
		vr_menu_reference(&handlers[index].widget_tag, NONE);
		vr_menu_reference(&handlers[index].sound_effect, NONE);
	}
	if (!vr_menu_handlers(screen, handlers, 3))
		return;

	/* the pause menu's new item, opening the screen */
	memset(handlers, 0, sizeof(handlers));
	handlers[0].flags = VR_MENU_OPEN_WIDGET;
	handlers[0].event_type = VR_MENU_EVENT_A;
	vr_menu_reference(&handlers[0].widget_tag, vr_menu.screen_tag_index);
	vr_menu_reference(&handlers[0].sound_effect, NONE);
	vr_menu.button_tag_index = vr_menu_button(resume_index, button_name, "vr_settings_button", handlers, 1);
	if (vr_menu.button_tag_index == NONE || !(children = vr_menu_allocate(5 * (long)sizeof(*children))))
		return;
	/* (the map's own tags changed last, when nothing can fail) */
	memcpy(children, pause_list->child_widgets.address, 4 * sizeof(*children));
	children[4] = children[3];
	vr_menu_reference(&children[4].widget_tag, vr_menu.button_tag_index);
	strcpy(children[4].name, "vr_settings_button");
	children[4].vertical_offset = (short)(children[3].vertical_offset + 28);
	pause_list->child_widgets.count = 5;
	pause_list->child_widgets.address = children;
	((struct vr_menu_child *)pause->child_widgets.address)[4].horizontal_offset = hints_x;
	platform_log("vr: VR settings added to the pause menu");
}

/* ---------- the widgets' callbacks */

/* which setting the widget's definition is: its index, NONE for the pause
menu's item, or -2 for neither */
static long vr_menu_setting_of(
	long definition_tag_index)
{
	long index;

	if (definition_tag_index == NONE)
		return -2;
	if (definition_tag_index == vr_menu.button_tag_index)
		return NONE;
	for (index = 0; index < VR_MENU_SETTING_COUNT; index++)
	{
		if (vr_menu.setting_tag_indices[index] == definition_tag_index)
			return index;
	}
	return -2;
}

boolean vr_menu_setting_text(
	long definition_tag_index,
	wchar_t *text,
	long size)
{
	long setting_index = vr_menu_setting_of(definition_tag_index);
	char line[64];
	long index;

	if (setting_index == -2 || size <= 0)
		return FALSE;
	if (setting_index == NONE)
	{
		snprintf(line, sizeof(line), "VR SETTINGS");
	}
	else
	{
		struct vr_menu_setting const *setting = &vr_menu_settings[setting_index];
		long value_index = vr_menu_value_index(setting);

		snprintf(line, sizeof(line), "%s: %s", setting->label,
			value_index != NONE ? setting->values[value_index].label : "CUSTOM");
	}
	for (index = 0; line[index] && index < size - 1; index++)
		text[index] = (wchar_t)(unsigned char)line[index];
	text[index] = 0;
	return TRUE;
}

boolean vr_menu_setting_change(
	long definition_tag_index,
	long step)
{
	long setting_index = vr_menu_setting_of(definition_tag_index);
	struct vr_menu_setting const *setting;
	long value_index;
	char const *value;
	boolean written = FALSE;

	if (setting_index < 0)
		return FALSE;
	setting = &vr_menu_settings[setting_index];
	value_index = vr_menu_value_index(setting);
	value_index = value_index == NONE ? 0 :
		(value_index + step + setting->value_count) % setting->value_count;
	value = setting->values[value_index].value;
	switch (setting->type)
	{
	case _vr_setting_boolean:
		written = config_write_boolean(setting->key, !strcmp(value, "true"));
		break;
	case _vr_setting_real:
		written = config_write_real(setting->key, atof(value));
		break;
	case _vr_setting_string:
		written = config_write_string(setting->key, value);
		break;
	}
	vr_reload_settings();
	platform_log("vr: %s set to %s%s", setting->key, value, written ? "" : " (not saved to config.toml)");
	return TRUE;
}

#endif
