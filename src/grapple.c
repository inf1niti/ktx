#include "g_local.h"

#define PULL_SPEED      700
#define INIT_PULL_SPEED	250
#define THROW_SPEED     800
#define NEW_THROW_SPEED 920
#define CR_THROW_SPEED  1200
#define HOOK_FIRE_RATE  0.212
#define HOOK_ACCEL_TIME 0.667
#define EPSILON 				1e-6F

void SpawnBlood(vec3_t dest, float damage);

float CalculateAcceleration(float currentSpeed, float targetSpeed)
{
	return (targetSpeed - currentSpeed) / g_globalvars.frametime; // units/sec^2
}


// calculates cosine/angle of influence between two vectors 
//  returns a float between -1.0 and 1.0
// (-1.0: opposite, 0.0: orthogonal, 1.0: aligned)
float VectorAlignment(vec3_t vector1, vec3_t vector2)
{
	vec3_t uv_1, uv_2;
	float ln1, ln2;
	VectorCopy(vector1, uv_1);
	VectorCopy(vector2, uv_2);
	ln1 = VectorNormalize(uv_1);
	ln2 = VectorNormalize(uv_2);

	// if either vector is too small to reliably normalize/compare
	if (ln1 < EPSILON || ln2 < EPSILON)
	{
		return 0;
	}

	return bound(-1.0, DotProduct(uv_1, uv_2), 1.0); // clamp output
}

