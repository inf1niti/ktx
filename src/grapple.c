/*
 * Original 'Morning Star' (Grapple Hook) by "Mike" <amichael@asu.alasu.edu> 
 * Quakeworld-friendly grapple by Wedge (Steve Bond)
 * PureCTF changes by Methabol
 *
 *
 *  $Id$
 */

#include "g_local.h"

#define PULL_SPEED      525
#define THROW_SPEED     800
#define NEW_THROW_SPEED 920
#define CR_THROW_SPEED  1200
#define HOOK_FIRE_RATE  0.212

void SpawnBlood(vec3_t dest, float damage);

float AdjustPullSpeed(float currentSpeed, float targetSpeed, float adjustmentRate, float easeFactor)
{
    // Ease velocity in when currentSpeed is low, ease out if close to targetSpeed
    float speedDiff = targetSpeed - currentSpeed;

    // If adjustment rate is greeater than the diff, snap to target
    if (fabs(speedDiff) < adjustmentRate)
    {
      return currentSpeed + (speedDiff * 0.1); //  gradually approach target
    }

    // Apply easing factor:  Increase speed initially, ease out as approaching target
    if (speedDiff > 0)
    {
      return currentSpeed + (adjustmentRate * easeFactor); // Increase speed
    }
    else
    {
      return currentSpeed - (adjustmentRate * easeFactor); // Decrease speed
    }
}

//
// GrappleReset - Removes the hook and resets its owner's state.
//                 expects a pointer to the hook
//
void GrappleReset(gedict_t *rhook)
{
	gedict_t *owner = PROG_TO_EDICT(rhook->s.v.owner);

	if (owner == world)
	{
		return;
	}

	sound(owner, CHAN_NO_PHS_ADD + CHAN_WEAPON, "weapons/bounce2.wav", 1, ATTN_NORM);
	owner->on_hook = false;
	owner->hook_out = false;
	owner->s.v.weaponframe = 0;

  owner->attack_finished = (self->ctf_flag & CTF_RUNE_HST) ? 
    g_globalvars.time + ((HOOK_FIRE_RATE / 2) / cvar("k_ctf_rune_power_hst")) : g_globalvars.time + (HOOK_FIRE_RATE / 2);
  owner->hook_reset_time = (self->ctf_flag & CTF_RUNE_HST) ? 
    g_globalvars.time + (HOOK_FIRE_RATE / cvar("k_ctf_rune_power_hst")) : g_globalvars.time + HOOK_FIRE_RATE;

	rhook->think = (func_t) SUB_Remove;
	rhook->s.v.nextthink = next_frame();
}

//
// GrappleTrack - Constantly updates the hook's position relative to
//                 what it's hooked to. Inflicts damage if attached to
//                 a player that is not on the same team as the hook's
//                 owner.
//
void GrappleTrack(void)
{
	gedict_t *enemy = PROG_TO_EDICT(self->s.v.enemy);
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);

	// Release dead targets
	if ((enemy->ct == ctPlayer) && ISDEAD(enemy))
	{
		owner->on_hook = false;
	}

	// drop the hook if owner is dead or has released the button
	if (!owner->on_hook || (owner->s.v.health <= 0))
	{
		GrappleReset(self);

		return;
	}

	if (enemy->ct == ctPlayer)
	{
		if (!CanDamage(enemy, owner))
		{
			GrappleReset(self);

			return;
		}

		// move the hook along with the player.  It's invisible, but
		// we need this to make the sound come from the right spot
		setorigin(self, PASSVEC3(enemy->s.v.origin));

		// only deal damage every 100ms 	
		if (g_globalvars.time >= (owner->hook_damage_time + 0.1))
		{
			owner->hook_damage_time = g_globalvars.time;
			sound(self, CHAN_WEAPON, "blob/land1.wav", 1, ATTN_NORM);
			enemy->deathtype = dtHOOK;
			T_Damage(enemy, self, owner, 1);
			trap_makevectors(self->s.v.v_angle);
			SpawnBlood(enemy->s.v.origin, 1);
		}
	}

	// If the hook is not attached to the player, constantly copy
	// the target's velocity. Velocity copying DOES NOT work properly
	// for a hooked client. 
	if (enemy->ct != ctPlayer)
	{
		VectorCopy(enemy->s.v.velocity, self->s.v.velocity);
	}

	self->s.v.nextthink = next_frame();
}

