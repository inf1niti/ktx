#include "g_local.h"

#define HOOK_FIRE_RATE  0.364
#define PULL_SPEED      684
#define INIT_PULL_SPEED 284
#define THROW_SPEED     884
#define ACCEL_TIME      0.598
#define EPSILON         1e-6F

#define SLACK_DELAY     0.325
#define SLACK_DURATION  1.105

#define PULL_ACCEL      5200
#define PULL_DECEL      2400
#define PULL_RECOVER    7200

#define MIN_GRAVITY     0.36
#define MAX_GRAVITY     0.78

#define MIN_INERTIA     0.078
#define MAX_INERTIA     0.478

#define INPUT_TANGENTIAL_ACCEL 360
#define INPUT_BACK_PULL_SCALE  0.55

#define RADIAL_SPEED_CAP       1.08
#define RADIAL_AWAY_CAP        0.85
#define TANGENTIAL_SPEED_CAP   1.15
#define TOTAL_SPEED_CAP        1.35
#define OSCILLATION_DAMPING    0.92

void SpawnBlood(vec3_t dest, float damage);
void RCTF_GrappleRetract(void);
void RCTF_CancelHook(gedict_t *owner);

float RCTF_OscillationFactor(float length, float threshold, float vRad)
{
	float x, k, b;

	x = threshold - length;
	x = x < 0 ? 0 : x;

	k = 0.186;
	b = 1.62 * sqrt(k);

	return k * x - b * vRad;
}

float RCTF_VectorAlignment(vec3_t vector1, vec3_t vector2)
{
	vec3_t uv_1, uv_2;
	float ln1, ln2;

	VectorCopy(vector1, uv_1);
	ln1 = VectorNormalize(uv_1);

	VectorCopy(vector2, uv_2);
	ln2 = VectorNormalize(uv_2);

	return (ln1 < EPSILON || ln2 < EPSILON) ? 0 : bound(-1.0, DotProduct(uv_1, uv_2), 1.0);
}

void RCTF_ClearGrounded(gedict_t *player)
{
	player->s.v.flags -= ((int)player->s.v.flags) & FL_ONGROUND;
}

float RCTF_Approach(float current, float target, float accel, float decel)
{
	float step, delta;

	delta = target - current;
	step = (delta > 0) ? accel : decel;

	if (fabs(delta) <= step)
	{
		return target;
	}

	return current + ((delta > 0) ? step : -step);
}

void RCTF_DecomposeVelocity(vec3_t velocity, vec3_t uv_hook, vec3_t radialVel, vec3_t tangentialVel,
		float *radialSpeed)
{
	*radialSpeed = DotProduct(velocity, uv_hook);
	VectorScale(uv_hook, *radialSpeed, radialVel);
	VectorSubtract(velocity, radialVel, tangentialVel);
}

void RCTF_GetHookVector(gedict_t *hook, gedict_t *target, vec3_t hookVector)
{
	if (target->ct == ctPlayer)
	{
		VectorSubtract(target->s.v.origin, self->s.v.origin, hookVector);
	}
	else
	{
		VectorSubtract(hook->s.v.origin, self->s.v.origin, hookVector);
	}
}

float RCTF_HasteMultiplier(void)
{
	return (cvar("k_ctf_rune_power_hst") / 16) + 1;
}

float RCTF_TargetPullSpeed(float minPull, float maxPull)
{
	float lerpFactor;

	lerpFactor = bound(0, self->hook_time / ACCEL_TIME, 1);
	return minPull + lerpFactor * (maxPull - minPull);
}

float RCTF_MovementInfluence(vec3_t uv_hook, vec3_t wishDir, vec3_t tangentDir)
{
	float wishAlign, tangentLen;

	VectorClear(wishDir);
	VectorClear(tangentDir);
	trap_makevectors(self->s.v.v_angle);

	VectorMA(wishDir, self->movement[0], g_globalvars.v_forward, wishDir);
	VectorMA(wishDir, self->movement[1], g_globalvars.v_right, wishDir);

	if (VectorNormalize(wishDir) < EPSILON)
	{
		return 0;
	}

	wishAlign = bound(-1.0, DotProduct(wishDir, uv_hook), 1.0);
	VectorMA(wishDir, -wishAlign, uv_hook, tangentDir);
	tangentLen = VectorNormalize(tangentDir);

	if (tangentLen < EPSILON)
	{
		VectorClear(tangentDir);
	}

	return wishAlign;
}

