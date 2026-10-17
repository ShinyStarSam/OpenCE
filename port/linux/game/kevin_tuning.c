/*
KEVIN_TUNING.C

Kevin's combat tuning (kevin.enabled; kevin/BRIDGE_PLAN.md, "Combat
tuning: from useless to solo-the-game", milestone M4).

Kevin is a Spartan of his own, the same on every map, whatever actors it
has: his actor variant and actor definition are made here, from his own
numbers (kevin_tuning_baseline), and added to the tag table at each level's
start as tags of their own (kevin\actor variant and kevin\actor; the table
is copied with them after it, as menu_tags.c does for the menus, every
existing tag keeping its index). His actor is made from them. They are let
go of when the next map's table replaces this one.

The sidecar changes eight settings (`tuning`), each worked into them from
his baseline, 50 (or x 1) being his own: accuracy, damage dealt and taken,
rate of fire, reaction time, grenades, cover and aggression; or picks a
preset. Damage is scaled here (damage.c's hook), and
at the ends of accuracy his aim is taken over (kevin_bot.c's): at 0 just
past his target's body, every shot, and at 100 its head with the lead its
speed needs; there his shots go exactly where he aims (weapons.c's hook),
without the weapon's own spread.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "ai/actor_definitions.h"
#include "ai/actor_types.h"
#include "ai/actors.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"

#include "kevin_bot.h"
#include "kevin_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cache_files.c's (port) */
void *cache_files_tag_instances(long *count);
void cache_files_set_tag_instances(void *instances, long count);
char *tag_get_name(long tag_index);

/* ---------- constants */

enum
{
	_kevin_setting_accuracy = 0,
	_kevin_setting_damage_dealt,
	_kevin_setting_damage_taken,
	_kevin_setting_rate_of_fire,
	_kevin_setting_reaction,
	_kevin_setting_grenades,
	_kevin_setting_cover,
	_kevin_setting_aggression,
	NUMBER_OF_KEVIN_SETTINGS,

	KEVIN_TUNING_NAME_SIZE = 256,
};

/* at accuracy 0 he aims this far outside his target's bounding sphere */
#define KEVIN_MISS_MARGIN 0.6f

/* ---------- structures */

/* cache_files.c's tag table entry (menu_tags.c has it too) */
struct cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

typedef char kevin_tuning_tag_instance_size[sizeof(struct cache_file_tag_instance) == 0x20 ? 1 : -1];

struct kevin_setting_definition
{
	char const *name;
	real minimum;
	real maximum;
	real own;                    /* his own (his baseline's) */
};

struct kevin_preset
{
	char const *name;
	real values[NUMBER_OF_KEVIN_SETTINGS];
};

/* ---------- globals */

static struct kevin_setting_definition const kevin_setting_definitions[NUMBER_OF_KEVIN_SETTINGS] =
{
	{ "accuracy", 0.f, 100.f, 50.f },
	{ "damage_dealt", 0.f, 10.f, 1.f },
	{ "damage_taken", 0.f, 10.f, 1.f },
	{ "rate_of_fire", 0.f, 100.f, 50.f },
	{ "reaction", 0.f, 100.f, 50.f },
	{ "grenades", 0.f, 100.f, 50.f },
	{ "cover", 0.f, 100.f, 50.f },
	{ "aggression", 0.f, 100.f, 50.f },
};

/* accuracy, damage dealt, damage taken, rate of fire, reaction, grenades,
cover, aggression */
static struct kevin_preset const kevin_presets[] =
{
	{ "useless", { 0.f, 0.2f, 3.f, 20.f, 0.f, 0.f, 0.f, 20.f } },
	{ "kevin", { 50.f, 1.f, 1.f, 50.f, 50.f, 50.f, 50.f, 50.f } },
	{ "veteran", { 75.f, 1.5f, 0.6f, 65.f, 75.f, 60.f, 60.f, 60.f } },
	{ "master_chief", { 90.f, 2.5f, 0.35f, 75.f, 90.f, 70.f, 70.f, 75.f } },
	{ "god", { 100.f, 10.f, 0.f, 100.f, 100.f, 100.f, 50.f, 100.f } },
};

