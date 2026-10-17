/*
KEVIN_BRIDGE.H

The bridge between the game and Kevin's sidecar (kevin.enabled, kevin.port;
kevin_bridge.c, kevin/BRIDGE_PLAN.md).
*/

#ifndef __KEVIN_BRIDGE_H
#define __KEVIN_BRIDGE_H

struct unit_control_data;

/* ---------- prototypes/KEVIN_BRIDGE.C */

/* game.c's game_tick, before the AI's update: reads what the sidecar sent
(so a command is applied this tick) and sends it the state and events */
void kevin_bridge_update(void);

/* kevin_bot.c's look: where the sidecar has him aim (aim_at), TRUE while
it does */
boolean kevin_bridge_look_target(long unit_index, real_vector3d *target);

/* kevin_bot.c, after the AI's control and his look: the sidecar's
overlay (presses, holds, hold fire) on his unit's control */
void kevin_bridge_adjust_control(long unit_index, struct unit_control_data *control_data);

/* an event of kevin_bot.c's: its type, a one-line summary, the object it
is about (its position is the event's; NONE for none), whether Kevin did
it, and further fields as JSON members ("\"why\":\"stuck\"", or NULL) */
void kevin_bridge_event(char const *type, char const *summary, long object_index,
	boolean kevin_did, char const *fields);

/* the game's commands to him no longer hold (a new level, his death, his
being made again): commands of the generation before are refused */
void kevin_bridge_new_generation(char const *why);

/* game_state.c: a checkpoint saved, or reverted to */
void kevin_bridge_checkpoint(boolean reverted);

#endif
