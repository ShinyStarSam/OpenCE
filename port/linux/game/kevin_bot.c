/*
KEVIN_BOT.C

Kevin, an AI companion (kevin.enabled, a prototype; kevin/BRIDGE_PLAN.md in
the repository): a Spartan, of the player's biped, in his own armour colour,
with the level's starting weapons, whom the game's AI drives. He is no
player: you play the campaign alone, with your screen and sound, and he is
one more of the AI's units at your side.

He is made behind you, on the ground you stand on, once the level has
started and no cutscene plays (kevin_bot_spawn), and made again there a
while after he dies. His unit is given an actor: the level's actor variant
named by kevin.actor_variant, in the first encounter on the structure BSP
loaded, told to follow the first player; the actor and the encounter are
kept active. The AI moves an encounter's actors among its firing
positions, which can be across the level from you; when it sends Kevin to
one more than a few units from you, he walks to you instead
(kevin_bot_move_to_you), and stops near you.

Every second the actor's state is written to debug.txt, to see what the AI
does with his unit.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "ai/actor_definitions.h"
#include "ai/actors.h"
#include "ai/ai_globals.h"
#include "ai/ai_debug.h"
#include "ai/ai_scenario_definitions.h"
#include "ai/encounters.h"
#include "ai/path.h"
#include "ai/path_structure_bsp.h"
#include "ai/props.h"
#include "cache/cache_files.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/objects.h"
#include "physics/collision_bsp.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "units/bipeds.h"
#include "units/unit_control_data.h"
#include "units/units.h"

#include "kevin_bot.h"

#include <string.h>

/* port_config.c's */
int config_boolean(const char *name);
char const *config_string(char const *name);

/* main.c's */
extern short player_spawn_count;

/* ---------- constants */

enum
{
	/* ticks between the lines of the actor's state in debug.txt */
	KEVIN_LOG_TICKS = 30,
	/* ticks he stays put, away from you, before he is brought to you */
	KEVIN_STRANDED_TICKS = 150,
	/* ticks after his death before he is made again behind you */
	KEVIN_RESPAWN_TICKS = 10 * 30,

	/* as actors.c's actor_default_state */
	_kevin_actor_state_none = 0,
	_kevin_actor_state_alert = 2,
};

/* how far from you he keeps: he sets off when you are further than
KEVIN_FOLLOW_START, stops nearer than KEVIN_FOLLOW_STOP, and is left to
the AI's firing positions within KEVIN_FOLLOW_FIRING_RADIUS of you */
#define KEVIN_FOLLOW_START 5.f
#define KEVIN_FOLLOW_STOP 3.f
#define KEVIN_FOLLOW_FIRING_RADIUS 6.f
/* how far you move before his destination is moved to you */
#define KEVIN_FOLLOW_RETARGET 1.5f
/* how far from you, staying put, he is brought to you; how far behind you,
and above the ground (world units); ticks outside the level before he is */
#define KEVIN_STRANDED_DISTANCE 10.f
#define KEVIN_BROUGHT_BEHIND 1.5f
#define KEVIN_BROUGHT_ABOVE 0.05f
#define KEVIN_OUTSIDE_TICKS 3

/* the AI takes cover only at its encounter's firing positions: he joins
the encounter of his team with the most of them this near you, looked for
this often (ticks), when it has at least this many more than his own */
#define KEVIN_COVER_DISTANCE 20.f
#define KEVIN_COVER_TICKS 60
#define KEVIN_COVER_BETTER_BY 2

/* an aim turning more than this in a tick (radians: 0.12, as a cosine) is
written to debug.txt as a snap, no more than this many lines a second */
#define KEVIN_SNAP_COSINE 0.9928f
#define KEVIN_SNAP_LINES 10

/* his armour colour (red, green, blue, of 1) */
#define KEVIN_ARMOUR_RED 0.12f
#define KEVIN_ARMOUR_GREEN 0.24f
#define KEVIN_ARMOUR_BLUE 0.55f

/* in combat the AI moves him as it likes within this of you; further, he
comes back to you, and so closes on the enemies you close on */
#define KEVIN_COMBAT_LEASH 8.f

/* enemies this near you, and the one you aim at this near, he is told of
this often (ticks), and prefers as targets by these weights (the AI's
weights are tens for each step of range and knowledge) */
#define KEVIN_SHARED_ENEMY_DISTANCE 15.f
#define KEVIN_AIMED_ENEMY_DISTANCE 40.f
#define KEVIN_SHARE_TICKS 15
#define KEVIN_TARGET_WEIGHT_AIMED 30.f
#define KEVIN_TARGET_WEIGHT_NEAR_YOU 15.f

/* his bursts against the actor variant's: the pauses between them, their
length and the wait before the first */
#define KEVIN_BURST_SEPARATION_SCALE 0.4f
#define KEVIN_BURST_DURATION_SCALE 1.6f
#define KEVIN_FIRST_BURST_DELAY_SCALE 0.4f

/* he changes weapon no sooner than this (ticks) after the last change: to
the one of shortest reach (its autoaim distance) that still reaches the
target, so his pistol far off and his rifle near */
#define KEVIN_WEAPON_SWITCH_TICKS 90
/* ... a weapon without a zoom counted as reaching this much of its autoaim
distance (an assault rifle's 25 to 10, where you change to your pistol, which
you hit with to 30) */
#define KEVIN_WEAPON_UNZOOMED_REACH 0.4f
/* he fires out to this much of his weapon's autoaim distance, rather than
his actor variant's maximum firing range */
#define KEVIN_FIRING_RANGE_SCALE 1.3f
/* an enemy further than this (world units) leaves him time to reload; a
nearer one, he takes out his other weapon instead, as you do */
#define KEVIN_RELOAD_SAFE_DISTANCE 15.f

/* in combat, when the AI has no firing position near you for him, he
takes his own: this far ahead of you toward his target (no nearer it than
the second), this far to a side, kept this long (ticks) or until you move
on this far */
#define KEVIN_ASSAULT_ADVANCE 3.f
#define KEVIN_ASSAULT_TARGET_NEAREST 4.f
#define KEVIN_ASSAULT_SPREAD_MINIMUM 1.5f
#define KEVIN_ASSAULT_SPREAD_MAXIMUM 3.5f
#define KEVIN_ASSAULT_TICKS_MINIMUM 45
#define KEVIN_ASSAULT_TICKS_MAXIMUM 75
#define KEVIN_ASSAULT_YOU_MOVED 3.f

/* his look turns as a critically damped spring of this stiffness (radians
per second; the larger, the quicker), no faster than the turn speed
(radians a second): calm, glancing, and in combat, where it follows the
AI's aim closely enough to hit */
#define KEVIN_LOOK_STIFFNESS_CALM 5.f
#define KEVIN_LOOK_STIFFNESS_GLANCE 7.f
#define KEVIN_LOOK_STIFFNESS_COMBAT 25.f
#define KEVIN_LOOK_TURN_SPEED_CALM 1.6f
#define KEVIN_LOOK_TURN_SPEED_GLANCE 2.4f
#define KEVIN_LOOK_TURN_SPEED_COMBAT 20.f
/* ticks between glances (at you, or a look around), and their length */
#define KEVIN_GLANCE_INTERVAL_MINIMUM (4 * 30)
#define KEVIN_GLANCE_INTERVAL_MAXIMUM (9 * 30)
#define KEVIN_GLANCE_LENGTH_MINIMUM (1 * 30 + 6)
#define KEVIN_GLANCE_LENGTH_MAXIMUM (2 * 30 + 15)
/* a glance is at you this often (of 100) when you are near, else a look
around, this far (radians) to one side of the way he goes */
#define KEVIN_GLANCE_AT_YOU_PERCENT 50
#define KEVIN_GLANCE_AT_YOU_DISTANCE 12.f
#define KEVIN_GLANCE_AROUND_MINIMUM 0.35f
#define KEVIN_GLANCE_AROUND_MAXIMUM 0.9f
/* how much of the way he goes, or you face, his look ahead takes each tick
(of 1): the way changes at a path's corners and when he stops, and his
look should not swing with each */
#define KEVIN_LOOK_AHEAD_BLEND_MOVING 0.08f
#define KEVIN_LOOK_AHEAD_BLEND_STOPPED 0.03f
/* his look leads his body by no more than this (radians): a biped's aim is
held to a cone about its body, which turns only so fast, and an aim
outside it flips to the cone's other side */
#define KEVIN_LOOK_BODY_LIMIT 1.f
/* looking ahead he looks up or down the slope he goes along, or, stopped,
as you do, no more than these (radians) */
#define KEVIN_LOOK_SLOPE_MAXIMUM 0.6f
#define KEVIN_LOOK_YOUR_PITCH_MAXIMUM 0.35f
/* how far below level he looks ahead of himself (radians) */
#define KEVIN_LOOK_AHEAD_PITCH -0.08f
/* how fast he moves (world units a tick) before he looks the way he goes */
#define KEVIN_LOOK_MOVING_SPEED 0.02f
/* nearer than this (world units, across the ground) he doesn't look at you */
#define KEVIN_LOOK_AT_YOU_NEAREST 1.5f

enum
{
	_kevin_actor_mode_combat = 3,

	_kevin_glance_none = 0,
	_kevin_glance_you,
	_kevin_glance_around,
};

/* ---------- globals */