static struct
{
	real settings[NUMBER_OF_KEVIN_SETTINGS];
	char const *preset;           /* the preset the settings are, or NULL */
	boolean settings_valid;

	/* the tag table with his tags after the map's (NULL: none) */
	struct cache_file_tag_instance *instances;
	long instance_count;
	long variant_index;           /* his tags, or NONE */
	long actor_index;
	struct actor_variant_definition original_variant;  /* his baseline */
	struct actor_variant_definition variant;           /* ... tuned */
	struct actor_definition original_actor;
	struct actor_definition actor;

	long shots;
	long hits;
	/* (a round can damage a unit and what it rides in: a hit counted once a
	tick for each) */
	long last_hit_tick;
	long last_hit_object_index;
} kevin_tuning_globals = { { 0.f }, NULL, FALSE, NULL, 0, NONE, NONE };

/* ---------- private code */

/* a factor from a 0-100 setting: low at 0, 1 at 50, high at 100 */
static real kevin_tuning_curve(
	real setting,
	real low,
	real high)
{
	return setting < 50.f
		? low + (1.f - low) * setting / 50.f
		: 1.f + (high - 1.f) * (setting - 50.f) / 50.f;
}

/* a fraction (a chance, a tracking fraction) from a 0-100 setting: none at
0, his own at 50, all at 100 */
static real kevin_tuning_fraction(
	real setting,
	real own)
{
	return setting < 50.f
		? own * setting / 50.f
		: own + (1.f - own) * (setting - 50.f) / 50.f;
}

static void kevin_tuning_default_settings(
	void)
{
	long index;

	if (kevin_tuning_globals.settings_valid)
		return;
	for (index = 0; index < NUMBER_OF_KEVIN_SETTINGS; index++)
		kevin_tuning_globals.settings[index] = kevin_setting_definitions[index].own;
	kevin_tuning_globals.preset = "kevin";
	kevin_tuning_globals.settings_valid = TRUE;

	return;
}