//
// MakeLink - spawns the chain link entities
//
gedict_t* MakeLink(void)
{
	newmis = spawn();
	g_globalvars.newmis = EDICT_TO_PROG(newmis);

	newmis->s.v.movetype = MOVETYPE_FLYMISSILE;
	newmis->s.v.solid = SOLID_NOT;
	newmis->s.v.owner = EDICT_TO_PROG(self);

	if (k_ctf_custom_models)
	{
		setmodel(newmis, "progs/bit.mdl");
	}
	else
	{
		setmodel(newmis, "progs/spike.mdl");
	}

	setorigin(newmis, PASSVEC3(self->s.v.origin));
	setsize(newmis, 0, 0, 0, 0, 0, 0);

	return newmis;
}

//
// RemoveChain - Removes all chain link entities; this is a separate
//                function because CLIENT also needs to be able
//                to remove the chain. Only one function required to
//                remove all links.
//
void RemoveChain(void)
{
	self->think = (func_t) SUB_Remove;
	self->s.v.nextthink = next_frame();

	if (self->s.v.goalentity)
	{
		gedict_t *goal = PROG_TO_EDICT(self->s.v.goalentity);
		goal->think = (func_t) SUB_Remove;
		goal->s.v.nextthink = next_frame();

		if (goal->s.v.goalentity)
		{
			gedict_t *goal2 = PROG_TO_EDICT(goal->s.v.goalentity);
			goal2->think = (func_t) SUB_Remove;
			goal2->s.v.nextthink = next_frame();
		}
	}
}

//
// Update_Chain - Repositions the chain links each frame. This single function
//                maintains the positions of all of the links. Only one link
//                is thinking every frame. 
//
void UpdateChain(void)
{
	vec3_t t1, t2, t3;
	vec3_t temp;
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner), *goal, *goal2;

	if (!owner->hook_out)
	{
		self->think = (func_t) RemoveChain;
		self->s.v.nextthink = next_frame();
		return;
	}

	owner->hook_cancel_time += 1;
	// delay cancelling the hook until ~250ms (13 * 19) if `smooth hook` is enabled (prevent spam attacks)
	if (owner->hook_cancel_time > 24)
	{
		CancelHook(owner);
  	}
	
	VectorSubtract(owner->hook->s.v.origin, owner->s.v.origin, temp);

	goal = PROG_TO_EDICT(self->s.v.goalentity);
	goal2 = PROG_TO_EDICT(goal->s.v.goalentity);

	if(vlen(temp) <= 100 && owner->on_hook)
	{
		// If there is a chain, ditch it now. We're close enough. 
		// Having extra entities lying around is never a good idea.
		self->think = (func_t) RemoveChain;
		self->s.v.nextthink = next_frame();
		return;
	}

	VectorScale(temp, 0.25, t1);
	VectorAdd(t1, owner->s.v.origin, t1);
	VectorScale(temp, 0.50, t2);
	VectorAdd(t2, owner->s.v.origin, t2);
	VectorScale(temp, 0.75, t3);
	VectorAdd(t3, owner->s.v.origin, t3);

	// These numbers are correct assuming 3 links.
	// 4 links would be *20 *40 *60 and *80
	setorigin(self, PASSVEC3(t1));
	setorigin(goal, PASSVEC3(t2));
	setorigin(goal2, PASSVEC3(t3));

	self->s.v.nextthink = next_frame();
}