// additive velocities with rudimentary physics can lead to quadratic speed growth
// appropriate damping can manage this more realistically than setting speed ceilings
void ApplyDampingForce(vec3_t velocity, float dampingCoefficient){
	vec3_t force;

	VectorScale(velocity, -dampingCoefficient, force);
	VectorAdd(velocity, force, velocity);
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
	vec3_t hookVector, uv_hook;

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

	// if ((int)owner->s.v.flags & FL_ONGROUND)
	// {
	// 	owner->s.v.flags -= FL_ONGROUND;
	// }

	VectorSubtract(self->s.v.origin, owner->s.v.origin, hookVector);
	VectorCopy(hookVector, uv_hook);
	VectorNormalize(uv_hook);

	// Store the initial hook length and direction for later calculations
	owner->hook_initial_length = vlen(hookVector);
	owner->hook_time = 0;
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
	gedict_t *target;

	vec3_t hookVector, uv_self, uv_hook, accelVec, radialVel, tangentialVel, newRadialVel,
		uv_gravity, hookGravity, springForce, osRadialVel, osTangentialVel, alignedVelocity, newVelocity;

	float distanceToHook, hasteMultiplier, maxPullSpeed, initPullSpeed, currentSpeedAlongHook, pullLerpFactor, 
		targetSpeed, speedDiff, accel, exceedAmount, dampFraction, radialSlowdown, newRadialSpeed,
		gravityInfluence, pullFraction, gravLerpFactor, minGravFactor, maxGravFactor, appliedFactor, playerSpeed,
		pullAlignment, slackTime, slackDuration, timeElapsed, timeFraction, minSpeedReduction, maxSpeedReduction, 
		reductionFactor, appliedReduction, magnitude, inertiaLerpFactor, maxInertiaFactor, overshootThreshold, ratio,
		springStrength, k, springMagnitude, radialSpeed, dampingFactor, finalSpeedCap, finalSpeed, scalingFactor;

	// Drop the hook if player lets go of fire
	if (!self->s.v.button0)
	{
		if (self->s.v.weapon == IT_HOOK)
		{
			GrappleReset(self->hook);
			return;
		}
	}

	// preform per-frame precalculations
	target = PROG_TO_EDICT(self->hook->s.v.enemy);

	if (target->ct == ctPlayer)
	{
		VectorSubtract(target->s.v.origin, self->s.v.origin, hookVector);
	}
	else
	{
		VectorSubtract(self->hook->s.v.origin, self->s.v.origin, hookVector);
	}

	// core velocities
	VectorCopy(hookVector, uv_hook);
	VectorNormalize(uv_hook);
	distanceToHook = VectorLength(hookVector);
	hasteMultiplier = (cvar("k_ctf_rune_power_hst") / 16) + 1;
	
	// pullspeed
	maxPullSpeed = (self->ctf_flag & CTF_RUNE_HST) ? PULL_SPEED * hasteMultiplier : PULL_SPEED;
	initPullSpeed = (self->ctf_flag & CTF_RUNE_HST) ? INIT_PULL_SPEED * hasteMultiplier : INIT_PULL_SPEED;
	currentSpeedAlongHook = DotProduct(self->s.v.velocity, uv_hook);
	pullLerpFactor = bound(0, (self->hook_time / HOOK_ACCEL_TIME), 1);
	targetSpeed = initPullSpeed + pullLerpFactor * (maxPullSpeed - initPullSpeed); // lerp target speed


	//  HOOK ACCELLERATION
	//  ==================

	speedDiff = targetSpeed - currentSpeedAlongHook;
	if (speedDiff > 0)
	{
		accel = speedDiff / g_globalvars.frametime;
		VectorScale(uv_hook, accel, accelVec);
		VectorMA(self->s.v.velocity, g_globalvars.frametime, accelVec, self->s.v.velocity);
	}

	else if (speedDiff < 0)
	{
		// exceeding target speed, soft damping on radial
		radialSpeed = currentSpeedAlongHook;
		VectorScale(uv_hook, radialSpeed, radialVel);
		VectorSubtract(self->s.v.velocity, radialVel, tangentialVel);

		exceedAmount = abs(speedDiff);
		dampFraction = 0.025; // remove 2.5% of excess speed per frame
		radialSlowdown = exceedAmount * dampFraction;
		newRadialSpeed = radialSpeed - radialSlowdown;
		newRadialSpeed = newRadialSpeed < maxPullSpeed ? maxPullSpeed : newRadialSpeed;

		VectorScale(uv_hook, newRadialSpeed, newRadialVel);
		VectorAdd(newRadialVel, tangentialVel, newVelocity);
		VectorCopy(newVelocity, self->s.v.velocity);
	}
	self->hook_time = min(self->hook_time + g_globalvars.frametime, HOOK_ACCEL_TIME);


	// GRAVITY ADJUSTMENT
	// ==================

	VectorSet(uv_gravity, 0, 0, -1);
	gravityInfluence = VectorAlignment(uv_gravity, uv_hook);
	pullFraction = bound(0, (currentSpeedAlongHook / maxPullSpeed), 1);
	gravLerpFactor = pullFraction;
	minGravFactor = 0.333;
	maxGravFactor = 0.9;
	appliedFactor = minGravFactor + gravLerpFactor * (maxGravFactor - minGravFactor); // lerp gravity adjustment factor (min/max adjustment 0.1/0.5)

	// if appliedFactor is not insignificantly small
	if (gravityInfluence < 0 && appliedFactor > 0.02)
	{
		VectorScale(uv_hook, gravityInfluence, hookGravity);

		// add back a fraction of gravity along the hook axis
		VectorMA(self->s.v.velocity, appliedFactor * cvar("sv_gravity") * g_globalvars.frametime, hookGravity, self->s.v.velocity);
	}


	// INERTIA / ANGLE OF INFLUENCE
	// ============================

	VectorCopy(self->s.v.velocity, uv_self);
	playerSpeed = VectorNormalize(uv_self); // Get speed and normalize velocity
	pullAlignment = bound(-1.0, DotProduct(uv_self, uv_hook), 1.0);
	slackTime = 0.6;
	slackDuration = 1.5;

	// Apply elastic effect when moving opposite to hook direction
	if (pullAlignment < 0 && distanceToHook > 0.5 * self->hook_initial_length)
	{
		self->hook_awaytime += g_globalvars.frametime;

		if(self->hook_awaytime > slackTime) {
			timeElapsed = self->hook_awaytime - slackTime;
			timeFraction = bound(0, (timeElapsed / slackDuration), 1);
			minSpeedReduction = 0.05;
			maxSpeedReduction = 0.5;
			reductionFactor = abs(pullAlignment) * timeFraction;
			appliedReduction = minSpeedReduction + reductionFactor * (maxSpeedReduction - minSpeedReduction);

			magnitude = playerSpeed * (1.0 - appliedReduction);
			maxInertiaFactor = 0.64;
			inertiaLerpFactor = maxInertiaFactor * abs(pullAlignment) * timeFraction;

			VectorScale(uv_hook, magnitude, alignedVelocity);
			VectorScale(self->s.v.velocity, (1.0 - inertiaLerpFactor), newVelocity);
			VectorMA(newVelocity, inertiaLerpFactor, alignedVelocity, newVelocity);

			VectorCopy(newVelocity, self->s.v.velocity);
		}
	}
	else if (self->hook_awaytime > 0)
	{
		self->hook_awaytime = 0;
	}


	// OVERSHOOT & OSCILLATION
	// =======================

	overshootThreshold = 0.18 * self->hook_initial_length;
	if (distanceToHook < overshootThreshold)
	{
    float x = (overshootThreshold - distanceToHook);
    if (x < 0) x = 0;
    
    float vRadial = DotProduct(self->s.v.velocity, uv_hook);

    float k = 0.125;
    float b = 2.0 * sqrt(k);
    float F = k * x - b * vRadial;
    VectorScale(uv_hook, F, accelVec);
    VectorMA(self->s.v.velocity, g_globalvars.frametime, accelVec, self->s.v.velocity);
	}


	// SPEED CAP
	// =========

	finalSpeedCap = (self->ctf_flag & CTF_RUNE_HST) ? 
			PULL_SPEED * 1.175 * hasteMultiplier : PULL_SPEED * 1.175;
	
	finalSpeed = VectorLength(self->s.v.velocity);
	if (finalSpeed > finalSpeedCap)
	{
		scalingFactor = finalSpeedCap / finalSpeed;
		VectorScale(self->s.v.velocity, scalingFactor, self->s.v.velocity);
	}

	// remove onground constantly?
	if ((int)self->s.v.flags & FL_ONGROUND)
	{
		self->s.v.flags -= FL_ONGROUND;
	}
}