/* the copies made again from the originals, by the settings */
static void kevin_tuning_apply(
	void)
{
	struct actor_variant_definition *variant = &kevin_tuning_globals.variant;
	struct actor_ranged_combat_properties *ranged = &variant->ranged_combat;
	struct actor_grenade_combat_properties *grenades = &variant->grenade_combat;
	struct actor_definition *actor = &kevin_tuning_globals.actor;
	real const *settings = kevin_tuning_globals.settings;
	real factor;

	if (kevin_tuning_globals.variant_index == NONE)
		return;
	*variant = kevin_tuning_globals.original_variant;
	*actor = kevin_tuning_globals.original_actor;
	variant->actor_reference.index = kevin_tuning_globals.actor_index;

	/* accuracy: his aim's error, and how closely he tracks and leads */
	ranged->projectile_error_angle *= kevin_tuning_curve(settings[_kevin_setting_accuracy], 3.f, 0.1f);
	ranged->target_track_fraction = kevin_tuning_fraction(settings[_kevin_setting_accuracy], ranged->target_track_fraction);
	ranged->target_lead_fraction = kevin_tuning_fraction(settings[_kevin_setting_accuracy], ranged->target_lead_fraction);

	/* rate of fire: rounds a second, bursts longer and closer together */
	ranged->rate_of_fire *= kevin_tuning_curve(settings[_kevin_setting_rate_of_fire], 0.3f, 3.f);
	factor = kevin_tuning_curve(settings[_kevin_setting_rate_of_fire], 0.5f, 2.5f);
	ranged->burst_geometry.burst_duration_lower_bound *= factor;
	ranged->burst_geometry.burst_duration_upper_bound *= factor;
	factor = kevin_tuning_curve(settings[_kevin_setting_rate_of_fire], 3.f, 0.15f);
	ranged->burst_geometry.burst_separation_lower_bound *= factor;
	ranged->burst_geometry.burst_separation_upper_bound *= factor;

	/* reaction: the delays before he fires at what he has seen */
	factor = kevin_tuning_curve(settings[_kevin_setting_reaction], 4.f, 0.f);
	ranged->first_burst_delay_lower_bound *= factor;
	ranged->first_burst_delay_upper_bound *= factor;
	ranged->surprise_delay_time *= factor;
	ranged->new_target_pattern_time *= kevin_tuning_curve(settings[_kevin_setting_reaction], 3.f, 0.f);

	/* grenades: how likely and how often; without end at 100 */
	grenades->throw_grenade_chance = kevin_tuning_fraction(settings[_kevin_setting_grenades], grenades->throw_grenade_chance);
	grenades->throw_grenade_delay *= kevin_tuning_curve(settings[_kevin_setting_grenades], 3.f, 0.2f);
	SET_FLAG(variant->flags, _actor_variant_definition_has_unlimited_grenades_bit,
		settings[_kevin_setting_grenades] >= 100.f ||
			TEST_FLAG(kevin_tuning_globals.original_variant.flags, _actor_variant_definition_has_unlimited_grenades_bit));

	/* cover: how far he goes for it, how readily, how soon he hides */
	actor->defensive.cover_max_distance *= kevin_tuning_curve(settings[_kevin_setting_cover], 0.f, 3.f);
	actor->defensive.evasion_seek_cover_chance =
		kevin_tuning_fraction(settings[_kevin_setting_cover], actor->defensive.evasion_seek_cover_chance);
	actor->defensive.shield_fraction_hide =
		kevin_tuning_fraction(settings[_kevin_setting_cover], actor->defensive.shield_fraction_hide);

	/* aggression: the range he fights from, leaps, and panic */
	factor = kevin_tuning_curve(settings[_kevin_setting_aggression], 1.6f, 0.5f);
	ranged->combat_range_lower_bound *= factor;
	ranged->combat_range_upper_bound *= factor;
	actor->berserk.melee_leap_chance =
		kevin_tuning_fraction(settings[_kevin_setting_aggression], actor->berserk.melee_leap_chance);
	actor->panic.panic_chance_friend_killed *= kevin_tuning_curve(settings[_kevin_setting_aggression], 2.f, 0.f);
	if (actor->panic.panic_chance_friend_killed > 1.f)
		actor->panic.panic_chance_friend_killed = 1.f;

	return;
}

/* a tag table entry of his */
static void kevin_tuning_instance_set(
	struct cache_file_tag_instance *instance,
	long group_tag,
	long tag_index,
	char *name,
	void *definition)
{
	csmemset(instance, 0, sizeof(*instance));
	instance->group_tag = group_tag;
	instance->parent_group_tags[0] = NONE;
	instance->parent_group_tags[1] = NONE;
	instance->tag_index = tag_index;
	instance->name = name;
	instance->base_address = definition;

	return;
}

static void kevin_tuning_idle_looking(
	struct actor_idle_looking *idle,
	real facing_lower,
	real facing_upper,
	real aim_lower,
	real aim_upper,
	real look_lower,
	real look_upper)
{
	idle->idle_facing_time_lower_bound = facing_lower;
	idle->idle_facing_time_upper_bound = facing_upper;
	idle->idle_aim_time_lower_bound = aim_lower;
	idle->idle_aim_time_upper_bound = aim_upper;
	idle->idle_look_time_lower_bound = look_lower;
	idle->idle_look_time_upper_bound = look_upper;

	return;
}

/* what a map's loader works out from the definition's numbers */
static void kevin_tuning_runtime(
	struct actor_definition *actor)
{
	struct actor_perception_properties *perception = &actor->perception;
	struct actor_looking_properties *looking = &actor->looking;

