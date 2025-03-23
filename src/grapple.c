#include "g_local.h"

#define HOOK_FIRE_RATE  0.364
#define PULL_SPEED      684
#define INIT_PULL_SPEED 284
#define THROW_SPEED     884
#define ACCEL_TIME      0.598
#define EPSILON         1e-6F

#define SLACK_DELAY     0.325
#define SLACK_DURATION  1.105

#define MIN_GRAVITY     0.36
#define MAX_GRAVITY     0.78

#define MIN_INERTIA     0.078
#define MAX_INERTIA     0.478

void SpawnBlood(vec3_t dest, float damage);

float OscilationFactor(float length, float threshold, float vRad) {
	float x, k, b;
	x = threshold - length;
	x = x < 0 ? 0 : x;

	k = 0.186;
	b = 1.62 * sqrt(k);
	return k * x - b * vRad;
}

float VectorAlignment(vec3_t vector1, vec3_t vector2)
{
	vec3_t uv_1, uv_2;
	float ln1, ln2;

	VectorCopy(vector1, uv_1);
	ln1 = VectorNormalize(uv_1);

	VectorCopy(vector2, uv_2);
	ln2 = VectorNormalize(uv_2);

	// if either vector is too small to reliably normalize/compare
	return (ln1 < EPSILON || ln2 < EPSILON) ?
		0 : bound(-1.0, DotProduct(uv_1, uv_2), 1.0); // clamp output
}

void GrappleReset(gedict_t *rhook)
{
	gedict_t *owner = PROG_TO_EDICT(rhook->s.v.owner);
	if (owner == world) { return; }
	
	owner->on_hook = false;
	owner->hook_out = false;
	rhook->think = (func_t)GrappleRetract;
	rhook->s.v.nextthink = next_frame();

	if ((int)owner->s.v.items & IT_INVISIBILITY)
	{
		sound(rhook, CHAN_WEAPON, "weapons/ax1.wav", 0.85, ATTN_IDLE);
	}
	else
	{
		sound(rhook, CHAN_WEAPON, "weapons/ax1.wav", 1, ATTN_NORM);
	}

	// attack only finishes when hook is reset/released (continuous attack)
	owner->attack_finished = (self->ctf_flag & CTF_RUNE_HST) ? 
		g_globalvars.time + ((HOOK_FIRE_RATE) / cvar("k_ctf_rune_power_hst")) : g_globalvars.time + (HOOK_FIRE_RATE);

	owner->hook_reset_time = (self->ctf_flag & CTF_RUNE_HST) ? 
		g_globalvars.time + (HOOK_FIRE_RATE / cvar("k_ctf_rune_power_hst")) : g_globalvars.time + HOOK_FIRE_RATE;
}

void GrappleRetract(void)
{
	float hookDistance, returnSpeed;
	vec3_t hookVector, uv_hook, hookOrigin;
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);

	if (!owner || owner == world)
	{
		self->think = (func_t)SUB_Remove;
		self->s.v.nextthink = next_frame();
		return; 
	}

	VectorSubtract(owner->s.v.origin, self->s.v.origin, hookVector);
	VectorCopy(hookVector, uv_hook);
	hookDistance = VectorNormalize(uv_hook);

	if (g_globalvars.time >= owner->attack_finished || hookDistance <= 80)
	{
		self->think = (func_t)SUB_Remove;
		self->s.v.nextthink = next_frame();
		return;
	}

	returnSpeed = (hookDistance - 80) / g_globalvars.frametime * 0.234; // frametime * hook retract animation time
	VectorScale(hookVector, returnSpeed, self->s.v.velocity);

	self->touch = (func_t)SUB_Remove;
	self->think = (func_t)GrappleRetract;
	self->s.v.nextthink = next_frame();
}

void GrappleTrack(void)
{
	gedict_t *enemy = PROG_TO_EDICT(self->s.v.enemy);
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);

	if ((enemy->ct == ctPlayer) && ISDEAD(enemy)) { owner->on_hook = false; }

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

// spawns the chain link entities
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

// RemoveChain()
// Removes all chain link entities; this is a separate function because CLIENT also needs 
// to be able to remove the chain. Only one function required to remove all links.
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