//
// CancelHook - Allow the hook ro be reset mid-throw (smooth hook / hookstyle 1)
//
void CancelHook(gedict_t *owner)
{
	if (!owner->s.v.button0 && (owner->s.v.weapon == IT_HOOK))
	{
		GrappleReset(owner->hook);
	}
}

//
// BuildChain - Builds the chain (linked list)
//
void BuildChain(void)
{
	self->s.v.goalentity = EDICT_TO_PROG(MakeLink());
	PROG_TO_EDICT(self->s.v.goalentity)->think = (func_t)UpdateChain;
	PROG_TO_EDICT(self->s.v.goalentity)->s.v.nextthink = next_frame();
	PROG_TO_EDICT(self->s.v.goalentity)->s.v.owner = self->s.v.owner;
	PROG_TO_EDICT(self->s.v.goalentity)->s.v.goalentity = EDICT_TO_PROG(MakeLink());
	PROG_TO_EDICT(PROG_TO_EDICT(self->s.v.goalentity)->s.v.goalentity)->s.v.goalentity =
			EDICT_TO_PROG(MakeLink());
}

void GrappleAnchor(void)
{
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);
	vec3_t hookVector;

	if (other == owner)
	{
		return;
	}
	// DO NOT allow the grapple to hook to any projectiles, no matter WHAT!
	// if you create new types of projectiles, make sure you use one of the
	// classnames below or write code to exclude your new classname so
	// grapples will not stick to them.

	if (streq(other->classname, "rocket") || streq(other->classname, "grenade")
			|| streq(other->classname, "spike") || streq(other->classname, "hook"))
	{
		return;
	}

	if (other->ct == ctPlayer)
	{
		// grappling players in prewar is annoying
		if ((match_in_progress != 2) || ((tp_num() == 4) && streq(getteam(other), getteam(owner))))
		{
			GrappleReset(self);

			return;
		}

		owner->hook_damage_time = g_globalvars.time;
		sound(self, CHAN_WEAPON, "player/axhit1.wav", 1, ATTN_NORM);
		other->deathtype = dtHOOK;
		T_Damage(other, self, owner, 10);

		// make hook invisible since we will be pulling directly
		// towards the player the hook hit. Quakeworld makes it
		// too quirky to try to match hook's velocity with that of
		// the client that it hit. 
		setmodel(self, "");
	}
	else
	{
		sound(self, CHAN_WEAPON, "player/axhit2.wav", 1, ATTN_NORM);

		// One point of damage inflicted upon impact. Subsequent
		// damage will only be done to PLAYERS... this way secret
		// doors and triggers will only be damaged once.
		if (other->s.v.takedamage)
		{
			other->deathtype = dtHOOK;
			T_Damage(other, self, owner, 1);
		}

		SetVector(self->s.v.velocity, 0, 0, 0);
		SetVector(self->s.v.avelocity, 0, 0, 0);
	}

	if (!owner->s.v.button0)
	{
		GrappleReset(self);

		return;
	}

	if ((int)owner->s.v.flags & FL_ONGROUND)
	{
		owner->s.v.flags -= FL_ONGROUND;
	}

	VectorSubtract(self->s.v.origin, owner->s.v.origin, hookVector);

	// Set initial pull speed based on current movement velocity
	self->hook_pullspeed = fmaxf(vlen(self->s.v.velocity) * 1.1, 250);  // Dynamic initial speed, slightly higher than current velocity

	// Store the initial hook length and direction for later calculations
	owner->hook_initial_length = VectorLength(hookVector);
	VectorNormalize(hookVector);
	VectorCopy(hookVector, owner->hook_initial_direction);
	owner->on_hook = true;

	self->s.v.enemy = EDICT_TO_PROG(other);
	self->think = (func_t) GrappleTrack;
	self->s.v.nextthink = next_frame();
	self->s.v.solid = SOLID_NOT;
	self->touch = (func_t) SUB_Null;
}