	perception->runtime_awareness_delta_combat = perception->combat_awareness_time > 0.f
		? 1.f / (perception->combat_awareness_time * TICKS_PER_SECOND) : 0.f;
	perception->runtime_awareness_delta_guard = perception->guard_awareness_time > 0.f
		? 1.f / (perception->guard_awareness_time * TICKS_PER_SECOND) : 0.f;
	perception->runtime_awareness_delta_non_combat = perception->non_combat_awareness_time > 0.f
		? 1.f / (perception->non_combat_awareness_time * TICKS_PER_SECOND) : 0.f;
	looking->runtime_maximum_aiming_deviation_cosine.yaw = cosine(looking->maximum_aiming_deviation.yaw);
	looking->runtime_maximum_aiming_deviation_cosine.pitch = cosine(looking->maximum_aiming_deviation.pitch);
	looking->runtime_maximum_looking_deviation_cosine.yaw = cosine(looking->maximum_looking_deviation.yaw);
	looking->runtime_maximum_looking_deviation_cosine.pitch = cosine(looking->maximum_looking_deviation.pitch);
	actor->moving.runtime_begin_movement_angle_cosine = actor->moving.begin_movement_angle > 0.f
		? cosine(actor->moving.begin_movement_angle) : 0.f;

	return;
}

/* his own AI: the same on every map, whatever its actors. His numbers began
as the campaign's marine's, as a soldier of the same training, and differ
where he is a Spartan: his weapons' full damage (a marine's are scaled to
0.6), his body's own health and shields. What the engine works out from
them as a map loads (cosines, awareness a tick) is worked out here */
static void kevin_tuning_baseline(
	struct actor_variant_definition *variant,
	struct actor_definition *actor)
{
	struct actor_ranged_combat_properties *ranged = &variant->ranged_combat;
	struct actor_burst_geometry *burst = &ranged->burst_geometry;
	struct actor_grenade_combat_properties *grenades = &variant->grenade_combat;
	struct actor_looking_properties *looking = &actor->looking;

	csmemset(variant, 0, sizeof(*variant));
	csmemset(actor, 0, sizeof(*actor));

	/* the variant: his references are none but his actor definition (his
	unit, weapons and grenades are his own: kevin_bot.c) */
	variant->actor_reference.group_tag = ACTOR_DEFINITION_TAG;
	variant->actor_reference.name = "";
	variant->actor_reference.index = NONE;
	variant->unit_reference.group_tag = 'unit';
	variant->unit_reference.name = "";
	variant->unit_reference.index = NONE;
	variant->major_upgrade_reference.group_tag = ACTOR_VARIANT_DEFINITION_TAG;
	variant->major_upgrade_reference.name = "";
	variant->major_upgrade_reference.index = NONE;
	ranged->reference.group_tag = 'weap';
	ranged->reference.name = "";
	ranged->reference.index = NONE;
	variant->items.equipment_reference.group_tag = 'eqip';
	variant->items.equipment_reference.name = "";
	variant->items.equipment_reference.index = NONE;

	ranged->maximum_firing_range = 13.f;
	ranged->rate_of_fire = 10.f;
	ranged->projectile_error_angle = 0.0349f;
	ranged->first_burst_delay_lower_bound = 0.25f;
	ranged->first_burst_delay_upper_bound = 0.75f;
	ranged->surprise_delay_time = 0.5f;
	ranged->combat_range_lower_bound = 3.f;
	ranged->combat_range_upper_bound = 6.f;
	ranged->target_track_fraction = 1.f;
	ranged->target_lead_fraction = 0.8f;
	ranged->weapon_damage_modifier = 1.f;
	burst->burst_origin_radius = 0.25f;
	burst->burst_origin_angle = 0.0873f;
	burst->burst_return_length_lower_bound = 0.2f;
	burst->burst_return_length_upper_bound = 0.35f;
	burst->burst_return_angle = 0.2618f;
	burst->burst_duration_lower_bound = 0.15f;
	burst->burst_duration_upper_bound = 0.35f;
	burst->burst_separation_lower_bound = 1.f;
	burst->burst_separation_upper_bound = 1.2f;

	grenades->grenade_type = 0;
	grenades->trajectory_type = 0;
	grenades->stimulus_type = 1;
	grenades->minimum_enemy_count = 3;
	grenades->enemy_radius = 3.5f;
	grenades->grenade_maximum_velocity = 0.2333f;
	grenades->grenade_range_lower_bound = 3.f;
	grenades->grenade_range_upper_bound = 14.f;
	grenades->collateral_damage_radius = 5.f;
	grenades->throw_grenade_chance = 0.25f;
	grenades->throw_grenade_delay = 1.f;
	grenades->encounter_grenade_timeout = 20.f;

