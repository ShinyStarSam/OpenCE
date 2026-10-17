/*
KEVIN_BOT.H

Kevin, an AI companion (kevin.enabled): a Spartan at your side in a
campaign level, whom the game's AI drives (kevin_bot.c).
*/

#ifndef __KEVIN_BOT_H
#define __KEVIN_BOT_H

/* ---------- prototypes/KEVIN_BOT.C */

/* main.c's create_local_players, before it makes the players: whether
this game has Kevin (a campaign level played alone on this computer) */
void kevin_bot_game_begin(void);

/* players.c's players_update_before_game, each tick: makes Kevin behind you
(again, a while after he dies), gives his unit its actor, and keeps him
with you */
void kevin_bot_update(void);

/* his unit, or NONE (kevin_view.c) */
long kevin_bot_unit_index(void);

/* actor_moving.c's actor_move_to_firing_position: when the AI sends Kevin
to a firing position away from you, he keeps with you instead; TRUE when he
does, with the move's result */
boolean kevin_bot_move_to_you(long actor_index, short firing_position_index, boolean *result);

/* actors.c's actor_unit_control, before the AI's control is given to
Kevin's unit: his aim and look, turned smoothly toward where he means to
look (the way he goes, a glance at you or what the AI noticed, you while he
speaks, or in combat where the AI aims) */
struct unit_control_data;
void kevin_bot_adjust_control(long actor_index, struct unit_control_data *control_data);

/* he looks at you while he speaks, for this many ticks (the companion's
voice, kevin/BRIDGE_PLAN.md) */
void kevin_bot_set_talking(long ticks);

/* actor_perception.c's actor_compute_prop_target_weight: Kevin's target
weight for this unit, raised for the enemy you aim at and those near you */
real kevin_bot_target_weight(long actor_index, long unit_index, real weight);

/* actor_combat.c: Kevin's bursts against his actor variant's, as a scale */
#define KEVIN_FIRE_BURST_SEPARATION 0
#define KEVIN_FIRE_BURST_DURATION 1
#define KEVIN_FIRE_FIRST_BURST_DELAY 2
real kevin_bot_fire_scale(long actor_index, short what);

/* actor_combat.c: how far Kevin fires, his weapon's reach when it is
further than his actor variant's maximum firing range */
real kevin_bot_maximum_firing_range(long actor_index, real range);

/* actor_moving.c: Kevin moves any way from where his body faces, as a
player does; his body faces where he looks (kevin_bot_adjust_control), and
an actor otherwise turns to the way it goes before it sets off */
boolean kevin_bot_moves_freely(long actor_index);

/* whether this unit is Kevin's, with its actor (actor_moving.c) */
boolean kevin_bot_drives_unit(long unit_index);

#endif