// Called from client.c
void GrappleService(void)
{
	vec3_t hookVector, hookVelocity, springForce, gravityForce, airControlForce, gravityDirection;
	gedict_t *enemy;
	float hasteMultiplier = (cvar("k_ctf_rune_power_hst") / 16) + 1;
	float springConstantBase = 0.3;
	float dampingBase = 0.9;

	// Set gravity direction (downwards)
	VectorSet(gravityDirection, 0.0, 0.0, -1.0);

	// Drop the hook if player lets go of fire
	if (!self->s.v.button0)
	{
		if (self->s.v.weapon == IT_HOOK)
		{
			GrappleReset(self->hook);
			return;
		}
	}

	enemy = PROG_TO_EDICT(self->hook->s.v.enemy);

	// If hooked to a player, track them directly!
	if (enemy->ct == ctPlayer)
	{
		VectorSubtract(enemy->s.v.origin, self->s.v.origin, hookVector);
	}
	else
	{
		VectorSubtract(self->hook->s.v.origin, self->s.v.origin, hookVector);
	}
	VectorNormalize(hookVector);

	// Dot Product to determine the alignment between hook vector and gravity direction
	float gravityAlignment = DotProduct(hookVector, gravityDirection);
	gravityAlignment = fminf(fmaxf(gravityAlignment, -1.0), 1.0);  // Clamp between -1 and 1

	// Calculate gravity scaling factor
	float gravityScaleFactor = 0.9;  // Base gravity scale
	if (gravityAlignment > 0)  // Hook pulling downwards
	{
		gravityScaleFactor += (gravityAlignment * 0.6);  // Increase gravity effect up to 1.3x
	}
	else  // Hook pulling upwards
	{
		gravityScaleFactor += (gravityAlignment * 0.4);  // Decrease gravity effect (down to ~0.4x)
	}

	// // Air Control Adjustment - adjust hook pull direction by player input
	// float inputScale = 0.05;  // Scale of air control influence
	// airControlForce[0] = inputScale * self->s.v.v_angle[0];  // Player input influences
	// airControlForce[1] = inputScale * self->s.v.v_angle[1];
	// airControlForce[2] = 0;  // No vertical air control in this example

	// VectorAdd(hookVector, airControlForce, hookVector);
	// VectorNormalize(hookVector);

	// Dot Product to determine influence based on angle between velocity and hook direction
	float alignment = DotProduct(self->s.v.velocity, hookVector) / (VectorLength(self->s.v.velocity) * VectorLength(hookVector));
	alignment = fminf(fmaxf(alignment, -1.0), 1.0);
	float angleFactor = (1.0 - alignment) * 0.5;

	// Adjust pull speed using the easing factor
	float easeFactor = 1 - (self->hook_pullspeed / PULL_SPEED);
	easeFactor = fmaxf(0.3, easeFactor);  // Prevent speed from dropping too low, but allow a smoother ease-in
	self->hook_pullspeed = AdjustPullSpeed(self->hook_pullspeed, PULL_SPEED, 4, easeFactor);

	// Scale hook velocity by current pull speed
	if (self->ctf_flag & CTF_RUNE_HST)
	{
		VectorScale(hookVector, self->hook_pullspeed * hasteMultiplier, hookVelocity);
	}
	else
	{
		VectorScale(hookVector, self->hook_pullspeed, hookVelocity);
	}

	// Apply angle factor to reduce runaway acceleration at specific alignments
	VectorScale(hookVelocity, 1.0 - angleFactor, hookVelocity);

	// Add hook velocity to player's current velocity
	VectorAdd(self->s.v.velocity, hookVelocity, self->s.v.velocity);

	// Cap velocity to prevent runaway speed
	float maxSpeed = 750;
	float currentSpeed = VectorLength(self->s.v.velocity);
	if (currentSpeed > maxSpeed)
	{
		VectorScale(self->s.v.velocity, maxSpeed / currentSpeed, self->s.v.velocity);
	}

  // apply scaled gravity effect based on speed
	if (!((int)self->s.v.flags & FL_ONGROUND))  // Only apply gravity if not on ground
	{
    float speedFactor = fminf(currentSpeed / maxSpeed, 1);
    float pullSpeedGravityFactor = (1 - speedFactor) * 0.5;
    gravityScaleFactor *= 1 - pullSpeedGravityFactor;
		float gravityStrength = -gravityScaleFactor * cvar("sv_gravity") * g_globalvars.frametime;
		self->s.v.velocity[2] += gravityStrength;
	}

	// Overshoot and Oscillation Logic
	float distanceToHook = VectorLength(hookVector);
	float overshootThreshold = 0.2 * self->hook_initial_length;

	if (distanceToHook < overshootThreshold)
	{
		// Adjust spring constant dynamically based on current pull speed
		float speedFactor = self->hook_pullspeed / PULL_SPEED;
		float dynamicSpringConstant = springConstantBase + (speedFactor * 0.5);
		VectorScale(hookVector, -dynamicSpringConstant, springForce);
		VectorAdd(self->s.v.velocity, springForce, self->s.v.velocity);

		// Adjust damping factor dynamically based on current pull speed
		float dynamicDampingFactor = fminf(0.95, dampingBase + (speedFactor * 0.05));
		VectorScale(self->s.v.velocity, dynamicDampingFactor, self->s.v.velocity);
	}
}