	variant->items.grenades_lower_bound = 1;
	variant->items.grenades_upper_bound = 2;
	variant->items.dont_drop_grenades_chance = 0.7f;
	variant->items.weapon_loaded_lower_bound = 0.3f;
	variant->items.weapon_loaded_upper_bound = 0.9f;
	variant->items.weapon_ammo_lower_bound = 400;
	variant->items.weapon_ammo_upper_bound = 600;

	/* the definition: a soldier of the player's side */
	actor->flags =
		FLAG(_actor_definition_sneak_uncovering_target_bit) |
		FLAG(_actor_definition_shoot_at_targets_last_location_bit) |
		FLAG(_actor_definition_helps_players_in_vehicles_bit) |
		FLAG(_actor_definition_crouch_when_hiding_from_unopposable_enemy_bit) |
		FLAG(_actor_definition_crouch_in_line_of_fire_bit) |
		FLAG(_actor_definition_avoid_friend_line_of_fire_bit);
	actor->flags2 = 0x20;
	/* (the AI's soldier: its human soldiers' behaviour) */
	actor->type = _actor_marine;

	actor->perception.maximum_vision_distance = 40.f;
	actor->perception.central_vision_angle = 0.8727f;
	actor->perception.maximum_vision_angle = 1.2217f;
	actor->perception.peripheral_vision_angle = 2.0944f;
	actor->perception.gun_offset_stand.i = 0.2f;
	actor->perception.gun_offset_stand.j = -0.1f;
	actor->perception.gun_offset_stand.k = 0.51f;
	actor->perception.hearing_distance = 40.f;
	actor->perception.notice_projectile_chance = 1.f;
	actor->perception.notice_vehicle_chance = 1.f;
	actor->perception.combat_awareness_time = 0.6f;
	actor->perception.guard_awareness_time = 1.4f;

	actor->moving.cover_dive_chance = 0.25f;
	actor->moving.cover_emerge_chance = 0.25f;
	actor->moving.grenade_dive_chance = 0.85f;
	actor->moving.pathfinding_radius = 0.2f;

	looking->maximum_aiming_deviation.yaw = 1.5708f;
	looking->maximum_aiming_deviation.pitch = 1.5708f;
	looking->maximum_looking_deviation.yaw = 1.5708f;
	looking->maximum_looking_deviation.pitch = 1.0472f;
	looking->noncombat_looking_delta_angles[0] = 1.5708f;
	looking->noncombat_looking_delta_angles[1] = 1.5708f;
	looking->combat_looking_delta_angles[0] = 1.0472f;
	looking->combat_looking_delta_angles[1] = 1.5708f;
	looking->idle_aiming_deviation.yaw = 0.9599f;
	looking->idle_aiming_deviation.pitch = 0.0873f;
	looking->idle_looking_deviation.yaw = 0.7854f;
	looking->idle_looking_deviation.pitch = 0.1745f;
	looking->event_look_modifier_lower_bound = 3.f;
	looking->event_look_modifier_upper_bound = 5.f;
	kevin_tuning_idle_looking(&looking->idle_look_noncombat, 12.f, 20.f, 4.f, 8.f, 2.f, 5.f);
	kevin_tuning_idle_looking(&looking->idle_look_guard, 12.f, 20.f, 3.f, 6.f, 1.f, 3.f);
	kevin_tuning_idle_looking(&looking->idle_look_search_combat, 2.f, 6.f, 2.f, 3.f, 0.5f, 2.5f);

	actor->unopposable.trigger_unreachable_level = 4;
	actor->unopposable.trigger_vehicle_level = 3;
	actor->unopposable.trigger_player_level = 3;
	actor->unopposable.trigger_time_lower_bound = 1.f;
	actor->unopposable.trigger_time_upper_bound = 2.f;
	actor->unopposable.casualties_trigger = 1;
	actor->unopposable.retreating_friends_trigger = 2;
	actor->unopposable.retreat_time_lower_bound = 15.f;
	actor->unopposable.retreat_time_upper_bound = 20.f;

