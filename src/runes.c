/*
 *  $Id$
 */

#include "g_local.h"

// rune powerup timing (initial delay, powerup duration, respawn delay)
#define RUNE_DELAY_TIME			30
#define RUNE_DURATION			60
#define RUNE_RESPAWN_TIME		120

// map entity names for setting rune spawn locations
#define RUNE_SPAWN_RES			"item_rune_res"
#define RUNE_SPAWN_STR			"item_rune_str"
#define RUNE_SPAWN_HST			"item_rune_hst"
#define RUNE_SPAWN_RGN			"item_rune_rgn"

void SpawnRunesByType(int rune, char *spawnName, int delay);
void SpawnRune(vec3_t origin, int runeflags);
void ScheduleSpawnRune(vec3_t origin, int runeflags, int delay);
void RuneTouch(void);
void RespawnRune(void);

// clear previously existing rune entities
void ClearRunes(void)
{
	gedict_t *e;
	for (e = world; (e = ez_find(e, "rune"));)
	{
		ent_remove(e);
	}
}

// Spawns runes (delay for matchstart, no delay for prewar)
void SpawnRunes(qbool delay)
{
	int delayTime;
	delayTime = delay ? RUNE_DELAY_TIME : 0;

	ClearRunes();

	//spawn specific runes at matching spawn points (multiple instances allowed)
	SpawnRunesByType(CTF_RUNE_RES, RUNE_SPAWN_RES, delayTime);
	SpawnRunesByType(CTF_RUNE_STR, RUNE_SPAWN_STR, delayTime);
	SpawnRunesByType(CTF_RUNE_HST, RUNE_SPAWN_HST, delayTime);
	SpawnRunesByType(CTF_RUNE_RGN, RUNE_SPAWN_RGN, delayTime);
}

void SpawnRunesByType(int runeflags, char *className, int delay)
{
	gedict_t *runeSpawnPoint;
	if (delay > 0)
	{
		for (runeSpawnPoint = world; (runeSpawnPoint = ez_find(runeSpawnPoint, className));)
		{
			ScheduleSpawnRune(runeSpawnPoint->s.v.origin, runeflags, delay);
		}
	}
	else
	{
		for (runeSpawnPoint = world; (runeSpawnPoint = ez_find(runeSpawnPoint, className));)
		{
			SpawnRune(runeSpawnPoint->s.v.origin, runeflags);
		}
	}
}

void SpawnRune(vec3_t origin, int runeflags)
{
	// spawn new rune entity (at rune spawn location from world/bsp)
	gedict_t *rune = spawn();
	rune->classname = "rune";
	rune->ctf_flag = runeflags;
	rune->s.v.flags = FL_ITEM;
	rune->s.v.solid = SOLID_TRIGGER;
	rune->s.v.movetype = MOVETYPE_NONE;

	setorigin(rune, origin[0], origin[1], origin[2]);
	SetVector(rune->s.v.velocity, 0, 0, 0);

	// store origin to ensure proper respawn if multiple runes of same type
	VectorCopy(origin, rune->rune_spawn_origin);

	// set the model to be rendered for the entity
	if (runeflags & CTF_RUNE_RES)
	{
		setmodel(rune, "progs/end1.mdl");
	}
	else if (runeflags & CTF_RUNE_STR)
	{
		setmodel(rune, "progs/end2.mdl");
	}
	else if (runeflags & CTF_RUNE_HST)
	{
		setmodel(rune, "progs/end3.mdl");
	}
	else if (runeflags & CTF_RUNE_RGN)
	{
		setmodel(rune, "progs/end4.mdl");
	}

	setsize(rune, -16, -16, 0, 16, 16, 56);
	rune->touch = (func_t) RuneTouch;
	sound(rune, CHAN_VOICE, "items/itembk2.wav", 1, ATTN_NORM);	// play respawn sound
}

void ScheduleSpawnRune(vec3_t origin, int runeflags, int delay)
{
	gedict_t *temp = spawn();
	temp->s.v.nextthink = g_globalvars.time + delay;
	temp->think = (func_t) RespawnRune;
	temp->ctf_flag = runeflags;
	VectorCopy(origin, temp->rune_spawn_origin);
}

void RespawnRune(void)
{
	SpawnRune(self->rune_spawn_origin, self->ctf_flag);
	ent_remove(self);
}

void ClearRuneEffect(gedict_t *player)
{
	player->ctf_flag &= ~CTF_RUNE_MASK;
	cl_refresh_plus_scores(player);
	if (ISLIVE(player))
	{
		G_sprint(player, 1, "Your rune effect has worn off.\n");
	}
}