static struct
{
	boolean active;              /* this game has Kevin */
	long unit_index;             /* his, or NONE */
	long dead_ticks;             /* since he died, or was last not there */
	long actor_variant_index;    /* the level's, or NONE */
	long encounter_index;        /* an encounter of the level, or NONE */
	long log_ticks;
	long stranded_ticks;          /* ticks he has stayed at still_position */
	real_point3d still_position;
	boolean following;           /* walking to you, rather than stopped near you */

	/* his look: where it points and how fast it turns (radians a second) */
	boolean look_valid;
	real_euler_angles2d look;
	real_euler_angles2d look_velocity;
	real_vector2d travel;        /* the way he last went */
	real travel_pitch;           /* ... up or down */
	long glance_ticks;           /* left of this glance, or until the next */
	short glance;
	real glance_yaw;             /* a look around's, from the way he goes */
	long talking_ticks;          /* left of his speaking to you */
	unsigned long random_seed;   /* his own, not the game's */

	/* his unit's aim a tick ago, to find where it snaps */
	boolean last_aim_valid;
	real_vector3d last_aim;
	long snap_lines;             /* written this second */
	long encounter_ticks;        /* until the encounters near you are looked at again */
	long share_ticks;            /* until he is told again of the enemies near you */
	long weapon_switch_ticks;    /* until he may change weapon */

	/* his own position in a fight, near you */
	boolean assault_valid;
	real_point3d assault_point;
	long assault_surface_index;
	real_point3d assault_anchor; /* where you were when it was chosen */
	long assault_target_unit_index; /* ... and his target */
	long assault_ticks;
	long assault_tick;           /* the game tick it was last counted down in */

	/* what the combat log needs: enemies near you, what he knew of them,
	his last target and his shots */
	short enemies_near_you;
	short enemies_known;
	long last_target_unit_index;
	long last_weapon_index;
	short last_rounds_loaded;
	short shots;
	long outside_ticks;          /* ticks he has been outside the level */

	/* what your log needs */
	long your_last_weapon_index;
	short your_last_rounds_loaded;
	short your_shots;
	short your_trigger_ticks;
} kevin_bot_globals = { FALSE, NONE, 0, NONE, NONE, 0, 0, { 0.f, 0.f, 0.f }, FALSE };

/* ---------- private code */

static long kevin_bot_player_unit(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);

	return player_index != NONE ? player_get(player_index)->unit_index : NONE;
}

/* his unit, while it is there: none after a revert to a saved game from
before he was made (its index then another object's, or none), as a
cutscene's skip is; he is then made again at once */
static long kevin_bot_unit(
	void)
{
	struct unit_datum *unit;

	if (kevin_bot_globals.unit_index == NONE)
		return NONE;
	unit = unit_try_and_get(kevin_bot_globals.unit_index);
	if (!unit || unit->unit.player_index != NONE)
	{
		error(_error_silent, "kevin: his unit 0x%08X is gone (%s)",
			kevin_bot_globals.unit_index, unit ? "a player has it" : "deleted");
		kevin_bot_globals.unit_index = NONE;
		kevin_bot_globals.dead_ticks = KEVIN_RESPAWN_TICKS;
		return NONE;
	}

	return kevin_bot_globals.unit_index;
}

/* the first actor variant of the level whose tag name has kevin.actor_variant in it */
static long kevin_bot_find_actor_variant(
	void)
{
	char const *wanted = config_string("kevin.actor_variant");
	struct tag_iterator iterator;
	long tag_index;

	tag_iterator_new(&iterator, ACTOR_VARIANT_DEFINITION_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *name = tag_get_name(tag_index);

		if (name && wanted && wanted[0] && strstr(name, wanted))
			return tag_index;
	}

	return NONE;
}

/* the first encounter of the level on the structure BSP loaded: only an
active encounter's actors are updated (actors_update), and one on another
BSP is never active */
static long kevin_bot_find_encounter(
	void)
{
	struct scenario *scenario = global_scenario_get();
	long encounter_index;

	for (encounter_index = 0; encounter_index < scenario->ai_encounters.count; encounter_index++)
	{
		struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(
			&scenario->ai_encounters,
			encounter_index,
			struct encounter_definition);

		if (encounter->squads.count > 0 &&
			(encounter->runtime_structure_bsp_reference_index == NONE ||
			encounter->runtime_structure_bsp_reference_index == global_structure_bsp_index))
		{
			return encounter_index;
		}
	}

	return NONE;
}

static void kevin_bot_attach_actor(
	long unit_index)
{
	long actor_index;

	if (kevin_bot_globals.actor_variant_index == NONE)
		kevin_bot_globals.actor_variant_index = kevin_bot_find_actor_variant();
	if (kevin_bot_globals.encounter_index == NONE)
		kevin_bot_globals.encounter_index = kevin_bot_find_encounter();
	if (kevin_bot_globals.actor_variant_index == NONE)
	{
		error(_error_silent, "kevin: no actor variant in this level has \"%s\" in its name",
			config_string("kevin.actor_variant"));
		return;
	}

	actor_index = actor_create_for_unit(
		FALSE,
		unit_index,
		kevin_bot_globals.actor_variant_index,
		kevin_bot_globals.encounter_index,
		kevin_bot_globals.encounter_index != NONE ? 0 : NONE,
		FALSE,
		NONE,
		FALSE,
		_kevin_actor_state_alert,
		_kevin_actor_state_none,
		NONE,
		0);
	error(_error_silent, "kevin: actor 0x%08X (%s) for unit 0x%08X in encounter %ld",
		actor_index,
		tag_get_name(kevin_bot_globals.actor_variant_index),
		unit_index,
		kevin_bot_globals.encounter_index);

	/* never dormant or inactive: an encounter's actors sleep outside the area
	it has active, and an inactive encounter's are not updated at all
	(encounters.c, actors_update); Kevin is wherever you are */
	if (actor_index != NONE)
		actor_get(actor_index)->meta.force_active = TRUE;

	if (actor_index != NONE && kevin_bot_globals.encounter_index != NONE)
		encounter_get(kevin_bot_globals.encounter_index)->force_active = TRUE;
	kevin_bot_globals.encounter_ticks = 0;

	return;
}

static void kevin_bot_log(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct actor_datum *actor;
	real_point3d position;
	real_point3d yours;
	long your_unit_index = kevin_bot_player_unit(0);

	if (unit->unit.actor_index == NONE)
		return;
	actor = actor_get(unit->unit.actor_index);
	object_get_origin(unit_index, &position);
	if (your_unit_index != NONE)
		object_get_origin(your_unit_index, &yours);
	else
		yours = position;
	error(_error_silent,
		"kevin: mode %d action %d dormant %d frozen %d destination %d path %d at_destination %d "
		"moving %d thwarted %d throttle %.2f %.2f parent 0x%08X cluster %d "
		"vehicle 0x%08X passenger %d surface %ld destination surface %ld "
		"at %.1f %.1f %.1f, %.1f from you",
		actor->state.mode,
		actor->state.action,
		actor->meta.dormant,
		actor->meta.frozen,
		actor->control.path.destination_orders.destination_type,
		actor->control.path.path.valid,
		actor->control.path.at_destination,
		actor->control.moving,
		actor->control.movement_thwarted,
		actor->output.throttle.i,
		actor->output.throttle.j,
		unit->object.parent_object_index,
		unit->object.location.cluster_index,
		actor->input.vehicle_index,
		actor->input.vehicle_passenger,
		actor->input.pathfinding_surface_index,
		actor->control.path.destination.surface_index,
		position.x, position.y, position.z,
		distance3d(&position, &yours));

	return;
}

/* why the AI has no path for him: when it last planned one, where it
starts and wants to go, and whether a path from him to you can be found */
static void kevin_bot_log_path(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long actor_index = unit->unit.actor_index;
	struct actor_datum *actor = actor_get(actor_index);
	struct actor_debug_info *debug_info = &actor_debug_array[DATUM_INDEX_TO_ABSOLUTE_INDEX(actor_index)];
	struct biped_datum *biped = biped_try_and_get(unit_index);
	long your_unit_index = kevin_bot_player_unit(0);
	long your_surface_index = NONE;
	real_point3d your_point;
	struct path_input input;
	struct path_state state;
	short found = -1;

	actor_path_input_new(actor_index, &input);
	if (your_unit_index != NONE && biped_try_and_get(your_unit_index))
		your_surface_index = biped_find_pathfinding_surface_index(your_unit_index, &your_point);
	if (actor->input.pathfinding_surface_index != NONE && your_surface_index != NONE)
	{
		path_state_new(&input, &state, NULL);
		path_state_destination(&state, &your_point, your_surface_index, 0.f);
		found = path_state_find(&state) ? 1 : 0;
	}

	error(_error_silent,
		"kevin: path refreshed %ld ticks ago, timeslice %d flying %d midair %d support surface %ld "
		"start surface %ld at %.1f %.1f %.1f, destination %.1f %.1f %.1f (%.1f away), "
		"your surface %ld, path to you %d, following %d",
		debug_info->last_path_refresh == NONE ? -1 : game_time_get() - debug_info->last_path_refresh,
		actor->meta.timeslice,
		actor->state.flying,
		actor->input.in_midair,
		biped ? biped->biped.support_surface_index : NONE,
		actor->input.pathfinding_surface_index,
		actor->input.pathfinding_point.x, actor->input.pathfinding_point.y, actor->input.pathfinding_point.z,
		actor->control.path.destination.point.x,
		actor->control.path.destination.point.y,
		actor->control.path.destination.point.z,
		distance3d(&actor->input.position.body_position, &actor->control.path.destination.point),
		your_surface_index,
		found,
		kevin_bot_globals.following);

	return;
}