	actor->panic.cower_time_lower_bound = 3.f;
	actor->panic.cower_time_upper_bound = 5.f;
	actor->panic.panic_chance_friend_killed = 0.3f;
	actor->panic.panic_leader_type = _actor_player;
	actor->panic.panic_chance_leader_type_killed = 1.f;
	actor->panic.panic_damage_threshold = 0.5f;
	actor->panic.surprise_distance = 4.f;

	actor->defensive.hide_time_lower_bound = 2.f;
	actor->defensive.hide_time_upper_bound = 3.f;
	actor->defensive.defensive_crouch_type = 1;
	actor->defensive.defensive_threshold_attacking = 1.1f;
	actor->defensive.defensive_threshold_defending = 0.5f;
	actor->defensive.defending_hide_time_modifier = 5.f;
	actor->defensive.evasion_danger_threshold_attacking = 1.55f;
	actor->defensive.evasion_danger_threshold_defending = 0.9f;
	actor->defensive.evasion_seek_cover_chance = 0.6f;
	actor->defensive.evasion_delay_timer = 4.f;
	actor->defensive.cover_damage_threshold = 0.3f;

	actor->pursuit.uncover_delay_time_lower_bound = 1.f;
	actor->pursuit.uncover_delay_time_upper_bound = 3.f;
	actor->pursuit.target_location_time_lower_bound = 1.f;
	actor->pursuit.target_location_time_upper_bound = 3.f;
	actor->pursuit.pursuit_position_time_lower_bound = 1.f;
	actor->pursuit.pursuit_position_time_upper_bound = 3.f;
	actor->pursuit.coordinated_pursuit_positions = 6;
	actor->pursuit.uncoordinated_pursuit_positions = 7;

	actor->berserk.damage_berserk_amount = 0.1f;
	actor->berserk.damage_berserk_threshold = 0.45f;

	actor->firing_position.guard_position_time_lower_bound = 4.f;
	actor->firing_position.guard_position_time_upper_bound = 6.f;
	actor->firing_position.combat_position_time_lower_bound = 4.f;
	actor->firing_position.combat_position_time_upper_bound = 6.f;
	actor->firing_position.friend_avoidance_radius = 1.5f;

	kevin_tuning_runtime(actor);

	return;
}


static long kevin_tuning_setting_by_name(
	char const *name)
{
	long index;

	for (index = 0; index < NUMBER_OF_KEVIN_SETTINGS; index++)
	{
		if (!strcmp(kevin_setting_definitions[index].name, name))
			return index;
	}

	return NONE;
}

/* ---------- public code */

void kevin_tuning_level_begin(
	void)
{
	struct cache_file_tag_instance *instances;
	struct cache_file_tag_instance *grown;
	long count;
	long index;
	long salt = 0;

	kevin_tuning_default_settings();
	kevin_tuning_globals.shots = 0;
	kevin_tuning_globals.hits = 0;
	kevin_tuning_globals.last_hit_tick = NONE;
	instances = cache_files_tag_instances(&count);
	/* (the level begun again: his tags are there) */
	if (instances && instances == kevin_tuning_globals.instances)
	{
		kevin_tuning_apply();
		return;
	}
	/* (the last map's: this map's table replaced it) */
	if (kevin_tuning_globals.instances)
		free(kevin_tuning_globals.instances);
	kevin_tuning_globals.instances = NULL;
	kevin_tuning_globals.variant_index = NONE;
	kevin_tuning_globals.actor_index = NONE;
	if (!instances || count < 0 || count > UNSIGNED_SHORT_MAX - 2)
		return;
	grown = malloc((count + 2) * sizeof(*grown));
	if (!grown)
		return;
	memcpy(grown, instances, count * sizeof(*grown));
	for (index = 0; index < count; index++)
	{
		long instance_salt = (long)((unsigned long)instances[index].tag_index >> 16);

		if (instance_salt > salt)
			salt = instance_salt;
	}
	kevin_tuning_baseline(&kevin_tuning_globals.original_variant, &kevin_tuning_globals.original_actor);
	kevin_tuning_globals.variant_index = ((salt + 1) << 16) | count;
	kevin_tuning_globals.actor_index = ((salt + 2) << 16) | (count + 1);
	kevin_tuning_instance_set(&grown[count], ACTOR_VARIANT_DEFINITION_TAG, kevin_tuning_globals.variant_index,
		"kevin\\actor variant", &kevin_tuning_globals.variant);
	kevin_tuning_instance_set(&grown[count + 1], ACTOR_DEFINITION_TAG, kevin_tuning_globals.actor_index,
		"kevin\\actor", &kevin_tuning_globals.actor);
	kevin_tuning_globals.instances = grown;
	kevin_tuning_globals.instance_count = count + 2;
	cache_files_set_tag_instances(grown, count + 2);
	kevin_tuning_apply();

	return;
}

