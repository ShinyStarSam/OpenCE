/*
KEVIN_TUNING.H

Kevin's combat tuning (kevin_tuning.c, kevin/BRIDGE_PLAN.md, "Combat
tuning"): his own actor variant and actor definition, the same on every map,
and the settings the sidecar changes them by.
*/

#ifndef __KEVIN_TUNING_H
#define __KEVIN_TUNING_H

struct unit_control_data;

/* ---------- prototypes/KEVIN_TUNING.C */

/* a level started: his actor variant and actor definition added to its tags,
and his settings applied to them */
void kevin_tuning_level_begin(void);
/* his actor variant, the one his actor is made from (NONE: there was no room
for it) */
long kevin_tuning_variant_index(void);

/* the settings: their count, names and values, a setting changed (FALSE:
no such setting, or out of its range), a preset, or all as his own */
long kevin_tuning_setting_count(void);
char const *kevin_tuning_setting_name(long index);
real kevin_tuning_setting_value(long index);
boolean kevin_tuning_set(char const *name, real value);
boolean kevin_tuning_preset(char const *name);
char const *kevin_tuning_preset_name(void);
void kevin_tuning_reset(void);

/* damage.c: the scale of a hit by or on Kevin (1: neither) */
real kevin_tuning_damage_scale(long owner_object_index, long victim_object_index);
/* weapons.c: whether a projectile fired by the owner goes exactly where it
is aimed (Kevin at accuracy 0 or 100); each is counted as a shot of his */
boolean kevin_tuning_exact_shot(long owner_object_index);
/* kevin_bot.c: where he aims at accuracy 0 (just past his target) or 100
(its head, led), from his eye; FALSE otherwise */
boolean kevin_tuning_aim(long actor_index, real_vector3d *aim);
/* weapons.c, as an actor's projectile is aimed: at accuracy 0 or 100,
Kevin's goes from the origin his tuning's way, in place of the AI's aim at
its target (TRUE) */
boolean kevin_tuning_aim_projectile(long actor_index, real_point3d const *origin, real_vector3d *forward);
/* his shots, and their hits on units, since the level began */
long kevin_tuning_shots(void);
long kevin_tuning_hits(void);

#endif