/* when he hasn't moved for a while and you are away from him (he is on
ground the AI can't path from, as an object's floor, or you are where he
can't follow), he is put just behind you */
/* when his unit's aim turns further in a tick than his look should, what
turned it: his look's state, the AI's aim and the unit's */
static void kevin_bot_log_snap(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct actor_datum *actor = actor_get(unit->unit.actor_index);
	long your_unit_index = kevin_bot_player_unit(0);
	real_vector3d aim = unit->unit.aiming_vector;
	real_euler_angles2d aim_angles;
	real_euler_angles2d desired_angles;
	real_euler_angles2d ai_angles;
	real_euler_angles2d facing_angles;
	real_point3d position;
	real_point3d yours;
	real turn;

	if ((game_time_get() % TICKS_PER_SECOND) == 0)
		kevin_bot_globals.snap_lines = 0;
	if (normalize3d(&aim) == 0.f)
		return;
	if (!kevin_bot_globals.last_aim_valid)
	{
		kevin_bot_globals.last_aim = aim;
		kevin_bot_globals.last_aim_valid = TRUE;
		return;
	}
	turn = dot_product3d(&aim, &kevin_bot_globals.last_aim);
	kevin_bot_globals.last_aim = aim;
	if (turn >= KEVIN_SNAP_COSINE || kevin_bot_globals.snap_lines >= KEVIN_SNAP_LINES)
		return;
	kevin_bot_globals.snap_lines++;

	euler_angles2d_from_vector3d(&aim_angles, &aim);
	euler_angles2d_from_vector3d(&desired_angles, &unit->unit.desired_aiming_vector);
	euler_angles2d_from_vector3d(&ai_angles, &actor->output.aiming_vector);
	euler_angles2d_from_vector3d(&facing_angles, &unit->object.forward);
	object_get_origin(unit_index, &position);
	if (your_unit_index != NONE)
		object_get_origin(your_unit_index, &yours);
	else
		yours = position;
	error(_error_silent,
		"kevin: snap %.2f rad; aim yaw %.2f pitch %.2f, his look %.2f %.2f (turning %.2f %.2f), "
		"desired aim %.2f %.2f, AI aim %.2f %.2f, body %.2f; mode %d action %d glance %d following %d "
		"speed %.3f, %.1f from you, parent 0x%08X animation state %d",
		arccosine(turn > 1.f ? 1.f : turn),
		aim_angles.yaw, aim_angles.pitch,
		kevin_bot_globals.look.yaw, kevin_bot_globals.look.pitch,
		kevin_bot_globals.look_velocity.yaw, kevin_bot_globals.look_velocity.pitch,
		desired_angles.yaw, desired_angles.pitch,
		ai_angles.yaw, ai_angles.pitch,
		facing_angles.yaw,
		actor->state.mode,
		actor->state.action,
		kevin_bot_globals.glance,
		kevin_bot_globals.following,
		magnitude3d(&unit->object.translational_velocity),
		distance3d(&position, &yours),
		unit->object.parent_object_index,
		unit->unit.animation.state);

	return;
}

/* the AI takes cover at its encounter's firing positions only, and his
first encounter's can be anywhere in the level: he moves to the encounter
of his team with the most of them near you */
static void kevin_bot_take_cover_near_you(
	long actor_index)
{
	struct actor_datum *actor = actor_get(actor_index);
	struct scenario *scenario = global_scenario_get();
	long your_unit_index = kevin_bot_player_unit(0);
	long current_index = actor->meta.encounter_index == NONE
		? NONE
		: DATUM_INDEX_TO_ABSOLUTE_INDEX(actor->meta.encounter_index);
	long best_index = NONE;
	long best_count = 0;
	long current_count = 0;
	long encounter_index;
	real_point3d yours;

	if (--kevin_bot_globals.encounter_ticks > 0 || your_unit_index == NONE)
		return;
	kevin_bot_globals.encounter_ticks = KEVIN_COVER_TICKS;
	object_get_origin(your_unit_index, &yours);

	for (encounter_index = 0; encounter_index < scenario->ai_encounters.count; encounter_index++)
	{
		struct encounter_definition *definition = TAG_BLOCK_GET_ELEMENT(
			&scenario->ai_encounters,
			encounter_index,
			struct encounter_definition);
		struct encounter_datum *encounter;
		long count = 0;
		long firing_position_index;

		if (definition->squads.count <= 0 ||
			(definition->runtime_structure_bsp_reference_index != NONE &&
				definition->runtime_structure_bsp_reference_index != global_structure_bsp_index))
		{
			continue;
		}
		encounter = encounter_get(encounter_index);
		/* his team is the player's; the marines' is an ally */
		if (!game_team_is_ally(actor->meta.team_index, encounter->team_index))
			continue;
		for (firing_position_index = 0; firing_position_index < definition->firing_positions.count; firing_position_index++)
		{
			struct firing_position_definition *firing_position = TAG_BLOCK_GET_ELEMENT(
				&definition->firing_positions,
				firing_position_index,
				struct firing_position_definition);

			if (distance3d(&firing_position->position, &yours) < KEVIN_COVER_DISTANCE)
				count++;
		}
		if (encounter_index == current_index)
			current_count = count;
		if (count > best_count)
		{
			best_count = count;
			best_index = encounter_index;
		}
	}

	if (best_index != NONE && best_index != current_index)
		error(_error_silent, "kevin: cover search: team %d, encounter %ld has %ld firing positions near you, the best (%ld) %ld",
			actor->meta.team_index,
			current_index,
			current_count,
			best_index,
			best_count);
	if (best_index == NONE || best_index == current_index ||
		best_count < current_count + KEVIN_COVER_BETTER_BY)
	{
		return;
	}
	if (current_index != NONE)
		encounter_get(current_index)->force_active = FALSE;
	actor_change_encounter(actor_index, best_index, 0);
	encounter_get(best_index)->force_active = TRUE;
	kevin_bot_globals.encounter_index = best_index;
	error(_error_silent, "kevin: joined encounter %ld (%s): %ld firing positions near you, %ld in his last",
		best_index,
		TAG_BLOCK_GET_ELEMENT(&scenario->ai_encounters, best_index, struct encounter_definition)->name,
		best_count,
		current_count);

	return;
}

/* where he can stand behind you: on the ground you stand on, walked
along it from under you (so never inside a slope or past a wall), as high
above it as you are */
static boolean kevin_bot_ground_behind_you(
	long your_unit_index,
	real_point3d *point)
{
	struct unit_datum *you = unit_get(your_unit_index);
	long your_surface_index;
	long surface_index;
	real_point3d your_ground;
	real_point3d your_origin;
	real_point2d from;
	real_point2d want;

	if (!biped_try_and_get(your_unit_index))
		return FALSE;
	your_surface_index = biped_find_pathfinding_surface_index(your_unit_index, &your_ground);
	if (your_surface_index == NONE)
		return FALSE;
	object_get_origin(your_unit_index, &your_origin);

	from.x = your_ground.x;
	from.y = your_ground.y;
	want.x = your_ground.x - you->object.forward.i * KEVIN_BROUGHT_BEHIND;
	want.y = your_ground.y - you->object.forward.j * KEVIN_BROUGHT_BEHIND;
	surface_index = structure_surface_index_from_point(
		global_structure_bsp_get(),
		FALSE,
		&from,
		your_surface_index,
		&want);
	if (surface_index == NONE)
		return FALSE;
	collision_surface_project_point2d(global_collision_bsp_get(), surface_index, _z, TRUE, &want, point);
	point->z += your_origin.z - your_ground.z + KEVIN_BROUGHT_ABOVE;

	return TRUE;
}

/* puts him behind you; FALSE when there is no ground there for him (you
are in the air, or in a vehicle) */
static boolean kevin_bot_bring_to_you(
	long unit_index,
	char const *why)
{
	long your_unit_index = kevin_bot_player_unit(0);
	real_point3d point;

	if (your_unit_index == NONE || !kevin_bot_ground_behind_you(your_unit_index, &point))
		return FALSE;
	object_set_position(unit_index, &point, &unit_get(your_unit_index)->object.forward, NULL);
	kevin_bot_globals.stranded_ticks = 0;
	kevin_bot_globals.outside_ticks = 0;
	error(_error_silent, "kevin: %s; brought to you at %.1f %.1f %.1f", why, point.x, point.y, point.z);

	return TRUE;
}

/* when he hasn't moved for a while and you are away from him (he is on
ground the AI can't path from, as an object's floor, or you are where he
can't follow), he is put just behind you; at once, when he is outside the
level (fallen through it), where his view is drawn from nothing */
static void kevin_bot_bring_back_stranded(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long your_unit_index = kevin_bot_player_unit(0);
	real_point3d position;
	real_point3d yours;

	if (your_unit_index == NONE)
		return;
	if (unit->object.location.cluster_index == NONE && unit->object.parent_object_index == NONE)
	{
		if (++kevin_bot_globals.outside_ticks >= KEVIN_OUTSIDE_TICKS)
			kevin_bot_bring_to_you(unit_index, "outside the level");
		return;
	}
	kevin_bot_globals.outside_ticks = 0;

	object_get_origin(unit_index, &position);
	object_get_origin(your_unit_index, &yours);
	if (distance3d(&position, &kevin_bot_globals.still_position) > 0.5f ||
		distance3d(&position, &yours) < KEVIN_STRANDED_DISTANCE)
	{
		kevin_bot_globals.still_position = position;
		kevin_bot_globals.stranded_ticks = 0;
		return;
	}
	if (++kevin_bot_globals.stranded_ticks < KEVIN_STRANDED_TICKS)
		return;
	kevin_bot_bring_to_you(unit_index, "stuck away from you");

	return;
}

