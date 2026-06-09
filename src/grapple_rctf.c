#include "g_local.h"

#define PULL_SPEED      684
#define INIT_PULL_SPEED 360
#define THROW_SPEED     884
#define ACCEL_TIME      0.598
#define EPSILON         1e-6F

#define HOOK_MIN_REFIRE_TIME       0.28
#define HOOK_MAX_REFIRE_TIME       0.96
#define HOOK_RETRACT_SPEED         1400
#define HOOK_RETRACT_END_DISTANCE  80

#define GROUND_DETACH_SPEED        360
#define GROUND_DETACH_MIN_UP       0.02
#define GROUND_MIN_LIFT_SPEED      120
#define GROUND_MAX_LIFT_SPEED      260
#define GROUND_FULL_LIFT_UP        0.25
#define GROUND_TANGENTIAL_SCALE    0.35
#define GROUND_FAST_TANGENTIAL_SCALE 0.95

#define SLACK_DELAY     0.325
#define SLACK_DURATION  1.105

#define PULL_ACCEL      4800
#define PULL_DECEL      2400
#define PULL_RECOVER    7200
#define VERTICAL_PULL_BOOST 0.35

#define MIN_GRAVITY     0.36
#define MAX_GRAVITY     0.78

#define MIN_INERTIA     0.078
#define MAX_INERTIA     0.478

#define INPUT_TANGENTIAL_ACCEL 290
#define INPUT_TANGENTIAL_BACK_BIAS 0.25
#define INPUT_BACK_PULL_SCALE  0.55
#define INPUT_BACK_RESIST_SCALE 0.85
#define INPUT_BACK_GRAVITY_FACTOR 0.65
#define INPUT_FORWARD_RADIAL_BOOST 0.12
#define INPUT_FORWARD_TANGENTIAL_SCALE 0.65
#define INPUT_FORWARD_GRAVITY_SCALE 0.65

#define TENSION_INPUT_GAIN     320
#define TENSION_AWAY_GAIN      0.65
#define TENSION_DECAY_RATE     180
#define TENSION_RELEASE_RATE   720
#define TENSION_MAX            0.42

#define RADIAL_SPEED_CAP       1.14
#define RADIAL_AWAY_CAP        0.85
#define TANGENTIAL_SPEED_CAP   1.035
#define TOTAL_SPEED_CAP        1.26
#define SPEED_PRESERVE_TIME    0.22
#define SPEED_PRESERVE_BUFFER  0.99
#define OSCILLATION_DAMPING    0.92
#define OSCILLATION_TANGENTIAL_DAMPING 0.985
#define OSCILLATION_THRESHOLD_SCALE     0.33
#define OSCILLATION_DAMPING_DELAY       0.18

void SpawnBlood(vec3_t dest, float damage);
void RCTF_GrappleRetract(void);
void RCTF_CancelHook(gedict_t *owner);
float RCTF_HasteMultiplier(void);

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

float RCTF_MinPullSpeed(gedict_t *player)
{
	return (player->ctf_flag & CTF_RUNE_HST) ? INIT_PULL_SPEED * RCTF_HasteMultiplier() : INIT_PULL_SPEED;
}

float RCTF_MaxPullSpeed(gedict_t *player)
{
	return (player->ctf_flag & CTF_RUNE_HST) ? PULL_SPEED * RCTF_HasteMultiplier() : PULL_SPEED;
}