long kevin_tuning_variant_index(
	void)
{
	return kevin_tuning_globals.variant_index;
}

long kevin_tuning_setting_count(
	void)
{
	return NUMBER_OF_KEVIN_SETTINGS;
}

char const *kevin_tuning_setting_name(
	long index)
{
	return index >= 0 && index < NUMBER_OF_KEVIN_SETTINGS ? kevin_setting_definitions[index].name : "";
}

real kevin_tuning_setting_value(
	long index)
{
	kevin_tuning_default_settings();

	return index >= 0 && index < NUMBER_OF_KEVIN_SETTINGS ? kevin_tuning_globals.settings[index] : 0.f;
}

boolean kevin_tuning_set(
	char const *name,
	real value)
{
	long index = kevin_tuning_setting_by_name(name);

	kevin_tuning_default_settings();
	if (index == NONE ||
		!(value >= kevin_setting_definitions[index].minimum && value <= kevin_setting_definitions[index].maximum))
	{
		return FALSE;
	}
	if (kevin_tuning_globals.settings[index] != value)
		kevin_tuning_globals.preset = NULL;
	kevin_tuning_globals.settings[index] = value;
	kevin_tuning_apply();

	return TRUE;
}

boolean kevin_tuning_preset(
	char const *name)
{
	long index;

	for (index = 0; index < (long)NUMBEROF(kevin_presets); index++)
	{
		if (!strcmp(kevin_presets[index].name, name))
		{
			csmemcpy(kevin_tuning_globals.settings, kevin_presets[index].values, sizeof(kevin_tuning_globals.settings));
			kevin_tuning_globals.preset = kevin_presets[index].name;
			kevin_tuning_globals.settings_valid = TRUE;
			kevin_tuning_apply();
			return TRUE;
		}
	}

	return FALSE;
}

char const *kevin_tuning_preset_name(
	void)
{
	kevin_tuning_default_settings();

	return kevin_tuning_globals.preset ? kevin_tuning_globals.preset : "";
}

void kevin_tuning_reset(
	void)
{
	if (kevin_tuning_globals.preset && !strcmp(kevin_tuning_globals.preset, "kevin"))
		return;
	kevin_tuning_preset("kevin");
	error(_error_silent, "kevin: tuned as himself again");

	return;
}

real kevin_tuning_damage_scale(
	long owner_object_index,
	long victim_object_index)
{
	long kevin_unit_index = kevin_bot_unit_index();
	real scale = 1.f;

	if (kevin_unit_index == NONE || !kevin_tuning_globals.settings_valid)
		return 1.f;
	if (owner_object_index == kevin_unit_index && victim_object_index != kevin_unit_index)
	{
		scale *= kevin_tuning_globals.settings[_kevin_setting_damage_dealt];
		if (unit_try_and_get(victim_object_index) &&
			(game_time_get() != kevin_tuning_globals.last_hit_tick ||
				victim_object_index != kevin_tuning_globals.last_hit_object_index))
		{
			kevin_tuning_globals.hits++;
			kevin_tuning_globals.last_hit_tick = game_time_get();
			kevin_tuning_globals.last_hit_object_index = victim_object_index;
		}
	}
	if (victim_object_index == kevin_unit_index)
		scale *= kevin_tuning_globals.settings[_kevin_setting_damage_taken];

	return scale;
}