/* makes him behind you: the player's biped, in his armour colour, with the
level's first starting weapons (as players.c's player_spawn makes yours);
NONE while there is no ground behind you for him, or a cutscene plays */
static long kevin_bot_spawn(
	void)
{
	struct game_globals *game_globals = scenario_get_game_globals();
	struct game_globals_player_information *player_information;
	struct object_placement_data placement_data;
	long your_unit_index = kevin_bot_player_unit(0);
	real_rgb_color color;
	real_point3d point;
	long unit_index;
	short change_color_index;

	if (your_unit_index == NONE ||
		cinematic_in_progress() ||
		!game_globals ||
		game_globals->player_information.count <= 0 ||
		TEST_FLAG(unit_get(your_unit_index)->object.damage_flags, _object_dead_bit) ||
		!kevin_bot_ground_behind_you(your_unit_index, &point))
	{
		return NONE;
	}
	player_information = TAG_BLOCK_GET_ELEMENT(
		&game_globals->player_information,
		0,
		struct game_globals_player_information);
	if (player_information->player_unit.index == NONE)
		return NONE;

	object_placement_data_new(&placement_data, player_information->player_unit.index, NONE);
	placement_data.position = point;
	placement_data.forward = unit_get(your_unit_index)->object.forward;
	placement_data.up = *global_up3d;
	color.red = KEVIN_ARMOUR_RED;
	color.green = KEVIN_ARMOUR_GREEN;
	color.blue = KEVIN_ARMOUR_BLUE;
	for (change_color_index = 0; change_color_index < NUMBER_OF_OBJECT_CHANGE_COLORS; change_color_index++)
		placement_data.change_colors[change_color_index] = color;
	unit_index = object_new(&placement_data);
	if (unit_index == NONE)
		return NONE;
	/* (as player_spawn: the first profile, when the scenario has any) */
	if (*(long *)((byte *)global_scenario_get() + 0x348) != 0)
		player_add_equipment(unit_index, 0, TRUE);
	error(_error_silent, "kevin: made behind you, unit 0x%08X at %.1f %.1f %.1f",
		unit_index, point.x, point.y, point.z);

	return unit_index;
}

/* your shots, each second you fire: with what, at what and how far, to
see how you fight and so how he should */
static void kevin_bot_log_your_shots(
	void)
{
	long your_unit_index = kevin_bot_player_unit(0);
	long kevin_unit_index = kevin_bot_unit();
	struct unit_datum *you;
	short slot;
	long weapon_index;
	short rounds_loaded = NONE;

	if (your_unit_index == NONE)
		return;
	you = unit_get(your_unit_index);
	slot = you->unit.current_weapon_index;
	weapon_index = slot >= 0 && slot < MAXIMUM_WEAPONS_PER_UNIT ? you->unit.weapon_object_indices[slot] : NONE;
	if (weapon_index != NONE)
		rounds_loaded = weapon_get(weapon_index)->weapon.magazines[0].rounds_loaded;
	if (weapon_index == kevin_bot_globals.your_last_weapon_index && rounds_loaded < kevin_bot_globals.your_last_rounds_loaded)
		kevin_bot_globals.your_shots += kevin_bot_globals.your_last_rounds_loaded - rounds_loaded;
	if (TEST_FLAG(you->unit.control_flags, _unit_control_weapon_primary_trigger_bit))
		kevin_bot_globals.your_trigger_ticks++;
	kevin_bot_globals.your_last_weapon_index = weapon_index;
	kevin_bot_globals.your_last_rounds_loaded = rounds_loaded;

	if ((game_time_get() % TICKS_PER_SECOND) != 0)
		return;
	if (kevin_bot_globals.your_shots > 0 || kevin_bot_globals.your_trigger_ticks > 0)
	{
		long target_index = player_control_get_target_object_index(0);
		real_point3d yours;
		real_point3d target;
		real_point3d kevin;

		object_get_origin(your_unit_index, &yours);
		target = yours;
		if (target_index != NONE)
			object_get_origin(target_index, &target);
		kevin = yours;
		if (kevin_unit_index != NONE)
			object_get_origin(kevin_unit_index, &kevin);
		error(_error_silent,
			"you: %d shots (trigger %d ticks) with %s, at %s0x%08X (%s) %.1f away; Kevin %.1f from you",
			kevin_bot_globals.your_shots,
			kevin_bot_globals.your_trigger_ticks,
			weapon_index != NONE ? tag_name_strip_path(tag_get_name(weapon_get(weapon_index)->definition_index)) : "nothing",
			target_index != NONE ? "" : "nothing aimed at, ",
			target_index,
			target_index != NONE ? tag_name_strip_path(tag_get_name(object_get(target_index)->definition_index)) : "-",
			distance3d(&yours, &target),
			distance3d(&yours, &kevin));
	}
	kevin_bot_globals.your_shots = 0;
	kevin_bot_globals.your_trigger_ticks = 0;

	return;
}

static long kevin_bot_random_range(
	long minimum,
	long maximum)
{
	kevin_bot_globals.random_seed = kevin_bot_globals.random_seed * 1664525UL + 1013904223UL;

	return minimum + (long)((kevin_bot_globals.random_seed >> 8) % (unsigned long)(maximum - minimum + 1));
}

/* the shorter way from one angle to another */
static real kevin_bot_angle_difference(
	real from,
	real to)
{
	real difference = to - from;

	while (difference > _pi)
		difference -= 2.f * _pi;
	while (difference < -_pi)
		difference += 2.f * _pi;

	return difference;
}

/* one tick of a critically damped spring toward target, on one angle */
static void kevin_bot_spring(
	real *angle,
	real *velocity,
	real target,
	real stiffness,
	real maximum_speed)
{
	real dt = 1.f / TICKS_PER_SECOND;
	real offset = -kevin_bot_angle_difference(*angle, target);
	real t = stiffness * dt;
	/* e^-t, as a rational close to it, so that the step is the spring's
	exact one (a plain Euler step overshoots, and shakes, once 2t > 1) */
	real decay = 1.f / (1.f + t + 0.48f * t * t + 0.235f * t * t * t);
	real change = (*velocity + stiffness * offset) * dt;
	real step;

	*velocity = (*velocity - stiffness * change) * decay;
	step = (offset + change) * decay - offset;
	if (step > maximum_speed * dt)
		step = maximum_speed * dt;
	if (step < -maximum_speed * dt)
		step = -maximum_speed * dt;
	if (*velocity > maximum_speed)
		*velocity = maximum_speed;
	if (*velocity < -maximum_speed)
		*velocity = -maximum_speed;
	*angle += step;
	while (*angle > _pi)
		*angle -= 2.f * _pi;
	while (*angle < -_pi)
		*angle += 2.f * _pi;

	return;
}

/* the way from his head to your head; none when you are at arm's reach,
where it swings round with each step you take past him */
static boolean kevin_bot_direction_to_you(
	long unit_index,
	long your_unit_index,
	real_vector3d *direction)
{
	real_point3d head;
	real_point3d your_head;

	unit_get_head_position(unit_index, &head);
	unit_get_head_position(your_unit_index, &your_head);
	vector_from_points3d(&head, &your_head, direction);
	if (magnitude2d((real_vector2d const *)direction) < KEVIN_LOOK_AT_YOU_NEAREST)
		return FALSE;

	return normalize3d(direction) > 0.f;
}

/* the way he looks ahead: the way he goes or, stopped, the way you face
(toward where you are headed), each eased into */
static void kevin_bot_update_look_ahead(
	long unit_index,
	long your_unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	real_vector2d way;
	real blend;
	real length;
	real pitch;

	/* his own going only while he walks after you: a bump, or the AI
	shuffling him near you, is not where he is headed */
	way.i = unit->object.translational_velocity.i;
	way.j = unit->object.translational_velocity.j;
	blend = KEVIN_LOOK_AHEAD_BLEND_MOVING;
	if (kevin_bot_globals.following)
	{
		if (magnitude2d(&way) <= KEVIN_LOOK_MOVING_SPEED)
			return;
		pitch = arctangent(unit->object.translational_velocity.k, magnitude2d(&way));
		pitch = PIN(pitch, -KEVIN_LOOK_SLOPE_MAXIMUM, KEVIN_LOOK_SLOPE_MAXIMUM);
	}
	else
	{
		struct unit_datum *you;
		real_euler_angles2d your_angles;
		real_vector3d your_aim;

		if (your_unit_index == NONE)
			return;
		you = unit_get(your_unit_index);
		way.i = you->object.forward.i;
		way.j = you->object.forward.j;
		blend = KEVIN_LOOK_AHEAD_BLEND_STOPPED;
		pitch = 0.f;
		your_aim = you->unit.aiming_vector;
		if (normalize3d(&your_aim) > 0.f)
		{
			euler_angles2d_from_vector3d(&your_angles, &your_aim);
			pitch = PIN(your_angles.pitch, -KEVIN_LOOK_YOUR_PITCH_MAXIMUM, KEVIN_LOOK_YOUR_PITCH_MAXIMUM);
		}
	}
	if (normalize2d(&way) == 0.f)
		return;
	if (kevin_bot_globals.travel.i == 0.f && kevin_bot_globals.travel.j == 0.f)
		blend = 1.f;

	kevin_bot_globals.travel_pitch += (pitch - kevin_bot_globals.travel_pitch) * blend;
	kevin_bot_globals.travel.i += (way.i - kevin_bot_globals.travel.i) * blend;
	kevin_bot_globals.travel.j += (way.j - kevin_bot_globals.travel.j) * blend;
	length = magnitude2d(&kevin_bot_globals.travel);
	if (length > 0.f)
	{
		kevin_bot_globals.travel.i /= length;
		kevin_bot_globals.travel.j /= length;
	}

	return;
}