// UpdateChain() 
// Repositions the chain links each frame. This single function maintains the positions 
// of all of the links. Only one link is thinking every frame. 
void UpdateChain(void)
{
	vec3_t t1, t2, t3, temp;
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner), *goal, *goal2;

	if (!owner->hook_out)
	{
		self->think = (func_t) RemoveChain;
		self->s.v.nextthink = next_frame();
		return;
	}

	owner->hook_cancel_time += 1;
	// delay cancelling the hook until ~250ms (13 * 19) if `smooth hook` is enabled (prevent spam attacks)
	if (owner->hook_cancel_time > 24) { CancelHook(owner); }
	
	VectorSubtract(owner->hook->s.v.origin, owner->s.v.origin, temp);

	goal = PROG_TO_EDICT(self->s.v.goalentity);
	goal2 = PROG_TO_EDICT(goal->s.v.goalentity);

	if (vlen(temp) <= 100 && owner->on_hook)
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

	if ((int)owner->s.v.items & IT_INVISIBILITY)
	{
		ExtFieldSetAlpha(self, 0.125);
		ExtFieldSetAlpha(goal, 0.125);
		ExtFieldSetAlpha(goal, 0.125);
	}
	else
	{
		ExtFieldSetAlpha(self, 1);
		ExtFieldSetAlpha(goal, 1);
		ExtFieldSetAlpha(goal, 1);
	}

	self->s.v.nextthink = next_frame();
}

// Allow the hook ro be reset mid-throw (smooth hook / hookstyle 1)
void CancelHook(gedict_t *owner)
{
	if (!owner->s.v.button0 && (owner->s.v.weapon == IT_HOOK))
	{
		GrappleReset(owner->hook);
	}
}