void RCTF_UpdateSlack(float wishAlign, float distanceToHook)
{
	if ((wishAlign < -0.25) && (distanceToHook > (self->hook_initial_length * 0.5)))
	{
		self->hook_awaytime += g_globalvars.frametime;
	}
	else
	{
		self->hook_awaytime = 0;
	}
}

void RCTF_ApplyRadialPull(vec3_t uv_hook, float distanceToHook, float minPull, float maxPull, float wishAlign)
{
	vec3_t radialVel, tangentialVel;
	float targetSpeed, radialSpeed, accel, slackFraction, slackScale;

	RCTF_DecomposeVelocity(self->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);

	targetSpeed = RCTF_TargetPullSpeed(minPull, maxPull);
	if (wishAlign < -0.15)
	{
		targetSpeed *= 1.0 + (wishAlign * INPUT_BACK_PULL_SCALE);
	}

	RCTF_UpdateSlack(wishAlign, distanceToHook);
	if (self->hook_awaytime > SLACK_DELAY)
	{
		slackFraction = bound(0, (self->hook_awaytime - SLACK_DELAY) / SLACK_DURATION, 1);
		slackScale = MIN_INERTIA + fabs(wishAlign) * (MAX_INERTIA - MIN_INERTIA);
		targetSpeed *= 1.0 - (slackFraction * slackScale);
	}

	targetSpeed = bound(minPull * 0.25, targetSpeed, maxPull);
	accel = ((radialSpeed < 0) && (targetSpeed > radialSpeed)) ? PULL_RECOVER : PULL_ACCEL;
	radialSpeed = RCTF_Approach(radialSpeed, targetSpeed, accel * g_globalvars.frametime,
			PULL_DECEL * g_globalvars.frametime);

	VectorScale(uv_hook, radialSpeed, radialVel);
	VectorAdd(radialVel, tangentialVel, self->s.v.velocity);
	self->hook_time = min(self->hook_time + g_globalvars.frametime, ACCEL_TIME);
}

void RCTF_ApplyInputControl(vec3_t tangentDir, float wishAlign)
{
	float accel;

	if (VectorLength(tangentDir) < EPSILON)
	{
		return;
	}

	accel = INPUT_TANGENTIAL_ACCEL;
	if (wishAlign < 0)
	{
		accel *= 1.0 + fabs(wishAlign) * 0.5;
	}

	VectorMA(self->s.v.velocity, accel * g_globalvars.frametime, tangentDir, self->s.v.velocity);
}

void RCTF_ApplyGravityInfluence(vec3_t uv_hook, float maxPull)
{
	vec3_t transVector, uv_gravity;
	float radialSpeed, radialFactor, gravityInfluence, gravityScale;

	radialSpeed = DotProduct(self->s.v.velocity, uv_hook);
	radialFactor = bound(0, radialSpeed / maxPull, 1);
	VectorSet(uv_gravity, 0, 0, -1);
	gravityInfluence = RCTF_VectorAlignment(uv_gravity, uv_hook);
	gravityScale = MIN_GRAVITY + radialFactor * (MAX_GRAVITY - MIN_GRAVITY);

	if (gravityInfluence < 0 && radialFactor > 0.02)
	{
		VectorScale(uv_hook, gravityInfluence, transVector);
		VectorMA(self->s.v.velocity, gravityScale * cvar("sv_gravity") * g_globalvars.frametime,
				transVector, self->s.v.velocity);
	}
}