/* where he means to look, and how quickly he turns there (the spring's
stiffness, and its turn speed): in combat where the AI aims; at you while he
speaks to you; at you, or around, on a glance; else ahead */
static real kevin_bot_look_target(
	long actor_index,
	struct unit_control_data const *control_data,
	real_vector3d *target,
	real *maximum_speed)
{
	struct actor_datum *actor = actor_get(actor_index);
	long unit_index = actor->meta.unit_index;
	long your_unit_index = kevin_bot_player_unit(0);
	real_euler_angles2d angles;

	kevin_bot_update_look_ahead(unit_index, your_unit_index);

	if (actor->state.mode >= _kevin_actor_mode_combat)
	{
		*target = control_data->aiming_vector;
		kevin_bot_globals.glance = _kevin_glance_none;
		*maximum_speed = KEVIN_LOOK_TURN_SPEED_COMBAT;
		return KEVIN_LOOK_STIFFNESS_COMBAT;
	}

	*maximum_speed = KEVIN_LOOK_TURN_SPEED_GLANCE;
	if (kevin_bot_globals.talking_ticks > 0)
	{
		kevin_bot_globals.talking_ticks--;
		if (your_unit_index != NONE && kevin_bot_direction_to_you(unit_index, your_unit_index, target))
			return KEVIN_LOOK_STIFFNESS_GLANCE;
	}

	if (--kevin_bot_globals.glance_ticks <= 0)
	{
		if (kevin_bot_globals.glance == _kevin_glance_none)
		{
			real_point3d position;
			real_point3d yours;

			kevin_bot_globals.glance = _kevin_glance_around;
			if (your_unit_index != NONE)
			{
				object_get_origin(unit_index, &position);
				object_get_origin(your_unit_index, &yours);
				if (distance3d(&position, &yours) < KEVIN_GLANCE_AT_YOU_DISTANCE &&
					kevin_bot_random_range(1, 100) <= KEVIN_GLANCE_AT_YOU_PERCENT)
				{
					kevin_bot_globals.glance = _kevin_glance_you;
				}
			}
			kevin_bot_globals.glance_yaw = (real)kevin_bot_random_range(
				(long)(KEVIN_GLANCE_AROUND_MINIMUM * 1000.f),
				(long)(KEVIN_GLANCE_AROUND_MAXIMUM * 1000.f)) / 1000.f;
			if (kevin_bot_random_range(0, 1))
				kevin_bot_globals.glance_yaw = -kevin_bot_globals.glance_yaw;
			kevin_bot_globals.glance_ticks = kevin_bot_random_range(KEVIN_GLANCE_LENGTH_MINIMUM, KEVIN_GLANCE_LENGTH_MAXIMUM);
		}
		else
		{
			kevin_bot_globals.glance = _kevin_glance_none;
			kevin_bot_globals.glance_ticks = kevin_bot_random_range(KEVIN_GLANCE_INTERVAL_MINIMUM, KEVIN_GLANCE_INTERVAL_MAXIMUM);
		}
	}
	if (kevin_bot_globals.glance == _kevin_glance_you && your_unit_index != NONE &&
		kevin_bot_direction_to_you(unit_index, your_unit_index, target))
	{
		return KEVIN_LOOK_STIFFNESS_GLANCE;
	}

	if (kevin_bot_globals.travel.i == 0.f && kevin_bot_globals.travel.j == 0.f)
	{
		*target = control_data->looking_vector;
		*maximum_speed = KEVIN_LOOK_TURN_SPEED_CALM;
		return KEVIN_LOOK_STIFFNESS_CALM;
	}
	angles.yaw = arctangent(kevin_bot_globals.travel.j, kevin_bot_globals.travel.i);
	angles.pitch = kevin_bot_globals.travel_pitch + KEVIN_LOOK_AHEAD_PITCH;
	if (kevin_bot_globals.glance == _kevin_glance_around)
	{
		angles.yaw += kevin_bot_globals.glance_yaw;
		vector3d_from_euler_angles2d(target, &angles);
		return KEVIN_LOOK_STIFFNESS_GLANCE;
	}
	vector3d_from_euler_angles2d(target, &angles);
	*maximum_speed = KEVIN_LOOK_TURN_SPEED_CALM;

	return KEVIN_LOOK_STIFFNESS_CALM;
}

/* the first magazine's rounds: whether it can be reloaded, and whether the
weapon has anything to fire (a weapon without magazines, a plasma one, has) */
static boolean kevin_bot_weapon_needs_reload(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);
	struct weapon_magazine_definition *magazine_definition;

	if (definition->weapon.magazines.count <= 0)
		return FALSE;
	magazine_definition = TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0, struct weapon_magazine_definition);

	return weapon->weapon.magazines[0].rounds_loaded < magazine_definition->rounds_loaded_maximum &&
		weapon->weapon.magazines[0].rounds_total > 0;
}

static boolean kevin_bot_weapon_has_loaded_rounds(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);

	return definition->weapon.magazines.count <= 0 || weapon->weapon.magazines[0].rounds_loaded > 0;
}

static boolean kevin_bot_weapon_has_rounds(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);

	return definition->weapon.magazines.count <= 0 ||
		weapon->weapon.magazines[0].rounds_loaded > 0 ||
		weapon->weapon.magazines[0].rounds_total > 0;
}

/* how far a weapon is fought at: its autoaim distance, less for one
without a zoom (an assault rifle's sprays too wide to be worth it so far,
where a pistol's is not) */
static real kevin_bot_weapon_reach(
	long weapon_index)
{
	struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
	real reach = definition->weapon.aim_assist_parameters.autoaim_distance;

	return definition->weapon.zoom_level_count > 0 ? reach : reach * KEVIN_WEAPON_UNZOOMED_REACH;
}

/* in combat, the weapon he carries of shortest reach that still reaches
his target (else the furthest reaching); out of it, his weapon reloaded */
static void kevin_bot_choose_weapon(
	long actor_index,
	struct unit_control_data *control_data)
{
	struct actor_datum *actor = actor_get(actor_index);
	struct unit_datum *unit = unit_get(actor->meta.unit_index);
	long target_unit_index = actor_target_unit_index(actor_index);
	short current_slot = unit->unit.current_weapon_index;
	long current_weapon_index = current_slot >= 0 && current_slot < MAXIMUM_WEAPONS_PER_UNIT
		? unit->unit.weapon_object_indices[current_slot]
		: NONE;
	real target_distance = REAL_MAX;

	if (kevin_bot_globals.weapon_switch_ticks > 0)
		kevin_bot_globals.weapon_switch_ticks--;

	if (target_unit_index != NONE)
	{
		real_point3d position;
		real_point3d target;

		object_get_origin(actor->meta.unit_index, &position);
		object_get_origin(target_unit_index, &target);
		target_distance = distance3d(&position, &target);
	}

	/* his magazine empty in a fight: with an enemy near, his other weapon,
	quicker than reloading; with none near, reloaded */
	if (actor->state.mode >= _kevin_actor_mode_combat &&
		current_weapon_index != NONE &&
		!kevin_bot_weapon_has_loaded_rounds(current_weapon_index))
	{
		short slot;

		if (target_distance > KEVIN_RELOAD_SAFE_DISTANCE && kevin_bot_weapon_needs_reload(current_weapon_index))
		{
			SET_FLAG(control_data->control_flags, _unit_control_weapon_reload_bit, TRUE);
			return;
		}

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long weapon_index = unit->unit.weapon_object_indices[slot];

			if (slot != current_slot && weapon_index != NONE && kevin_bot_weapon_has_loaded_rounds(weapon_index))
			{
				control_data->weapon_index = slot;
				kevin_bot_globals.weapon_switch_ticks = KEVIN_WEAPON_SWITCH_TICKS;
				error(_error_silent, "kevin: %s empty; weapon %d (%s) rather than reloading",
					tag_name_strip_path(tag_get_name(weapon_get(current_weapon_index)->definition_index)),
					slot,
					tag_name_strip_path(tag_get_name(weapon_get(weapon_index)->definition_index)));
				return;
			}
		}
	}

	if (actor->state.mode >= _kevin_actor_mode_combat && target_unit_index != NONE)
	{
		real distance;
		real best_reach = 0.f;
		real furthest_reach = 0.f;
		short best_slot = NONE;
		short furthest_slot = NONE;
		short slot;

		if (kevin_bot_globals.weapon_switch_ticks > 0)
			return;
		distance = target_distance;
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long weapon_index = unit->unit.weapon_object_indices[slot];
			real reach;

			/* a weapon to fire now; or, with no enemy near, one to reload */
			if (weapon_index == NONE ||
				!(kevin_bot_weapon_has_loaded_rounds(weapon_index) ||
					(distance > KEVIN_RELOAD_SAFE_DISTANCE && kevin_bot_weapon_has_rounds(weapon_index))))
			{
				continue;
			}
			reach = kevin_bot_weapon_reach(weapon_index);
			if (reach >= distance && (best_slot == NONE || reach < best_reach))
			{
				best_reach = reach;
				best_slot = slot;
			}
			if (furthest_slot == NONE || reach > furthest_reach)
			{
				furthest_reach = reach;
				furthest_slot = slot;
			}
		}
		if (best_slot == NONE)
		{
			best_slot = furthest_slot;
			best_reach = furthest_reach;
		}
		if (best_slot != NONE && best_slot != current_slot)
		{
			control_data->weapon_index = best_slot;
			kevin_bot_globals.weapon_switch_ticks = KEVIN_WEAPON_SWITCH_TICKS;
			error(_error_silent, "kevin: target %.1f away; weapon %d (%s, reach %.1f) for %d",
				distance,
				best_slot,
				tag_name_strip_path(tag_get_name(weapon_get(unit->unit.weapon_object_indices[best_slot])->definition_index)),
				best_reach,
				current_slot);
		}
	}
	else if (actor->state.mode < _kevin_actor_mode_combat && current_weapon_index != NONE)
	{
		/* out of a fight, every weapon reloaded: the one in hand, then each
		other, taken out to be */
		if (kevin_bot_weapon_needs_reload(current_weapon_index))
		{
			SET_FLAG(control_data->control_flags, _unit_control_weapon_reload_bit, TRUE);
		}
		else if (kevin_bot_globals.weapon_switch_ticks <= 0)
		{
			short slot;

			for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			{
				long weapon_index = unit->unit.weapon_object_indices[slot];

				if (slot != current_slot && weapon_index != NONE && kevin_bot_weapon_needs_reload(weapon_index))
				{
					control_data->weapon_index = slot;
					kevin_bot_globals.weapon_switch_ticks = KEVIN_WEAPON_SWITCH_TICKS;
					break;
				}
			}
		}
	}

	return;
}