// Builds the hookchain (linked list)
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

	if (other == owner) { return; }

	// DO NOT allow the grapple to hook to any projectiles, no matter WHAT!
	// if you create new types of projectiles, make sure you use one of the
	// classnames below or write code to exclude your new classname so
	// grapples will not stick to them.
	if (streq(other->classname, "rocket") || streq(other->classname, "grenade")
		|| streq(other->classname, "spike") || streq(other->classname, "hook"))
			{ return; }

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
		if ((int)owner->s.v.items & IT_INVISIBILITY)
		{
			sound(self, CHAN_WEAPON, "weapons/tink1.wav", 1, ATTN_IDLE);
		}
		else
		{
			sound(self, CHAN_WEAPON, "player/axhit2.wav", 1, ATTN_NORM);
		}

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
	vec3_t hookVector, transVector, radialVel, tangentialVel, uv_self, uv_hook, uv_gravity;

	float distanceToHook, hasteMultiplier, playerSpeed, playerInfluence, minPull, maxPull, targetSpeed,
		speedDiff, radialSpeed, radialFactor, gravityInfluence, timeElapsed, timeFraction, magnitude,
		lerpFactor, overshootThreshold, /*radialDistance, radialZ, minAngle, maxAngle, minFactor, maxFactor, 
		forwardView, fadeFactor,*/ finalSpeed, finalSpeedCap;

	// Drop the hook if player lets go of fire
	if (!self->s.v.button0)
	{
		if (self->s.v.weapon == IT_HOOK)
		{
			GrappleReset(self->hook);
			return;
		}
	}

	// remove onground constantly
	// if ((int)self->s.v.flags & FL_ONGROUND)
	// {
	// 	self->s.v.flags -= FL_ONGROUND;
	// }

	// track anchor point, or player (if hooked to enemy)
	target = PROG_TO_EDICT(self->hook->s.v.enemy);
	if (target->ct == ctPlayer)
	{
		VectorSubtract(target->s.v.origin, self->s.v.origin, hookVector);
	}
	else
	{
		VectorSubtract(self->hook->s.v.origin, self->s.v.origin, hookVector);
	}

	if ((int)self->s.v.items & IT_INVISIBILITY)
	{
		ExtFieldSetAlpha(self->hook, 0.125);
	}
	else
	{
		ExtFieldSetAlpha(self->hook, 1);
	}

	// CORE VELOCITIES
	VectorCopy(hookVector, uv_hook);
	VectorNormalize(uv_hook);
	distanceToHook = VectorLength(hookVector);
	hasteMultiplier = (cvar("k_ctf_rune_power_hst") / 16) + 1;

	// HOOK ACCELLERATION
	minPull = (self->ctf_flag & CTF_RUNE_HST) ? INIT_PULL_SPEED * hasteMultiplier : INIT_PULL_SPEED;
	maxPull = (self->ctf_flag & CTF_RUNE_HST) ? PULL_SPEED * hasteMultiplier : PULL_SPEED;
	radialSpeed = DotProduct(self->s.v.velocity, uv_hook);
	lerpFactor = bound(0, (self->hook_time / ACCEL_TIME), 1);
	targetSpeed = minPull + lerpFactor * (maxPull - minPull); // lerp target speed

	speedDiff = targetSpeed - radialSpeed;
	if (speedDiff > 0)
	{
		magnitude = speedDiff / g_globalvars.frametime;
		VectorScale(uv_hook, magnitude, transVector);
		VectorMA(
			self->s.v.velocity,
			g_globalvars.frametime,
			transVector, // acceleration vector
			self->s.v.velocity);
	}

	else if (speedDiff < 0)
	{
		// exceeding target speed, soft damping on radial
		magnitude = radialSpeed - (abs(speedDiff) * 0.025);
		magnitude = magnitude < maxPull ? maxPull : magnitude;

		VectorScale(uv_hook, radialSpeed, radialVel);
		VectorSubtract(self->s.v.velocity, radialVel, tangentialVel);

		VectorScale(uv_hook, magnitude, radialVel); // new radial velocity
		VectorAdd(radialVel, tangentialVel, self->s.v.velocity); // new sum (player vel)
	}
	self->hook_time = min(self->hook_time + g_globalvars.frametime, ACCEL_TIME);

	// GRAVITY ADJUSTMENT
	VectorSet(uv_gravity, 0, 0, -1);
	gravityInfluence = VectorAlignment(uv_gravity, uv_hook);
	radialFactor = bound(0, (radialSpeed / maxPull), 1);
	lerpFactor = MIN_GRAVITY + radialFactor * (MAX_GRAVITY - MIN_GRAVITY); // gravity adjustment factor

	// if hook angle is upwards, and radialFactor is not insignificant
	if (gravityInfluence < 0 && radialFactor > 0.02)
	{
		VectorScale(uv_hook, gravityInfluence, transVector);

		// add back a fraction of gravity along the hook axis
		VectorMA(
			self->s.v.velocity, 
			lerpFactor * cvar("sv_gravity") * g_globalvars.frametime, 
			transVector, // gravity influence vector
			self->s.v.velocity
		);
	}

	// INERTIA / ANGLE OF INFLUENCE (ELASTICITY)
	VectorCopy(self->s.v.velocity, uv_self);
	playerSpeed = VectorNormalize(uv_self);
	playerInfluence = VectorAlignment(uv_self, uv_hook);

	// Apply elastic effect when moving opposite to hook direction
	if (playerInfluence < 0 && distanceToHook > (self->hook_initial_length / 2))
	{
		self->hook_awaytime += g_globalvars.frametime;
		timeElapsed = self->hook_awaytime - SLACK_DELAY;

		if (self->hook_awaytime > SLACK_DELAY) {
			timeFraction = bound(0, timeElapsed / SLACK_DURATION, 1);
			lerpFactor = MIN_INERTIA + (abs(playerInfluence) * timeFraction) * (MAX_INERTIA - MIN_INERTIA);

			VectorScale(uv_hook, (playerSpeed * (1.0 - lerpFactor)), transVector);
			VectorMA(
				self->s.v.velocity,
				lerpFactor,
				transVector, // inertia reduction vector
				self->s.v.velocity
			);
		}
	}

	else if (self->hook_awaytime > 0)
	{
		self->hook_awaytime = 0;
	}

	// OVERSHOOT & OSCILLATION
	overshootThreshold = 0.25 * self->hook_initial_length;
	if (distanceToHook < overshootThreshold)
	{
		magnitude = OscilationFactor(
			distanceToHook,
			overshootThreshold,
			DotProduct(self->s.v.velocity, uv_hook)
		);
		
		VectorScale(uv_hook, magnitude, transVector);
		VectorMA(
			self->s.v.velocity,
			g_globalvars.frametime,
			transVector, // oscilation magnitude vector
			self->s.v.velocity
		);

		magnitude = 0.9; // oscilation decay
		VectorScale(self->s.v.velocity, magnitude, self->s.v.velocity);
	}

	// // CLAMP BACKWARDS MOVEMENT (better air control physics with +moveback)
	// VectorCopy(self->s.v.velocity, transVector);

	// radialDistance = DotProduct(transVector, uv_hook);
	// radialZ = radialDistance * uv_hook[2];

	// VectorScale(uv_hook, radialDistance, radialVel);
	// VectorSubtract(transVector, radialVel, tangentialVel);

	// // min and max angles for clamping
	// minAngle = 10;
	// maxAngle = 70;

	// minFactor = sin(minAngle * (M_PI / 180));
	// maxFactor = sin(maxAngle * (M_PI / 180));

	// // compare viewangle with velocity to guesstimate if +moveback is pressed :/
	// trap_makevectors(self->s.v.v_angle);
	// forwardView = DotProduct(transVector, g_globalvars.v_forward);

	// fadeFactor = uv_hook[2] <= minFactor ?
	// 		0 : uv_hook[2] >= maxFactor ? 
	// 		1 : (uv_hook[2] - minFactor) / (maxFactor - minFactor);

	// if (fadeFactor > 0 && forwardView < 0)
	// {
	// 	if(radialDistance < 0 && distanceToHook > overshootThreshold)
	// 	{
	// 		radialDistance += (fabs(radialDistance) * fadeFactor * 0.75);
	// 		VectorScale(uv_hook, radialDistance, radialVel);
	// 		VectorAdd(radialVel, tangentialVel, transVector);
	// 	}

	// 	// recompute
	// 	radialDistance = DotProduct(transVector, uv_hook);
	// 	radialZ = radialDistance * uv_hook[2];
		
	// 	if(transVector[2] > radialZ)
	// 	{
	// 		transVector[2] = radialZ;
	// 	}
	// }
	// VectorCopy(transVector, self->s.v.velocity);


	// SPEED CAP
	finalSpeedCap = (self->ctf_flag & CTF_RUNE_HST) ? 
			PULL_SPEED * 1.125 * hasteMultiplier : PULL_SPEED * 1.125;
	
	finalSpeed = VectorLength(self->s.v.velocity);
	if (finalSpeed > finalSpeedCap)
	{
		magnitude = finalSpeedCap / finalSpeed;
		VectorScale(self->s.v.velocity, magnitude, self->s.v.velocity);
	}
}