void RCTF_ApplyOscillation(vec3_t uv_hook, float distanceToHook)
{
	vec3_t radialVel, tangentialVel, transVector;
	float radialSpeed, threshold, magnitude;

	threshold = 0.25 * self->hook_initial_length;
	if (distanceToHook >= threshold)
	{
		return;
	}

	magnitude = RCTF_OscillationFactor(distanceToHook, threshold, DotProduct(self->s.v.velocity, uv_hook));
	VectorScale(uv_hook, magnitude, transVector);
	VectorMA(self->s.v.velocity, g_globalvars.frametime, transVector, self->s.v.velocity);

	RCTF_DecomposeVelocity(self->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	if (radialSpeed > 0)
	{
		VectorScale(uv_hook, radialSpeed * OSCILLATION_DAMPING, radialVel);
		VectorAdd(radialVel, tangentialVel, self->s.v.velocity);
	}
}

void RCTF_CapVelocity(vec3_t uv_hook, float maxPull)
{
	vec3_t radialVel, tangentialVel;
	float radialSpeed, tangentialSpeed, totalSpeed, cap;

	RCTF_DecomposeVelocity(self->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);

	radialSpeed = bound(-(maxPull * RADIAL_AWAY_CAP), radialSpeed, maxPull * RADIAL_SPEED_CAP);
	VectorScale(uv_hook, radialSpeed, radialVel);

	tangentialSpeed = VectorNormalize(tangentialVel);
	cap = maxPull * TANGENTIAL_SPEED_CAP;
	if (tangentialSpeed > cap)
	{
		VectorScale(tangentialVel, cap, tangentialVel);
	}
	else
	{
		VectorScale(tangentialVel, tangentialSpeed, tangentialVel);
	}

	VectorAdd(radialVel, tangentialVel, self->s.v.velocity);

	totalSpeed = VectorLength(self->s.v.velocity);
	cap = maxPull * TOTAL_SPEED_CAP;
	if (totalSpeed > cap)
	{
		VectorScale(self->s.v.velocity, cap / totalSpeed, self->s.v.velocity);
	}
}

void RCTF_GrappleReset(gedict_t *rhook)
{
	gedict_t *owner = PROG_TO_EDICT(rhook->s.v.owner);

	if (owner == world)
	{
		return;
	}

	owner->on_hook = false;
	owner->hook_out = false;
	rhook->think = (func_t) RCTF_GrappleRetract;
	rhook->s.v.nextthink = next_frame();

	if (RingStealthActive(owner))
	{
		sound(rhook, CHAN_WEAPON, "weapons/ax1.wav", 0.85, ATTN_IDLE);
	}
	else
	{
		sound(rhook, CHAN_WEAPON, "weapons/ax1.wav", 1, ATTN_NORM);
	}

	owner->attack_finished = (owner->ctf_flag & CTF_RUNE_HST) ?
			g_globalvars.time + (HOOK_FIRE_RATE / cvar("k_ctf_rune_power_hst")) : g_globalvars.time + HOOK_FIRE_RATE;
	owner->hook_reset_time = (owner->ctf_flag & CTF_RUNE_HST) ?
			g_globalvars.time + (HOOK_FIRE_RATE / cvar("k_ctf_rune_power_hst")) : g_globalvars.time + HOOK_FIRE_RATE;
}

void RCTF_GrappleRetract(void)
{
	float hookDistance, returnSpeed;
	vec3_t hookVector, uv_hook;
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);

	if (!owner || owner == world)
	{
		self->think = (func_t) SUB_Remove;
		self->s.v.nextthink = next_frame();
		return;
	}

	VectorSubtract(owner->s.v.origin, self->s.v.origin, hookVector);
	VectorCopy(hookVector, uv_hook);
	hookDistance = VectorNormalize(uv_hook);

	if (g_globalvars.time >= owner->attack_finished || hookDistance <= 80)
	{
		self->think = (func_t) SUB_Remove;
		self->s.v.nextthink = next_frame();
		return;
	}

	returnSpeed = (hookDistance - 80) / g_globalvars.frametime * 0.234;
	VectorScale(uv_hook, returnSpeed, self->s.v.velocity);

	self->touch = (func_t) SUB_Remove;
	self->think = (func_t) RCTF_GrappleRetract;
	self->s.v.nextthink = next_frame();
}

void RCTF_GrappleTrack(void)
{
	gedict_t *enemy = PROG_TO_EDICT(self->s.v.enemy);
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);

	if ((enemy->ct == ctPlayer) && ISDEAD(enemy))
	{
		owner->on_hook = false;
	}

	if (!owner->on_hook || (owner->s.v.health <= 0))
	{
		RCTF_GrappleReset(self);
		return;
	}

	if (enemy->ct == ctPlayer)
	{
		if (!CanDamage(enemy, owner))
		{
			RCTF_GrappleReset(self);
			return;
		}

		setorigin(self, PASSVEC3(enemy->s.v.origin));

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

	if (enemy->ct != ctPlayer)
	{
		VectorCopy(enemy->s.v.velocity, self->s.v.velocity);
	}

	self->s.v.nextthink = next_frame();
}

gedict_t* RCTF_MakeLink(void)
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

	ExtFieldSetAlpha(newmis, RingStealthActive(PROG_TO_EDICT(self->s.v.owner)) ? RING_STEALTH_ALPHA : 1);

	setorigin(newmis, PASSVEC3(self->s.v.origin));
	setsize(newmis, 0, 0, 0, 0, 0, 0);

	return newmis;
}