/* he learns of the enemies near you, and of the one you aim at: the AI
knows only what its own unit has seen or heard */
static void kevin_bot_share_enemies(
	long actor_index)
{
	struct actor_datum *actor = actor_get(actor_index);
	long your_unit_index = kevin_bot_player_unit(0);
	long aimed_object_index;
	struct object_iterator iterator;
	struct unit_datum *other;
	real_point3d yours;

	if (--kevin_bot_globals.share_ticks > 0 || your_unit_index == NONE || !actor->meta.active)
		return;
	kevin_bot_globals.share_ticks = KEVIN_SHARE_TICKS;
	object_get_origin(your_unit_index, &yours);
	aimed_object_index = player_control_get_target_object_index(0);
	kevin_bot_globals.enemies_near_you = 0;
	kevin_bot_globals.enemies_known = 0;

	object_iterator_new(&iterator, _object_mask_biped, 0);
	while ((other = (struct unit_datum *)object_iterator_next(&iterator)) != NULL)
	{
		long other_index = iterator.index;
		real_point3d position;
		real distance;

		if (other_index == actor->meta.unit_index ||
			TEST_FLAG(other->object.damage_flags, _object_dead_bit) ||
			!game_team_is_enemy(actor->meta.team_index, other->object.owner_team_index))
		{
			continue;
		}
		object_get_origin(other_index, &position);
		distance = distance3d(&position, &yours);
		if (distance < KEVIN_SHARED_ENEMY_DISTANCE ||
			(other_index == aimed_object_index && distance < KEVIN_AIMED_ENEMY_DISTANCE))
		{
			if (distance < KEVIN_SHARED_ENEMY_DISTANCE)
				kevin_bot_globals.enemies_near_you++;
			if (prop_get_base_by_unit_index(actor_index, other_index, FALSE, FALSE) != NONE)
				kevin_bot_globals.enemies_known++;
			prop_get_base_by_unit_index(actor_index, other_index, TRUE, TRUE);
		}
	}

	return;
}

/* ai_debug.h's firing decisions: why the AI fired or held fire */
static char const *kevin_bot_firing_decision_name(
	short decision)
{
	static char const *names[] =
	{
		"disabled", "busy", "wrong target", "no target", "outside the active area",
		"not visible", "outside range", "blocked", "holding for a line", "holding",
		"pausing for a line", "pausing", "wild", "burst", "in midair", "not crouching",
		"not standing", "not stationary", "underwater", "inside minimum range",
	};

	return decision >= 0 && decision < (short)NUMBEROF(names) ? names[decision] : "none";
}

/* each second of a fight: his target and how he sees it, his weapon, his
shots, the enemies near you and how many of them he knew; and a line when
his target changes */
static void kevin_bot_log_combat(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long actor_index = unit->unit.actor_index;
	struct actor_datum *actor = actor_get(actor_index);
	long target_unit_index = actor_target_unit_index(actor_index);
	short slot = unit->unit.current_weapon_index;
	long weapon_index = slot >= 0 && slot < MAXIMUM_WEAPONS_PER_UNIT ? unit->unit.weapon_object_indices[slot] : NONE;
	short rounds_loaded = NONE;
	long your_unit_index = kevin_bot_player_unit(0);
	real_point3d position;
	real_point3d yours;
	real_point3d target;

	if (weapon_index != NONE && weapon_get(weapon_index)->weapon.magazines[0].rounds_loaded >= 0)
		rounds_loaded = weapon_get(weapon_index)->weapon.magazines[0].rounds_loaded;
	if (weapon_index == kevin_bot_globals.last_weapon_index && rounds_loaded < kevin_bot_globals.last_rounds_loaded)
		kevin_bot_globals.shots += kevin_bot_globals.last_rounds_loaded - rounds_loaded;
	kevin_bot_globals.last_weapon_index = weapon_index;
	kevin_bot_globals.last_rounds_loaded = rounds_loaded;

	object_get_origin(unit_index, &position);
	yours = position;
	if (your_unit_index != NONE)
		object_get_origin(your_unit_index, &yours);
	target = position;
	if (target_unit_index != NONE)
		object_get_origin(target_unit_index, &target);

	if (target_unit_index != kevin_bot_globals.last_target_unit_index)
	{
		kevin_bot_globals.last_target_unit_index = target_unit_index;
		if (target_unit_index != NONE)
		{
			error(_error_silent, "kevin: new target 0x%08X (%s) at %.1f %.1f %.1f, %.1f from him, %.1f from you%s",
				target_unit_index,
				tag_name_strip_path(tag_get_name(unit_get(target_unit_index)->definition_index)),
				target.x, target.y, target.z,
				distance3d(&position, &target),
				distance3d(&yours, &target),
				target_unit_index == player_control_get_target_object_index(0) ? ", the one you aim at" : "");
		}
	}

	if ((game_time_get() % TICKS_PER_SECOND) != 0)
		return;
	if (actor->state.mode >= _kevin_actor_mode_combat)
	{
		long prop_index = target_unit_index != NONE
			? prop_get_base_by_unit_index(actor_index, target_unit_index, FALSE, FALSE)
			: NONE;
		struct prop_datum *prop = prop_index != NONE ? prop_get(prop_index) : NULL;

		error(_error_silent,
			"kevin: combat: action %d, target 0x%08X %.1f from him %.1f from you (prop state %d sight %d), "
			"%d shots this second, weapon %d (%s) %d loaded, trigger %.2f, %s; "
			"%d enemies near you, he knew %d; destination %d %.1f from you%s",
			actor->state.action,
			target_unit_index,
			distance3d(&position, &target),
			distance3d(&yours, &target),
			prop ? prop->state : -1,
			prop ? prop->line_of_sight : -1,
			kevin_bot_globals.shots,
			slot,
			weapon_index != NONE ? tag_name_strip_path(tag_get_name(weapon_get(weapon_index)->definition_index)) : "none",
			rounds_loaded,
			unit->unit.primary_trigger,
			kevin_bot_firing_decision_name(actor_debug_array[DATUM_INDEX_TO_ABSOLUTE_INDEX(actor_index)].firing_decision),
			kevin_bot_globals.enemies_near_you,
			kevin_bot_globals.enemies_known,
			actor->control.path.destination_orders.destination_type,
			distance3d(&actor->control.path.destination.point, &yours),
			kevin_bot_globals.assault_valid ? ", his own assault position" : "");
	}
	kevin_bot_globals.shots = 0;

	return;
}

/* his weapons, when he spawns */
static void kevin_bot_log_weapons(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];
		struct weapon_definition *definition;

		if (weapon_index == NONE)
			continue;
		definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
		error(_error_silent, "kevin: weapon %d: %s, autoaim %.1f, zoom levels %d, reach %.1f",
			slot,
			tag_name_strip_path(tag_get_name(weapon_get(weapon_index)->definition_index)),
			definition->weapon.aim_assist_parameters.autoaim_distance,
			definition->weapon.zoom_level_count,
			kevin_bot_weapon_reach(weapon_index));
	}

	return;
}