boolean kevin_tuning_exact_shot(
	long owner_object_index)
{
	real accuracy;

	if (owner_object_index == NONE || owner_object_index != kevin_bot_unit_index())
		return FALSE;
	kevin_tuning_globals.shots++;
	accuracy = kevin_tuning_setting_value(_kevin_setting_accuracy);

	return accuracy <= 0.f || accuracy >= 100.f;
}

/* where he aims at the ends of his accuracy, from a point (his eye, or his
gun); FALSE elsewhere, or with no target */
static boolean kevin_tuning_aim_from(
	long actor_index,
	real_point3d const *from,
	real_vector3d *aim)
{
	struct actor_datum *actor = actor_get(actor_index);
	long unit_index = actor->meta.unit_index;
	real accuracy = kevin_tuning_setting_value(_kevin_setting_accuracy);
	long target_index;
	struct object_datum *target;
	real_point3d point;

	if (accuracy > 0.f && accuracy < 100.f)
		return FALSE;
	target_index = actor_target_unit_index(actor_index);
	target = target_index != NONE ? object_try_and_get(target_index) : NULL;
	if (!target)
		return FALSE;
	if (accuracy >= 100.f)
	{
		/* its head, where it will be when his round gets there */
		struct unit_datum *unit = unit_get(unit_index);
		short slot = unit->unit.current_weapon_index;
		long weapon_index = slot >= 0 && slot < MAXIMUM_WEAPONS_PER_UNIT ? unit->unit.weapon_object_indices[slot] : NONE;
		real speed = 0.f;

		unit_get_head_position(target_index, &point);
		if (weapon_index != NONE)
		{
			struct weapon_definition *weapon = weapon_definition_get(weapon_get(weapon_index)->definition_index);

			if (weapon->weapon.triggers.count > 0)
			{
				struct weapon_trigger_definition *trigger = TAG_BLOCK_GET_ELEMENT(&weapon->weapon.triggers, 0,
					struct weapon_trigger_definition);

				if (trigger->projectile.index != NONE)
					speed = projectile_definition_get(trigger->projectile.index)->projectile.initial_velocity;
			}
		}
		if (speed > 0.f)
		{
			real ticks = distance3d(from, &point) / speed;

			point_from_line3d(&point, &target->object.translational_velocity, ticks, &point);
		}
	}
	else
	{
		/* to the side of its body, just outside it: of what it rides in, when
		it rides (a dropship's pilot is inside its hull) */
		real_vector3d toward;
		real_vector3d side;

		while (target->object.parent_object_index != NONE && object_try_and_get(target->object.parent_object_index))
			target = object_get(target->object.parent_object_index);
		vector_from_points3d(from, &target->object.bounding_sphere_center, &toward);
		cross_product3d(&toward, global_up3d, &side);
		if (normalize3d(&side) == 0.f)
			side = *global_forward3d;
		point_from_line3d(&target->object.bounding_sphere_center, &side,
			target->object.bounding_sphere_radius + KEVIN_MISS_MARGIN, &point);
	}
	vector_from_points3d(from, &point, aim);

	return normalize3d(aim) > 0.f;
}

boolean kevin_tuning_aim(
	long actor_index,
	real_vector3d *aim)
{
	real_point3d eye;

	unit_get_camera_position(actor_get(actor_index)->meta.unit_index, &eye);

	return kevin_tuning_aim_from(actor_index, &eye, aim);
}

boolean kevin_tuning_aim_projectile(
	long actor_index,
	real_point3d const *origin,
	real_vector3d *forward)
{
	long unit_index = actor_get(actor_index)->meta.unit_index;
	real_vector3d aim;

	if (unit_index == NONE || unit_index != kevin_bot_unit_index() || !kevin_tuning_aim_from(actor_index, origin, &aim))
		return FALSE;
	*forward = aim;

	return TRUE;
}

long kevin_tuning_shots(
	void)
{
	return kevin_tuning_globals.shots;
}

long kevin_tuning_hits(
	void)
{
	return kevin_tuning_globals.hits;
}