void RCTF_RemoveChain(void)
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

void RCTF_UpdateChain(void)
{
	vec3_t t1, t2, t3, temp;
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner), *goal, *goal2;

	if (!owner->hook_out)
	{
		self->think = (func_t) RCTF_RemoveChain;
		self->s.v.nextthink = next_frame();
		return;
	}

	owner->hook_cancel_time += 1;
	if (owner->hook_cancel_time > 24)
	{
		RCTF_CancelHook(owner);
	}

	VectorSubtract(owner->hook->s.v.origin, owner->s.v.origin, temp);

	goal = PROG_TO_EDICT(self->s.v.goalentity);
	goal2 = PROG_TO_EDICT(goal->s.v.goalentity);

	if (vlen(temp) <= 100 && owner->on_hook)
	{
		self->think = (func_t) RCTF_RemoveChain;
		self->s.v.nextthink = next_frame();
		return;
	}

	VectorScale(temp, 0.25, t1);
	VectorAdd(t1, owner->s.v.origin, t1);

	VectorScale(temp, 0.50, t2);
	VectorAdd(t2, owner->s.v.origin, t2);

	VectorScale(temp, 0.75, t3);
	VectorAdd(t3, owner->s.v.origin, t3);

	setorigin(self, PASSVEC3(t1));
	setorigin(goal, PASSVEC3(t2));
	setorigin(goal2, PASSVEC3(t3));

	if (RingStealthActive(owner))
	{
		ExtFieldSetAlpha(self, RING_STEALTH_ALPHA);
		ExtFieldSetAlpha(goal, RING_STEALTH_ALPHA);
		ExtFieldSetAlpha(goal2, RING_STEALTH_ALPHA);
	}
	else
	{
		ExtFieldSetAlpha(self, 1);
		ExtFieldSetAlpha(goal, 1);
		ExtFieldSetAlpha(goal2, 1);
	}

	self->s.v.nextthink = next_frame();
}

void RCTF_CancelHook(gedict_t *owner)
{
	if (!owner->s.v.button0 && (owner->s.v.weapon == IT_HOOK))
	{
		RCTF_GrappleReset(owner->hook);
	}
}

void RCTF_BuildChain(void)
{
	self->s.v.goalentity = EDICT_TO_PROG(RCTF_MakeLink());
	PROG_TO_EDICT(self->s.v.goalentity)->think = (func_t) RCTF_UpdateChain;
	PROG_TO_EDICT(self->s.v.goalentity)->s.v.nextthink = next_frame();
	PROG_TO_EDICT(self->s.v.goalentity)->s.v.owner = self->s.v.owner;
	PROG_TO_EDICT(self->s.v.goalentity)->s.v.goalentity = EDICT_TO_PROG(RCTF_MakeLink());
	PROG_TO_EDICT(PROG_TO_EDICT(self->s.v.goalentity)->s.v.goalentity)->s.v.goalentity =
			EDICT_TO_PROG(RCTF_MakeLink());
}

void RCTF_GrappleAnchor(void)
{
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);
	vec3_t hookVector, uv_hook;

	if (other == owner)
	{
		return;
	}

	if (streq(other->classname, "rocket") || streq(other->classname, "grenade")
			|| streq(other->classname, "spike") || streq(other->classname, "hook"))
	{
		return;
	}

	if (other->ct == ctPlayer)
	{
		if ((match_in_progress != 2) || ((tp_num() == 4) && streq(getteam(other), getteam(owner))))
		{
			RCTF_GrappleReset(self);
			return;
		}

		owner->hook_damage_time = g_globalvars.time;
		sound(self, CHAN_WEAPON, "player/axhit1.wav", 1, ATTN_NORM);
		other->deathtype = dtHOOK;
		T_Damage(other, self, owner, 10);
		setmodel(self, "");
	}
	else
	{
		if (RingStealthActive(owner))
		{
			sound(self, CHAN_WEAPON, "weapons/tink1.wav", 1, ATTN_IDLE);
		}
		else
		{
			sound(self, CHAN_WEAPON, "player/axhit2.wav", 1, ATTN_NORM);
		}

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
		RCTF_GrappleReset(self);
		return;
	}

	VectorSubtract(self->s.v.origin, owner->s.v.origin, hookVector);
	VectorCopy(hookVector, uv_hook);
	VectorNormalize(uv_hook);

	RCTF_ClearGrounded(owner);
	owner->hook_initial_length = vlen(hookVector);
	owner->hook_time = 0;
	owner->on_hook = true;

	self->s.v.enemy = EDICT_TO_PROG(other);
	self->think = (func_t) RCTF_GrappleTrack;
	self->s.v.nextthink = next_frame();
	self->s.v.solid = SOLID_NOT;
	self->touch = (func_t) SUB_Null;
}