void RuneTouch(void)
{
	if (other->ct != ctPlayer || ISDEAD(other))
	{
		return;
	}

	if (!k_practice && match_in_progress != 2)
	{
		return;
	}

	if (other == PROG_TO_EDICT(self->s.v.owner))
	{
		return;
	}

	if (other->ctf_flag & CTF_RUNE_MASK)
	{
		if (g_globalvars.time > other->rune_notify_time)
		{
			other->rune_notify_time = g_globalvars.time + 10;
			G_sprint(other, 1, "You already have a rune.\n");
		}

		return;
	}

	other->ctf_flag |= self->ctf_flag;
	other->rune_pickup_time = g_globalvars.time;
	other->rune_effect_finished = g_globalvars.time + RUNE_DURATION;

	// force refresh player scores
	cl_refresh_plus_scores(other);

	if (self->ctf_flag & CTF_RUNE_RES)
	{
		G_sprint(other, 2, "You got the %s rune\n", redtext("resistance"));
	}

	if (self->ctf_flag & CTF_RUNE_STR)
	{
		G_sprint(other, 2, "You got the %s rune\n", redtext("strength"));
	}

	if (self->ctf_flag & CTF_RUNE_HST)
	{
		other->maxspeed *= (cvar("k_ctf_rune_power_hst") / 8) + 1;
		G_sprint(other, 2, "You got the %s rune\n", redtext("haste"));
	}

  if (self->ctf_flag & CTF_RUNE_RGN)
	{
		G_sprint(other, 2, "You got the %s rune\n", redtext("regeneration"));
	}

	sound(other, CHAN_ITEM, "weapons/lock4.wav", 1, ATTN_NORM);
	stuffcmd(other, "bf\n");

	ScheduleSpawnRune(self->rune_spawn_origin, self->ctf_flag, RUNE_RESPAWN_TIME);
	
	ent_remove(self);
}


void ResistanceSound(gedict_t *player)
{
	if (player->ctf_flag & CTF_RUNE_RES)
	{
		if (player->rune_sound_time < g_globalvars.time)
		{
			player->rune_sound_time = g_globalvars.time + 1;
			sound(player, CHAN_ITEM, "rune/rune1.wav", 1, ATTN_NORM);
		}
	}
}

void HasteSound(gedict_t *player)
{
	if (player->ctf_flag & CTF_RUNE_HST)
	{
		if (player->rune_sound_time < g_globalvars.time)
		{
			player->rune_sound_time = g_globalvars.time + 1;
			sound(player, CHAN_ITEM, "rune/rune3.wav", 1, ATTN_NORM);
		}
	}
}

void RegenerationSound(gedict_t *player)
{
	if (player->ctf_flag & CTF_RUNE_RGN)
	{
		if (player->rune_sound_time < g_globalvars.time)
		{
			player->rune_sound_time = g_globalvars.time + 1;
			sound(player, CHAN_ITEM, "rune/rune4.wav", 1, ATTN_NORM);
		}
	}
}

void CheckStuffRune(void)
{
	char *rune = "";

	if (cvar("k_instagib"))
	{
		if (self->i_agmr)
		{
			self->items2 = (int)self->items2 | (CTF_RUNE_RES << 5);

			return;
		}
	}

	if (!isCTF())
	{
		self->items2 = 0; // no runes/sigils in HUD

		if (self->last_rune && iKey(self, "runes"))
		{
			self->last_rune = NULL;
			stuffcmd_flags(self, STUFFCMD_IGNOREINDEMO, "set rune \"\"\n");
		}

		return;
	}

	self->items2 = (self->ctf_flag & CTF_RUNE_MASK) << 5;

	if (!iKey(self, "runes"))
	{
		return;
	}

	if (self->ctf_flag & CTF_RUNE_RES)
	{
		rune = "res";
	}
	else if (self->ctf_flag & CTF_RUNE_STR)
	{
		rune = "str";
	}
	else if (self->ctf_flag & CTF_RUNE_HST)
	{
		rune = "hst";
	}
	else if (self->ctf_flag & CTF_RUNE_RGN)
	{
		rune = "rgn";
	}
	else
	{
		rune = "";
	}

	if (!self->last_rune || strneq(rune, self->last_rune))
	{
		self->last_rune = rune;
		stuffcmd_flags(self, STUFFCMD_IGNOREINDEMO, "set rune \"%s\"\n", rune);
	}
}