// Called from weapons.c
void GrappleThrow(void)
{
	vec3_t initialVelocity, uv_throw;
	float hasteMultiplier, playerSpeed, influence, alignmentFactor;
	if (self->hook_out || self->hook_reset_time > g_globalvars.time) // only throw once & wait for cooldown time to complete
	{
		return;
	}

	hasteMultiplier =	(cvar("k_ctf_rune_power_hst") / 16) + 1;
	
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
  self->hook_awaytime = 0;

	trap_makevectors(self->s.v.v_angle);

	// Calculate throw direction (normalized)
	normalize(g_globalvars.v_forward, uv_throw);

	// Calculate alignment factor between player's current velocity and throw direction
	playerSpeed = vlen(self->s.v.velocity);
	influence = 0;
	if (playerSpeed > 0)  // Ensure velocity is non-zero to avoid division issues
	{
		influence = VectorAlignment(self->s.v.velocity, uv_throw);
	}

	// Use alignment factor to influence the throw speed
	// If aligned, increase speed slightly. If opposed, reduce speed slightly.
	alignmentFactor = 1 + (influence * 0.333);  // Scale between 0.66 (opposed) and 1.33 (aligned)

	// Adjust the throw speed based on alignment and haste multiplier
	if (self->ctf_flag & CTF_RUNE_HST)
	{
		HasteSound(self);
		VectorScale(uv_throw, NEW_THROW_SPEED * hasteMultiplier * alignmentFactor, initialVelocity);
		SetVector(newmis->s.v.avelocity, 300 * hasteMultiplier, 300 * hasteMultiplier, 300 * hasteMultiplier);
	}
	else
	{
		VectorScale(uv_throw, NEW_THROW_SPEED * alignmentFactor, initialVelocity);
		SetVector(newmis->s.v.avelocity, -300, -300, -300);
	}

	// Add the player's current velocity to the hook's initial velocity for inertia
	VectorCopy(initialVelocity, newmis->s.v.velocity);

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