void RCTF_GrappleService(void)
{
	gedict_t *target;
	vec3_t hookVector, uv_hook, wishDir, tangentDir;
	float distanceToHook, hasteMultiplier, minPull, maxPull, wishAlign;

	if (!self->s.v.button0)
	{
		if (self->s.v.weapon == IT_HOOK)
		{
			RCTF_GrappleReset(self->hook);
			return;
		}
	}

	target = PROG_TO_EDICT(self->hook->s.v.enemy);
	RCTF_GetHookVector(self->hook, target, hookVector);

	ExtFieldSetAlpha(self->hook, RingStealthActive(self) ? RING_STEALTH_ALPHA : 1);
	RCTF_ClearGrounded(self);

	VectorCopy(hookVector, uv_hook);
	VectorNormalize(uv_hook);
	distanceToHook = VectorLength(hookVector);
	hasteMultiplier = RCTF_HasteMultiplier();

	minPull = (self->ctf_flag & CTF_RUNE_HST) ? INIT_PULL_SPEED * hasteMultiplier : INIT_PULL_SPEED;
	maxPull = (self->ctf_flag & CTF_RUNE_HST) ? PULL_SPEED * hasteMultiplier : PULL_SPEED;
	wishAlign = RCTF_MovementInfluence(uv_hook, wishDir, tangentDir);

	RCTF_ApplyRadialPull(uv_hook, distanceToHook, minPull, maxPull, wishAlign);
	RCTF_ApplyInputControl(tangentDir, wishAlign);
	RCTF_ApplyGravityInfluence(uv_hook, maxPull);
	RCTF_ApplyOscillation(uv_hook, distanceToHook);
	RCTF_CapVelocity(uv_hook, maxPull);
}

void RCTF_GrappleThrow(void)
{
	vec3_t initialVelocity, uv_throw;
	float hasteMultiplier, playerSpeed, playerInfluence, alignmentFactor;

	if (self->hook_out || self->hook_reset_time > g_globalvars.time)
	{
		return;
	}

	hasteMultiplier = (cvar("k_ctf_rune_power_hst") / 16) + 1;
	g_globalvars.msg_entity = EDICT_TO_PROG(self);
	WriteByte(MSG_ONE, SVC_SMALLKICK);

	if (RingStealthActive(self))
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

	self->hook = newmis;
	self->hook_cancel_time = 0;
	self->hook_awaytime = 0;

	trap_makevectors(self->s.v.v_angle);
	normalize(g_globalvars.v_forward, uv_throw);

	playerSpeed = vlen(self->s.v.velocity);
	playerInfluence = playerSpeed > 0 ? RCTF_VectorAlignment(self->s.v.velocity, uv_throw) : 0;
	alignmentFactor = 1 + (playerInfluence * 0.333);

	if (self->ctf_flag & CTF_RUNE_HST)
	{
		if (!RingStealthActive(self) || (self->s.v.weapon != IT_HOOK))
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

	VectorCopy(initialVelocity, newmis->s.v.velocity);

	newmis->touch = (func_t) RCTF_GrappleAnchor;
	newmis->think = (func_t) RCTF_BuildChain;
	newmis->s.v.nextthink = next_frame();

	if (k_ctf_custom_models)
	{
		setmodel(newmis, "progs/star.mdl");
	}
	else
	{
		setmodel(newmis, "progs/v_spike.mdl");
	}
	ExtFieldSetAlpha(newmis, RingStealthActive(self) ? RING_STEALTH_ALPHA : 1);

	setorigin(newmis, self->s.v.origin[0] + g_globalvars.v_forward[0] * 16,
			self->s.v.origin[1] + g_globalvars.v_forward[1] * 16,
			self->s.v.origin[2] + g_globalvars.v_forward[2] * 16 + 16);
	setsize(newmis, 0, 0, 0, 0, 0, 0);
	self->hook_out = true;
}