/* his own position in a fight: ahead of you toward his target, to a side,
on ground he can reach from yours */
static boolean kevin_bot_assault(
	long actor_index,
	long your_unit_index,
	real_point3d const *your_point,
	long your_surface_index,
	boolean *result)
{
	struct actor_datum *actor = actor_get(actor_index);
	long target_unit_index = actor_target_unit_index(actor_index);

	/* kept until its time is up (counted once a tick: this is asked more
	often), you move on, or his target changes; not when he gets there,
	where he stands and fights */
	if (kevin_bot_globals.assault_tick != game_time_get())
	{
		kevin_bot_globals.assault_tick = game_time_get();
		kevin_bot_globals.assault_ticks--;
	}
	if (!kevin_bot_globals.assault_valid ||
		kevin_bot_globals.assault_ticks <= 0 ||
		distance3d(&kevin_bot_globals.assault_anchor, your_point) > KEVIN_ASSAULT_YOU_MOVED ||
		target_unit_index != kevin_bot_globals.assault_target_unit_index)
	{
		real_vector2d toward;
		real advance = 0.f;
		real side;
		real_point2d from;
		real_point2d want;
		long surface_index;

		toward.i = unit_get(your_unit_index)->object.forward.i;
		toward.j = unit_get(your_unit_index)->object.forward.j;
		if (target_unit_index != NONE)
		{
			real_point3d target;
			real_vector2d to_target;
			real distance;

			object_get_origin(target_unit_index, &target);
			to_target.i = target.x - your_point->x;
			to_target.j = target.y - your_point->y;
			distance = normalize2d(&to_target);
			if (distance > 0.f)
			{
				toward = to_target;
				advance = PIN(distance - KEVIN_ASSAULT_TARGET_NEAREST, 0.f, KEVIN_ASSAULT_ADVANCE);
			}
		}
		if (normalize2d(&toward) == 0.f)
		{
			toward.i = 1.f;
			toward.j = 0.f;
		}
		side = (real)kevin_bot_random_range(
			(long)(KEVIN_ASSAULT_SPREAD_MINIMUM * 100.f),
			(long)(KEVIN_ASSAULT_SPREAD_MAXIMUM * 100.f)) / 100.f;
		if (kevin_bot_random_range(0, 1))
			side = -side;

		from.x = your_point->x;
		from.y = your_point->y;
		want.x = your_point->x + toward.i * advance - toward.j * side;
		want.y = your_point->y + toward.j * advance + toward.i * side;
		surface_index = structure_surface_index_from_point(
			global_structure_bsp_get(),
			actor->emotions.ignorant_of_broken_surfaces,
			&from,
			your_surface_index,
			&want);
		if (surface_index == NONE)
		{
			kevin_bot_globals.assault_point = *your_point;
			kevin_bot_globals.assault_surface_index = your_surface_index;
		}
		else
		{
			collision_surface_project_point2d(
				global_collision_bsp_get(),
				surface_index,
				_z,
				TRUE,
				&want,
				&kevin_bot_globals.assault_point);
			kevin_bot_globals.assault_surface_index = surface_index;
		}
		kevin_bot_globals.assault_anchor = *your_point;
		kevin_bot_globals.assault_target_unit_index = target_unit_index;
		kevin_bot_globals.assault_ticks = kevin_bot_random_range(KEVIN_ASSAULT_TICKS_MINIMUM, KEVIN_ASSAULT_TICKS_MAXIMUM);
		kevin_bot_globals.assault_valid = TRUE;
		error(_error_silent, "kevin: assault position %.1f %.1f %.1f: %.1f ahead of you toward 0x%08X, %.1f to the side",
			kevin_bot_globals.assault_point.x,
			kevin_bot_globals.assault_point.y,
			kevin_bot_globals.assault_point.z,
			advance,
			target_unit_index,
			side);
	}

	*result = actor_move_to_point(
		actor_index,
		&kevin_bot_globals.assault_point,
		kevin_bot_globals.assault_surface_index,
		NONE);

	return TRUE;
}

/* ---------- public code */

real kevin_bot_target_weight(
	long actor_index,
	long unit_index,
	real weight)
{
	struct actor_datum *actor = actor_get(actor_index);
	long your_unit_index;
	real_point3d position;
	real_point3d yours;

	if (weight <= 0.f || unit_index == NONE || !kevin_bot_drives_unit(actor->meta.unit_index))
		return weight;
	your_unit_index = kevin_bot_player_unit(0);
	if (your_unit_index == NONE)
		return weight;

	if (unit_index == player_control_get_target_object_index(0))
		weight += KEVIN_TARGET_WEIGHT_AIMED;
	object_get_origin(unit_index, &position);
	object_get_origin(your_unit_index, &yours);
	if (distance3d(&position, &yours) < KEVIN_SHARED_ENEMY_DISTANCE)
		weight += KEVIN_TARGET_WEIGHT_NEAR_YOU;

	return weight;
}

real kevin_bot_maximum_firing_range(
	long actor_index,
	real range)
{
	struct actor_datum *actor = actor_get(actor_index);
	struct unit_datum *unit;
	short slot;
	long weapon_index;
	real reach;

	if (range <= 0.f || !kevin_bot_drives_unit(actor->meta.unit_index))
		return range;
	unit = unit_get(actor->meta.unit_index);
	slot = unit->unit.current_weapon_index;
	weapon_index = slot >= 0 && slot < MAXIMUM_WEAPONS_PER_UNIT ? unit->unit.weapon_object_indices[slot] : NONE;
	if (weapon_index == NONE)
		return range;
	reach = weapon_definition_get(weapon_get(weapon_index)->definition_index)->weapon.aim_assist_parameters.autoaim_distance *
		KEVIN_FIRING_RANGE_SCALE;

	return reach > range ? reach : range;
}

boolean kevin_bot_moves_freely(
	long actor_index)
{
	return kevin_bot_drives_unit(actor_get(actor_index)->meta.unit_index);
}

real kevin_bot_fire_scale(
	long actor_index,
	short what)
{
	if (!kevin_bot_drives_unit(actor_get(actor_index)->meta.unit_index))
		return 1.f;

	switch (what)
	{
	case KEVIN_FIRE_BURST_SEPARATION:
		return KEVIN_BURST_SEPARATION_SCALE;
	case KEVIN_FIRE_BURST_DURATION:
		return KEVIN_BURST_DURATION_SCALE;
	case KEVIN_FIRE_FIRST_BURST_DELAY:
		return KEVIN_FIRST_BURST_DELAY_SCALE;
	}

	return 1.f;
}

void kevin_bot_adjust_control(
	long actor_index,
	struct unit_control_data *control_data)
{
	struct actor_datum *actor = actor_get(actor_index);
	real_vector3d target;
	real_euler_angles2d target_angles;
	real stiffness;
	real maximum_speed;

	if (!kevin_bot_drives_unit(actor->meta.unit_index))
		return;

	stiffness = kevin_bot_look_target(actor_index, control_data, &target, &maximum_speed);
	if (normalize3d(&target) == 0.f)
		return;
	euler_angles2d_from_vector3d(&target_angles, &target);

	if (!kevin_bot_globals.look_valid)
	{
		real_vector3d aiming = unit_get(actor->meta.unit_index)->unit.aiming_vector;

		if (normalize3d(&aiming) > 0.f)
			euler_angles2d_from_vector3d(&kevin_bot_globals.look, &aiming);
		else
			kevin_bot_globals.look = target_angles;
		kevin_bot_globals.look_velocity.yaw = 0.f;
		kevin_bot_globals.look_velocity.pitch = 0.f;
		kevin_bot_globals.look_valid = TRUE;
	}
	kevin_bot_spring(&kevin_bot_globals.look.yaw, &kevin_bot_globals.look_velocity.yaw, target_angles.yaw, stiffness, maximum_speed);
	kevin_bot_spring(&kevin_bot_globals.look.pitch, &kevin_bot_globals.look_velocity.pitch, target_angles.pitch, stiffness, maximum_speed);
	if (kevin_bot_globals.look.pitch > 1.5f)
		kevin_bot_globals.look.pitch = 1.5f;
	if (kevin_bot_globals.look.pitch < -1.5f)
		kevin_bot_globals.look.pitch = -1.5f;

	/* no further ahead of his body than its aim allows; it catches up, as
	his facing is his look (below) */
	{
		struct unit_datum *unit = unit_get(actor->meta.unit_index);
		real body_yaw = arctangent(unit->object.forward.j, unit->object.forward.i);
		real lead = kevin_bot_angle_difference(body_yaw, kevin_bot_globals.look.yaw);

		if (lead > KEVIN_LOOK_BODY_LIMIT)
		{
			kevin_bot_globals.look.yaw = body_yaw + KEVIN_LOOK_BODY_LIMIT;
			if (kevin_bot_globals.look_velocity.yaw > 0.f)
				kevin_bot_globals.look_velocity.yaw = 0.f;
		}
		else if (lead < -KEVIN_LOOK_BODY_LIMIT)
		{
			kevin_bot_globals.look.yaw = body_yaw - KEVIN_LOOK_BODY_LIMIT;
			if (kevin_bot_globals.look_velocity.yaw < 0.f)
				kevin_bot_globals.look_velocity.yaw = 0.f;
		}
		kevin_bot_globals.look.yaw = kevin_bot_angle_difference(0.f, kevin_bot_globals.look.yaw);
	}

	vector3d_from_euler_angles2d(&control_data->aiming_vector, &kevin_bot_globals.look);
	control_data->looking_vector = control_data->aiming_vector;