void RCTF_ClearGrounded(gedict_t *player)
{
	player->s.v.flags -= ((int)player->s.v.flags) & FL_ONGROUND;
	player->s.v.groundentity = EDICT_TO_PROG(world);
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

float RCTF_SpeedPreserveFactor(void)
{
	return bound(0, 1.0 - (self->hook_time / SPEED_PRESERVE_TIME), 1.0);
}

float RCTF_PreservedCap(float normalCap, float initialSpeed, float preserveFactor)
{
	float preservedCap;

	preservedCap = initialSpeed * SPEED_PRESERVE_BUFFER;
	if (preservedCap <= normalCap)
	{
		return normalCap;
	}

	return normalCap + (preservedCap - normalCap) * preserveFactor;
}

void RCTF_DecomposeVelocity(vec3_t velocity, vec3_t uv_hook, vec3_t radialVel, vec3_t tangentialVel,
		float *radialSpeed)
{
	*radialSpeed = DotProduct(velocity, uv_hook);
	VectorScale(uv_hook, *radialSpeed, radialVel);
	VectorSubtract(velocity, radialVel, tangentialVel);
}

void RCTF_SetMinimumRadialSpeed(gedict_t *player, vec3_t uv_hook, float minSpeed)
{
	vec3_t radialVel, tangentialVel;
	float radialSpeed;

	RCTF_DecomposeVelocity(player->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	if (radialSpeed >= minSpeed)
	{
		return;
	}

	VectorScale(uv_hook, minSpeed, radialVel);
	VectorAdd(radialVel, tangentialVel, player->s.v.velocity);
}

void RCTF_DampenTangentialVelocity(gedict_t *player, vec3_t uv_hook, float scale)
{
	vec3_t radialVel, tangentialVel;
	float radialSpeed;

	RCTF_DecomposeVelocity(player->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	VectorScale(tangentialVel, scale, tangentialVel);
	VectorAdd(radialVel, tangentialVel, player->s.v.velocity);
}

void RCTF_SetMinimumGroundLift(gedict_t *player, vec3_t uv_hook)
{
	float liftSpeed;

	if (uv_hook[2] <= GROUND_DETACH_MIN_UP)
	{
		return;
	}

	liftSpeed = GROUND_MAX_LIFT_SPEED * bound(0, uv_hook[2] / GROUND_FULL_LIFT_UP, 1);
	liftSpeed = max(GROUND_MIN_LIFT_SPEED, liftSpeed);

	if (player->s.v.velocity[2] < liftSpeed)
	{
		player->s.v.velocity[2] = liftSpeed;
	}
}

void RCTF_DetachFromGround(gedict_t *player, vec3_t uv_hook)
{
	qbool wasGrounded = (int)player->s.v.flags & FL_ONGROUND;

	RCTF_ClearGrounded(player);
	if (wasGrounded && uv_hook[2] > GROUND_DETACH_MIN_UP)
	{
		RCTF_SetMinimumRadialSpeed(player, uv_hook, GROUND_DETACH_SPEED);
		RCTF_SetMinimumGroundLift(player, uv_hook);
	}
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

void RCTF_GetPullVector(vec3_t uv_hook, qbool wasOnGround, vec3_t uv_pull)
{
	VectorCopy(uv_hook, uv_pull);

	if (wasOnGround && (uv_pull[2] <= GROUND_DETACH_MIN_UP))
	{
		uv_pull[2] = 0;
		if (VectorNormalize(uv_pull) < EPSILON)
		{
			VectorCopy(uv_hook, uv_pull);
		}
	}
}

float RCTF_HasteMultiplier(void)
{
	return (cvar("k_ctf_rune_power_hst") / 16) + 1;
}

float RCTF_GrappleRefireDelay(gedict_t *owner, gedict_t *rhook)
{
	vec3_t hookVector, uv_hook;
	float hookDistance, delay;

	VectorSubtract(owner->s.v.origin, rhook->s.v.origin, hookVector);
	VectorCopy(hookVector, uv_hook);
	hookDistance = VectorNormalize(uv_hook);

	delay = (hookDistance - HOOK_RETRACT_END_DISTANCE) / HOOK_RETRACT_SPEED;
	if (owner->ctf_flag & CTF_RUNE_HST)
	{
		delay /= cvar("k_ctf_rune_power_hst");
	}
	delay = bound(HOOK_MIN_REFIRE_TIME, delay, HOOK_MAX_REFIRE_TIME);

	return delay;
}

float RCTF_GrapplePostRetractDelay(gedict_t *owner)
{
	if (owner->ctf_flag & CTF_RUNE_HST)
	{
		return 0.143 / cvar("k_ctf_rune_power_hst");
	}

	return 0.130;
}

float RCTF_TargetPullSpeed(float minPull, float maxPull)
{
	float lerpFactor;

	lerpFactor = bound(0, self->hook_time / ACCEL_TIME, 1);
	return minPull + lerpFactor * (maxPull - minPull);
}

float RCTF_MovementInfluence(vec3_t uv_hook, vec3_t wishDir, vec3_t tangentDir)
{
	vec3_t controlDir;
	float wishAlign, tangentLen, forwardMove;

	VectorClear(wishDir);
	VectorClear(tangentDir);
	VectorClear(controlDir);
	trap_makevectors(self->s.v.v_angle);

	VectorMA(wishDir, self->movement[0], g_globalvars.v_forward, wishDir);
	VectorMA(wishDir, self->movement[1], g_globalvars.v_right, wishDir);

	if (VectorNormalize(wishDir) < EPSILON)
	{
		return 0;
	}

	forwardMove = max(self->movement[0], 0);
	VectorMA(controlDir, forwardMove, g_globalvars.v_forward, controlDir);
	VectorMA(controlDir, self->movement[1], g_globalvars.v_right, controlDir);

	wishAlign = bound(-1.0, DotProduct(wishDir, uv_hook), 1.0);
	if (VectorNormalize(controlDir) < EPSILON)
	{
		return wishAlign;
	}

	VectorMA(controlDir, -DotProduct(controlDir, uv_hook), uv_hook, tangentDir);
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

float RCTF_UpdateTension(float wishAlign, float radialSpeed, float maxPull)
{
	float tensionCap, inputTension, velocityTension, tensionBoost;

	tensionCap = maxPull * TENSION_MAX;
	inputTension = 0;
	velocityTension = 0;
	tensionBoost = 0;

	if (wishAlign < -0.25)
	{
		inputTension = fabs(wishAlign) * TENSION_INPUT_GAIN * g_globalvars.frametime;
	}

	if (radialSpeed < 0)
	{
		velocityTension = min(-radialSpeed, maxPull) * TENSION_AWAY_GAIN * g_globalvars.frametime;
	}

	self->hook_tension = min(self->hook_tension + inputTension + velocityTension, tensionCap);

	if ((wishAlign >= -0.1) && (radialSpeed >= -25) && (self->hook_tension > 0))
	{
		tensionBoost = self->hook_tension;
		self->hook_tension = max(0, self->hook_tension - TENSION_RELEASE_RATE * g_globalvars.frametime);
	}
	else if (!inputTension && !velocityTension && (self->hook_tension > 0))
	{
		self->hook_tension = max(0, self->hook_tension - TENSION_DECAY_RATE * g_globalvars.frametime);
	}

	return tensionBoost;
}

float RCTF_DownwardPullTarget(vec3_t uv_hook, vec3_t velocity, float targetSpeed)
{
	float fallingPull;

	if (uv_hook[2] >= -GROUND_DETACH_MIN_UP)
	{
		return targetSpeed;
	}

	fallingPull = max(0, -velocity[2]) * -uv_hook[2];
	if (fallingPull > targetSpeed)
	{
		return fallingPull;
	}

	return targetSpeed;
}

float RCTF_PreservedRadialPullTarget(float radialSpeed, float targetSpeed, float maxPull)
{
	float preserveFactor, preservedCap;

	if (radialSpeed <= targetSpeed)
	{
		return targetSpeed;
	}

	preserveFactor = RCTF_SpeedPreserveFactor();
	if (preserveFactor <= 0)
	{
		return targetSpeed;
	}

	preservedCap = RCTF_PreservedCap(maxPull * RADIAL_SPEED_CAP, self->hook_initial_radial_speed, preserveFactor);
	if (radialSpeed <= preservedCap)
	{
		return radialSpeed;
	}

	return max(targetSpeed, preservedCap);
}

void RCTF_ApplyRadialPull(vec3_t uv_hook, float distanceToHook, float minPull, float maxPull, float wishAlign,
		qbool forwardHeld)
{
	vec3_t radialVel, tangentialVel;
	float targetSpeed, radialSpeed, accel, slackFraction, slackScale, tensionBoost, pullWishAlign;

	RCTF_DecomposeVelocity(self->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	pullWishAlign = (wishAlign < -0.15) ? wishAlign * INPUT_BACK_RESIST_SCALE : wishAlign;

	targetSpeed = RCTF_TargetPullSpeed(minPull, maxPull);
	targetSpeed += bound(0, uv_hook[2], 1) * VERTICAL_PULL_BOOST * (maxPull - targetSpeed);
	if (forwardHeld)
	{
		targetSpeed += INPUT_FORWARD_RADIAL_BOOST * (maxPull - targetSpeed);
	}

	if (pullWishAlign < -0.15)
	{
		targetSpeed *= 1.0 + (pullWishAlign * INPUT_BACK_PULL_SCALE);
	}

	RCTF_UpdateSlack(pullWishAlign, distanceToHook);
	tensionBoost = RCTF_UpdateTension(pullWishAlign, radialSpeed, maxPull);
	if (self->hook_awaytime > SLACK_DELAY)
	{
		slackFraction = bound(0, (self->hook_awaytime - SLACK_DELAY) / SLACK_DURATION, 1);
		slackScale = MIN_INERTIA + fabs(pullWishAlign) * (MAX_INERTIA - MIN_INERTIA);
		targetSpeed *= 1.0 - (slackFraction * slackScale);
	}

	targetSpeed += tensionBoost;
	targetSpeed = RCTF_DownwardPullTarget(uv_hook, self->s.v.velocity, targetSpeed);
	targetSpeed = bound(minPull * 0.25, targetSpeed, maxPull * RADIAL_SPEED_CAP);
	targetSpeed = RCTF_PreservedRadialPullTarget(radialSpeed, targetSpeed, maxPull);
	accel = ((radialSpeed < 0) && (targetSpeed > radialSpeed)) ? PULL_RECOVER : PULL_ACCEL;
	radialSpeed = RCTF_Approach(radialSpeed, targetSpeed, accel * g_globalvars.frametime,
			PULL_DECEL * g_globalvars.frametime);

	VectorScale(uv_hook, radialSpeed, radialVel);
	VectorAdd(radialVel, tangentialVel, self->s.v.velocity);
	self->hook_time = min(self->hook_time + g_globalvars.frametime, ACCEL_TIME);
}

void RCTF_ApplyInputControl(vec3_t tangentDir, float wishAlign, qbool forwardHeld)
{
	float accel;

	if (VectorLength(tangentDir) < EPSILON)
	{
		return;
	}

	if (wishAlign < -0.15)
	{
		return;
	}

	accel = INPUT_TANGENTIAL_ACCEL;
	if (wishAlign < 0)
	{
		accel *= 1.0 + fabs(wishAlign) * INPUT_TANGENTIAL_BACK_BIAS;
	}
	if (forwardHeld)
	{
		accel *= INPUT_FORWARD_TANGENTIAL_SCALE;
	}

	VectorMA(self->s.v.velocity, accel * g_globalvars.frametime, tangentDir, self->s.v.velocity);
}

void RCTF_ApplyGravityInfluence(vec3_t uv_hook, float maxPull, float wishAlign, qbool forwardHeld)
{
	vec3_t transVector, uv_gravity;
	float radialSpeed, radialFactor, gravityInfluence, gravityScale, gravityTangent;

	radialSpeed = DotProduct(self->s.v.velocity, uv_hook);
	radialFactor = bound(0, radialSpeed / maxPull, 1);
	if (wishAlign < -0.15)
	{
		radialFactor = max(radialFactor, bound(0, fabs(wishAlign) * INPUT_BACK_GRAVITY_FACTOR, 1));
	}

	VectorSet(uv_gravity, 0, 0, -1);
	gravityInfluence = RCTF_VectorAlignment(uv_gravity, uv_hook);
	VectorMA(uv_gravity, -gravityInfluence, uv_hook, transVector);
	gravityTangent = VectorNormalize(transVector);
	gravityScale = MIN_GRAVITY + radialFactor * (MAX_GRAVITY - MIN_GRAVITY);
	if (forwardHeld)
	{
		gravityScale *= INPUT_FORWARD_GRAVITY_SCALE;
	}

	if (gravityTangent > EPSILON && radialFactor > 0.02)
	{
		VectorMA(self->s.v.velocity, gravityScale * gravityTangent * cvar("sv_gravity") * g_globalvars.frametime,
				transVector, self->s.v.velocity);
	}
}

void RCTF_ApplyGroundBias(vec3_t uv_hook, float maxPull)
{
	float preserveFactor, scale;

	RCTF_SetMinimumRadialSpeed(self, uv_hook, GROUND_DETACH_SPEED);
	RCTF_SetMinimumGroundLift(self, uv_hook);

	scale = GROUND_TANGENTIAL_SCALE;
	preserveFactor = RCTF_SpeedPreserveFactor();
	if ((preserveFactor > 0) && (self->hook_initial_speed > maxPull))
	{
		scale += (GROUND_FAST_TANGENTIAL_SCALE - GROUND_TANGENTIAL_SCALE) * preserveFactor;
	}

	RCTF_DampenTangentialVelocity(self, uv_hook, scale);
}

void RCTF_ApplyOscillation(vec3_t uv_hook, float distanceToHook)
{
	vec3_t radialVel, tangentialVel, transVector;
	float radialSpeed, threshold, magnitude;

	threshold = OSCILLATION_THRESHOLD_SCALE * self->hook_initial_length;
	if (distanceToHook >= threshold)
	{
		return;
	}

	magnitude = RCTF_OscillationFactor(distanceToHook, threshold, DotProduct(self->s.v.velocity, uv_hook));
	VectorScale(uv_hook, magnitude, transVector);
	VectorMA(self->s.v.velocity, g_globalvars.frametime, transVector, self->s.v.velocity);

	if (self->hook_time < OSCILLATION_DAMPING_DELAY)
	{
		return;
	}

	RCTF_DecomposeVelocity(self->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	if (radialSpeed > 0)
	{
		VectorScale(uv_hook, radialSpeed * OSCILLATION_DAMPING, radialVel);
	}

	VectorScale(tangentialVel, OSCILLATION_TANGENTIAL_DAMPING, tangentialVel);
	VectorAdd(radialVel, tangentialVel, self->s.v.velocity);
}

void RCTF_CapVelocity(vec3_t uv_hook, float maxPull)
{
	vec3_t radialVel, tangentialVel;
	float radialSpeed, tangentialSpeed, totalSpeed, cap, radialCap, preserveFactor;

	RCTF_DecomposeVelocity(self->s.v.velocity, uv_hook, radialVel, tangentialVel, &radialSpeed);
	preserveFactor = RCTF_SpeedPreserveFactor();

	radialCap = RCTF_PreservedCap(maxPull * RADIAL_SPEED_CAP, self->hook_initial_radial_speed, preserveFactor);
	radialSpeed = bound(-(maxPull * RADIAL_AWAY_CAP), radialSpeed, radialCap);
	VectorScale(uv_hook, radialSpeed, radialVel);

	tangentialSpeed = VectorNormalize(tangentialVel);
	cap = RCTF_PreservedCap(maxPull * TANGENTIAL_SPEED_CAP, self->hook_initial_tangential_speed, preserveFactor);
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
	cap = RCTF_PreservedCap(maxPull * TOTAL_SPEED_CAP, self->hook_initial_speed, preserveFactor);
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
	owner->hook_tension = 0;
	owner->hook_initial_radial_speed = 0;
	owner->hook_initial_tangential_speed = 0;
	owner->hook_initial_speed = 0;
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

	owner->hook_reset_time = g_globalvars.time + RCTF_GrappleRefireDelay(owner, rhook);
	owner->attack_finished = owner->hook_reset_time + RCTF_GrapplePostRetractDelay(owner);
	NativeHookState(owner, native_hook_retracting, rhook->s.v.origin, owner->s.v.origin);
}

void RCTF_GrappleRetract(void)
{
	float hookDistance, returnSpeed, timeLeft;
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
	timeLeft = owner->hook_reset_time - g_globalvars.time;

	if (timeLeft <= g_globalvars.frametime || hookDistance <= HOOK_RETRACT_END_DISTANCE)
	{
		NativeHookState(owner, native_hook_inactive, owner->s.v.origin, owner->s.v.origin);
		SUB_Remove();
		return;
	}

	returnSpeed = (hookDistance - HOOK_RETRACT_END_DISTANCE) / timeLeft;
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

void RCTF_NativeThrownThink(void)
{
	gedict_t *owner = PROG_TO_EDICT(self->s.v.owner);

	if (!owner || owner == world || !owner->hook_out)
	{
		self->think = (func_t) SUB_Remove;
		self->s.v.nextthink = next_frame();
		return;
	}

	owner->hook_cancel_time += 1;
	if (owner->hook_cancel_time > 24)
	{
		RCTF_CancelHook(owner);
	}

	NativeHookState(owner, native_hook_thrown, self->s.v.origin, self->s.v.origin);
	self->s.v.nextthink = next_frame();
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
	vec3_t hookVector, uv_hook, uv_pull, radialVel, tangentialVel;
	float radialSpeed;
	qbool wasOnGround;

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

	wasOnGround = (int)owner->s.v.flags & FL_ONGROUND;
	RCTF_GetPullVector(uv_hook, wasOnGround, uv_pull);
	RCTF_DecomposeVelocity(owner->s.v.velocity, uv_pull, radialVel, tangentialVel, &radialSpeed);
	owner->hook_initial_length = vlen(hookVector);
	owner->hook_time = 0;
	owner->hook_tension = 0;
	owner->hook_initial_radial_speed = max(0, radialSpeed);
	owner->hook_initial_tangential_speed = VectorLength(tangentialVel);
	owner->hook_initial_speed = VectorLength(owner->s.v.velocity);
	if (!NativeHookPredictionEnabled())
	{
		RCTF_DetachFromGround(owner, uv_hook);
	}
	owner->on_hook = true;
	NativeHookState(owner, native_hook_anchored, self->s.v.origin, self->s.v.origin);

	self->s.v.enemy = EDICT_TO_PROG(other);
	self->think = (func_t) RCTF_GrappleTrack;
	self->s.v.nextthink = next_frame();
	self->s.v.solid = SOLID_NOT;
	self->touch = (func_t) SUB_Null;
}

void RCTF_GrappleService(void)
{
	gedict_t *target;
	vec3_t hookVector, uv_hook, uv_pull, wishDir, tangentDir;
	float distanceToHook, minPull, maxPull, wishAlign;
	qbool useGroundBias, wasOnGround, forwardHeld;

	if (!self->s.v.button0)
	{
		if (self->s.v.weapon == IT_HOOK)
		{
			RCTF_GrappleReset(self->hook);
			return;
		}
	}

	ExtFieldSetAlpha(self->hook, RingStealthActive(self) ? RING_STEALTH_ALPHA : 1);
	NativeHookState(self, native_hook_anchored, self->hook->s.v.origin, self->hook->s.v.origin);

	if (NativeHookPredictionEnabled())
	{
		return;
	}

	target = PROG_TO_EDICT(self->hook->s.v.enemy);
	RCTF_GetHookVector(self->hook, target, hookVector);

	VectorCopy(hookVector, uv_hook);
	VectorNormalize(uv_hook);
	distanceToHook = VectorLength(hookVector);
	wasOnGround = (int)self->s.v.flags & FL_ONGROUND;
	useGroundBias = wasOnGround && (uv_hook[2] > GROUND_DETACH_MIN_UP);
	RCTF_GetPullVector(uv_hook, wasOnGround, uv_pull);
	RCTF_ClearGrounded(self);
	minPull = RCTF_MinPullSpeed(self);
	maxPull = RCTF_MaxPullSpeed(self);
	wishAlign = RCTF_MovementInfluence(uv_pull, wishDir, tangentDir);
	forwardHeld = self->movement[0] > 0;

	RCTF_ApplyRadialPull(uv_pull, distanceToHook, minPull, maxPull, wishAlign, forwardHeld);
	RCTF_ApplyInputControl(tangentDir, wishAlign, forwardHeld);
	if (useGroundBias)
	{
		RCTF_ApplyGroundBias(uv_pull, maxPull);
	}
	else if (!wasOnGround)
	{
		RCTF_ApplyGravityInfluence(uv_pull, maxPull, wishAlign, forwardHeld);
	}
	RCTF_ApplyOscillation(uv_pull, distanceToHook);
	RCTF_CapVelocity(uv_pull, maxPull);
}

void RCTF_GrappleThrow(void)
{
	vec3_t initialVelocity, uv_throw;
	float hasteMultiplier, playerSpeed, playerInfluence, alignmentFactor;

	if (self->hook_out || self->hook_reset_time > g_globalvars.time)
	{
		return;
	}

	hasteMultiplier = RCTF_HasteMultiplier();
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
	self->hook_tension = 0;
	self->hook_initial_radial_speed = 0;
	self->hook_initial_tangential_speed = 0;
	self->hook_initial_speed = 0;

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
	if (!NativeHookPredictionEnabled())
	{
		newmis->think = (func_t) RCTF_BuildChain;
		newmis->s.v.nextthink = next_frame();
	}
	else
	{
		newmis->think = (func_t) RCTF_NativeThrownThink;
		newmis->s.v.nextthink = next_frame();
	}

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
	NativeHookState(self, native_hook_thrown, newmis->s.v.origin, newmis->s.v.origin);
}