// Called from weapons.c
void GrappleThrow(void)
{
	vec3_t initialVelocity, uv_throw;
	float hasteMultiplier, playerSpeed, playerInfluence, alignmentFactor;
	if (self->hook_out || self->hook_reset_time > g_globalvars.time) // only throw once & wait for cooldown time to complete
	{
		return;
	}

	hasteMultiplier =	(cvar("k_ctf_rune_power_hst") / 16) + 1;
	g_globalvars.msg_entity = EDICT_TO_PROG(self);
	WriteByte( MSG_ONE, SVC_SMALLKICK);
	
	if ((int)self->s.v.items & IT_INVISIBILITY)
	{
		sound(self, CHAN_WEAPON, "knight/sword2.wav", 0.7, ATTN_IDLE);
	}
	else
	{
		sound(self, CHAN_WEAPON, "knight/sword1.wav", 0.9, ATTN_NORM);
	}


	newmis = spawn();
	g_globalvars.newmis = EDICT_TO_PROG(newmis);
	newmis->s.v.movetype = MOVETYPE_FLYMISSILE;
	newmis->s.v.solid = SOLID_BBOX;
	newmis->s.v.owner = EDICT_TO_PROG(self);
	newmis->classname = "hook";
	if ((int)self->s.v.items & IT_INVISIBILITY)
	{
		ExtFieldSetAlpha(newmis, 0.125);
	}
	else
	{
		ExtFieldSetAlpha(newmis, 1);
	}

	self->hook = newmis;
	self->hook_cancel_time = 0;
	self->hook_awaytime = 0;

	trap_makevectors(self->s.v.v_angle);

	// Calculate throw direction (normalized)
	normalize(g_globalvars.v_forward, uv_throw);

	// Calculate alignment factor between player's current velocity and throw direction
	playerSpeed = vlen(self->s.v.velocity);
	playerInfluence = 0;
	if (playerSpeed > 0)  // Ensure velocity is non-zero to avoid division issues
	{
		playerInfluence = VectorAlignment(self->s.v.velocity, uv_throw);
	}

	// Use alignment factor to influence the throw speed
	// If aligned, increase speed slightly. If opposed, reduce speed slightly.
	alignmentFactor = 1 + (playerInfluence * 0.333);  // Scale between 0.66 (opposed) and 1.33 (aligned)

	// Adjust the throw speed based on alignment and haste multiplier
	if (self->ctf_flag & CTF_RUNE_HST)
	{
		if (!((int)self->s.v.items & IT_INVISIBILITY) || (self->s.v.weapon != IT_HOOK))
		{
			HasteSound(self);
		}
		VectorScale(uv_throw, THROW_SPEED * hasteMultiplier * alignmentFactor, initialVelocity);
		SetVector(newmis->s.v.avelocity, 300 * hasteMultiplier, 300 * hasteMultiplier, 300 * hasteMultiplier);
	}
	else
	{
		VectorScale(uv_throw, THROW_SPEED * alignmentFactor, initialVelocity);
		SetVector(newmis->s.v.avelocity, 300, 300, 300);
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