	/* his body faces where he looks: a biped aims only so far from its
	body, and turned the AI's way the body dragged his aim round with it,
	then flipped it to the cone's other side. The throttle is along the
	body (forward, and to its left), so the way the AI moves him is kept by
	turning the throttle with it */
	{
		real_vector2d old_forward;
		real_vector2d new_forward;
		real_vector2d way;

		old_forward.i = control_data->facing_vector.i;
		old_forward.j = control_data->facing_vector.j;
		new_forward.i = control_data->aiming_vector.i;
		new_forward.j = control_data->aiming_vector.j;
		if (normalize2d(&old_forward) > 0.f && normalize2d(&new_forward) > 0.f)
		{
			way.i = control_data->throttle.i * old_forward.i - control_data->throttle.j * old_forward.j;
			way.j = control_data->throttle.i * old_forward.j + control_data->throttle.j * old_forward.i;
			control_data->throttle.i = way.i * new_forward.i + way.j * new_forward.j;
			control_data->throttle.j = -way.i * new_forward.j + way.j * new_forward.i;
			control_data->facing_vector.i = new_forward.i;
			control_data->facing_vector.j = new_forward.j;
			control_data->facing_vector.k = 0.f;
		}
	}

	kevin_bot_choose_weapon(actor_index, control_data);

	return;
}

void kevin_bot_set_talking(
	long ticks)
{
	kevin_bot_globals.talking_ticks = ticks;

	return;
}

void kevin_bot_game_begin(
	void)
{
	kevin_bot_globals.active = FALSE;
	kevin_bot_globals.unit_index = NONE;
	kevin_bot_globals.dead_ticks = KEVIN_RESPAWN_TICKS;
	kevin_bot_globals.actor_variant_index = NONE;
	kevin_bot_globals.encounter_index = NONE;
	kevin_bot_globals.look_valid = FALSE;
	kevin_bot_globals.last_aim_valid = FALSE;
	kevin_bot_globals.glance = _kevin_glance_none;
	kevin_bot_globals.glance_ticks = KEVIN_GLANCE_INTERVAL_MINIMUM;
	kevin_bot_globals.talking_ticks = 0;
	kevin_bot_globals.random_seed = 0x4B455649UL;
	if (config_boolean("kevin.enabled") &&
		global_scenario_get()->type == _scenario_type_solo &&
		player_spawn_count == 1)
	{
		kevin_bot_globals.active = TRUE;
		error(_error_silent, "kevin: this level has Kevin");
	}

	return;
}

long kevin_bot_unit_index(
	void)
{
	return kevin_bot_globals.active ? kevin_bot_unit() : NONE;
}

void kevin_bot_update(
	void)
{
	long unit_index;
	struct unit_datum *unit;

	if (!kevin_bot_globals.active || !ai_globals->ai_initialized_for_map)
		return;
	unit_index = kevin_bot_unit();
	if (unit_index != NONE && !TEST_FLAG(unit_get(unit_index)->object.damage_flags, _object_dead_bit))
	{
		kevin_bot_globals.dead_ticks = 0;
	}
	else
	{
		/* dead, or not made yet: made (again) behind you after a while */
		if (unit_index != NONE && kevin_bot_globals.dead_ticks == 0)
		{
			real_point3d position;

			object_get_origin(unit_index, &position);
			error(_error_silent, "kevin: he died at %.1f %.1f %.1f", position.x, position.y, position.z);
		}
		if (++kevin_bot_globals.dead_ticks < KEVIN_RESPAWN_TICKS)
			return;
		unit_index = kevin_bot_spawn();
		if (unit_index == NONE)
			return;
		kevin_bot_globals.unit_index = unit_index;
		kevin_bot_globals.dead_ticks = 0;
	}
	unit = unit_get(unit_index);

	if (unit->unit.actor_index == NONE)
	{
		/* the AI can't walk him off an object he is attached to */
		if (unit->object.parent_object_index != NONE)
		{
			error(_error_silent, "kevin: detached from 0x%08X", unit->object.parent_object_index);
			object_detach(unit_index);
		}
		kevin_bot_globals.look_valid = FALSE;
		kevin_bot_globals.last_aim_valid = FALSE;
		kevin_bot_globals.assault_valid = FALSE;
		kevin_bot_globals.last_target_unit_index = NONE;
		kevin_bot_attach_actor(unit_index);
		kevin_bot_log_weapons(unit_index);
	}
	if (unit->unit.actor_index != NONE)
	{
		struct actor_datum *actor = actor_get(unit->unit.actor_index);
		short destination_type = actor->control.path.destination_orders.destination_type;
		boolean result;

		/* the AI asks for a firing position only when it picks a new one, so
		he is kept with you each tick too; a prop (a vehicle he is getting
		into, something he searches) is left to the AI */
		if (destination_type != _destination_prop)
		{
			kevin_bot_move_to_you(
				unit->unit.actor_index,
				destination_type == _destination_firing_position
					? actor->control.path.destination_orders.firing_position_index
					: NONE,
				&result);
		}
		kevin_bot_bring_back_stranded(unit_index);
		kevin_bot_take_cover_near_you(unit->unit.actor_index);
		kevin_bot_share_enemies(unit->unit.actor_index);
		kevin_bot_log_combat(unit_index);
		kevin_bot_log_your_shots();
		kevin_bot_log_snap(unit_index);
	}
	if (++kevin_bot_globals.log_ticks >= KEVIN_LOG_TICKS)
	{
		kevin_bot_globals.log_ticks = 0;
		kevin_bot_log(unit_index);
		if (unit->unit.actor_index != NONE)
			kevin_bot_log_path(unit_index);
	}

	return;
}

boolean kevin_bot_move_to_you(
	long actor_index,
	short firing_position_index,
	boolean *result)
{
	struct actor_datum *actor = actor_get(actor_index);
	long your_unit_index;
	long your_surface_index;
	real_point3d your_point;
	real distance;

	if (!kevin_bot_globals.active ||
		actor->meta.unit_index == NONE ||
		actor->meta.unit_index != kevin_bot_unit())
	{
		return FALSE;
	}
	your_unit_index = kevin_bot_player_unit(0);
	if (your_unit_index == NONE || !biped_try_and_get(your_unit_index))
		return FALSE;
	your_surface_index = biped_find_pathfinding_surface_index(your_unit_index, &your_point);
	if (your_surface_index == NONE)
		return FALSE;
	distance = distance3d(&actor->input.position.body_position, &your_point);

	/* a firing position near you is the AI's to choose */
	if (actor->meta.encounter_index != NONE && firing_position_index != NONE)
	{
		struct encounter_definition *encounter = TAG_BLOCK_GET_ELEMENT(
			&global_scenario_get()->ai_encounters,
			DATUM_INDEX_TO_ABSOLUTE_INDEX(actor->meta.encounter_index),
			struct encounter_definition);

		if (firing_position_index < encounter->firing_positions.count)
		{
			struct firing_position_definition *firing_position = TAG_BLOCK_GET_ELEMENT(
				&encounter->firing_positions,
				firing_position_index,
				struct firing_position_definition);

			if (distance3d(&firing_position->position, &your_point) < KEVIN_FOLLOW_FIRING_RADIUS)
				return FALSE;
		}
	}

	/* in combat, near you, the AI's own moves (evading, charging) are its
	to make; else, as its firing positions are far from you, he takes his
	own position, pushing on with you */
	if (actor->state.mode >= _kevin_actor_mode_combat)
	{
		struct actor_destination_orders const *orders = &actor->control.path.destination_orders;

		kevin_bot_globals.following = FALSE;
		if (orders->destination_type == _destination_raw_location &&
			!(kevin_bot_globals.assault_valid &&
				distance3d(&orders->raw.point, &kevin_bot_globals.assault_point) < 0.1f) &&
			distance3d(&orders->raw.point, &your_point) < KEVIN_COMBAT_LEASH)
		{
			return FALSE;
		}

		return kevin_bot_assault(actor_index, your_unit_index, &your_point, your_surface_index, result);
	}
	kevin_bot_globals.assault_valid = FALSE;

	if (distance > KEVIN_FOLLOW_START)
		kevin_bot_globals.following = TRUE;
	else if (distance < KEVIN_FOLLOW_STOP)
		kevin_bot_globals.following = FALSE;

	if (kevin_bot_globals.following)
	{
		/* to you, his destination moved only once you have moved on from it */
		if (actor->control.path.destination_orders.destination_type == _destination_raw_location &&
			actor->control.path.destination_orders.raw.surface_index != NONE &&
			distance3d(&actor->control.path.destination_orders.raw.point, &your_point) < KEVIN_FOLLOW_RETARGET)
		{
			your_point = actor->control.path.destination_orders.raw.point;
			your_surface_index = actor->control.path.destination_orders.raw.surface_index;
		}
		*result = actor_move_to_point(actor_index, &your_point, your_surface_index, your_unit_index);
	}
	else
	{
		/* stopped where he is */
		actor_find_pathfinding_location(actor_index);
		if (actor->input.pathfinding_surface_index == NONE)
			return FALSE;
		if (actor->control.path.destination_orders.destination_type == _destination_raw_location &&
			actor->control.path.destination_orders.raw.surface_index != NONE &&
			distance3d(&actor->control.path.destination_orders.raw.point, &actor->input.pathfinding_point) < 1.f)
		{
			*result = actor_move_to_point(
				actor_index,
				&actor->control.path.destination_orders.raw.point,
				actor->control.path.destination_orders.raw.surface_index,
				NONE);
		}
		else
		{
			*result = actor_move_to_point(
				actor_index,
				&actor->input.pathfinding_point,
				actor->input.pathfinding_surface_index,
				NONE);
		}
	}

	return TRUE;
}

boolean kevin_bot_drives_unit(
	long unit_index)
{
	long kevin_unit_index;

	if (!kevin_bot_globals.active || unit_index == NONE)
		return FALSE;
	kevin_unit_index = kevin_bot_unit();

	return kevin_unit_index == unit_index && unit_get(unit_index)->unit.actor_index != NONE;
}