// Called from weapons.c
void GrappleThrow(void)
{
	float hasteMultiplier, throwSpeed;

	if (self->hook_out || self->hook_reset_time > g_globalvars.time) // only throw once & wait for cooldown time to complete
	{
		return;
	}

	hasteMultiplier =	(cvar("k_ctf_rune_power_hst") / 16) + 1;
	throwSpeed = NEW_THROW_SPEED;
	
	g_globalvars.msg_entity = EDICT_TO_PROG(self);
	WriteByte( MSG_ONE, SVC_SMALLKICK);

	sound(self, CHAN_WEAPON, "weapons/ax1.wav", 1, ATTN_NORM);

	newmis = spawn();
	g_globalvars.newmis = EDICT_TO_PROG(newmis);
	newmis->s.v.movetype = MOVETYPE_FLYMISSILE;
	newmis->s.v.solid = SOLID_BBOX;
	newmis->s.v.owner = EDICT_TO_PROG(self);
	self->hook = newmis;
	newmis->classname = "hook";
	self->hook_cancel_time = 0;

	trap_makevectors(self->s.v.v_angle);

	// Weapon velocitys should not be based on server maxspeed imo
	// Removing purectf velocity changes ( 2.5 * self->maxspeed )

	if (self->ctf_flag & CTF_RUNE_HST)
	{
		HasteSound(self);
		VectorScale(g_globalvars.v_forward, throwSpeed * hasteMultiplier, newmis->s.v.velocity);
		// rotate/spin star model as hook flies
		SetVector(newmis->s.v.avelocity, 300 * hasteMultiplier, 300 * hasteMultiplier, 300 * hasteMultiplier);
	}
	else
	{
		VectorScale(g_globalvars.v_forward, throwSpeed, newmis->s.v.velocity);
		// rotate/spin star model as hook flies
		SetVector(newmis->s.v.avelocity, -250, -250, -250);
	}


	newmis->touch = (func_t) GrappleAnchor;
	newmis->think = (func_t) BuildChain;
	newmis->s.v.nextthink = next_frame();

	if (k_ctf_custom_models)
	{
		setmodel(newmis, "progs/star.mdl");
	}
	else
	{
		setmodel(newmis, "progs/v_spike.mdl");
	}

	setorigin(newmis, self->s.v.origin[0] + g_globalvars.v_forward[0] * 16,
				self->s.v.origin[1] + g_globalvars.v_forward[1] * 16,
				self->s.v.origin[2] + g_globalvars.v_forward[2] * 16 + 16);
	setsize(newmis, 0, 0, 0, 0, 0, 0);
	self->hook_out = true;
}
