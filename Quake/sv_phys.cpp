/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2020-2021 Vittorio Romeo

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sv_phys.c

#include "console.hpp"
#include <glm/fwd.hpp>
#include "quakedef.hpp"
#include "server.hpp"
#include "vr.hpp"
#include "vr_cvars.hpp"
#include "vr_gorilla_debug.hpp"
#include "world.hpp"
#include "util.hpp"
#include "quakeglm.hpp"
#include "sys.hpp"
#include "qcvm.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <tuple>
#include <vector>

vr_gorilla_debug_state_t vr_gorilla_debug_state{};

/*


pushmove objects do not obey gravity, and do not interact with each other or
trigger fields, but block normal movement and push normal objects when they
move.

onground is set for toss objects when they come to a complete rest.  it is set
for steping or walking objects

doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
corpses are SOLID_NOT and MOVETYPE_TOSS
crates are SOLID_BBOX and MOVETYPE_TOSS
walking monsters are SOLID_SLIDEBOX and MOVETYPE_STEP
flying/floating monsters are SOLID_SLIDEBOX and MOVETYPE_FLY

solid_edge items only clip against bsp models.

*/

cvar_t sv_friction = {"sv_friction", "4", CVAR_NOTIFY | CVAR_SERVERINFO};
cvar_t sv_stopspeed = {"sv_stopspeed", "100", CVAR_NONE};
cvar_t sv_gravity = {"sv_gravity", "800", CVAR_NOTIFY | CVAR_SERVERINFO};
cvar_t sv_maxvelocity = {"sv_maxvelocity", "2000", CVAR_NONE};
cvar_t sv_nostep = {"sv_nostep", "0", CVAR_NONE};
cvar_t sv_freezenonclients = {"sv_freezenonclients", "0", CVAR_NONE};
cvar_t sv_gameplayfix_spawnbeforethinks = {
    "sv_gameplayfix_spawnbeforethinks", "0", CVAR_NONE};

cvar_t sv_sound_watersplash = {
    "sv_sound_watersplash", "misc/h2ohit1.wav", CVAR_NONE};
cvar_t sv_sound_land = {"sv_sound_land", "demon/dland2.wav", CVAR_NONE};


#define MOVE_EPSILON 0.01

void SV_Physics_Toss(edict_t* ent);

/*
================
SV_CheckAllEnts
================
*/
void SV_CheckAllEnts()
{
    // see if any solid entities are inside the final position
    edict_t* check = NEXT_EDICT(qcvm->edicts);
    for(int e = 1; e < qcvm->num_edicts; e++, check = NEXT_EDICT(check))
    {
        if(check->free)
        {
            continue;
        }

        if(check->v.movetype == MOVETYPE_PUSH ||
            check->v.movetype == MOVETYPE_NONE ||
            check->v.movetype == MOVETYPE_NOCLIP)
        {
            continue;
        }

        if(SV_TestEntityPosition(check))
        {
            Con_Printf("entity in invalid position\n");
        }
    }
}

/*
================
SV_CheckVelocity
================
*/
void SV_CheckVelocity(edict_t* ent)
{
    //
    // bound velocity
    //
    for(int i = 0; i < 3; i++)
    {
        if(IS_NAN(ent->v.velocity[i]))
        {
            Con_Printf(
                "Got a NaN velocity on %s\n", PR_GetString(ent->v.classname));
            ent->v.velocity[i] = 0;
        }

        if(IS_NAN(ent->v.origin[i]))
        {
            Con_Printf(
                "Got a NaN origin on %s\n", PR_GetString(ent->v.classname));
            ent->v.origin[i] = 0;
        }

        ent->v.velocity[i] = std::clamp(ent->v.velocity[i],
            float(-sv_maxvelocity.value), float(sv_maxvelocity.value));
    }
}

/*
=============
SV_RunThink

Runs thinking code if time.  There is some play in the exact time the think
function will be called, because it is called before any movement is done
in a frame.  Not used for pushmove objects, because they must be exact.
Returns false if the entity removed itself.
=============
*/
template <auto TNextThink, auto TThink, bool TDoLerp>
bool SV_RunThinkImpl(edict_t* ent)
{
    if(!((ent->v).*TThink))
    {
        return !ent->free;
    }

    float thinktime = (ent->v).*TNextThink;
    if(thinktime <= 0 || thinktime > qcvm->time + host_frametime)
    {
        return true;
    }

    if(thinktime < qcvm->time)
    {
        thinktime = qcvm->time; // don't let things stay in the past.
    }
    // it is possible to start that way
    // by a trigger with a local time.

    float oldframe = ent->v.frame; // johnfitz

    (ent->v).*TNextThink = 0;
    pr_global_struct->time = thinktime;
    pr_global_struct->self = EDICT_TO_PROG(ent);
    pr_global_struct->other = EDICT_TO_PROG(qcvm->edicts);
    PR_ExecuteProgram((ent->v).*TThink);

    if(TDoLerp)
    {
        // johnfitz -- PROTOCOL_QUAKEVR
        // capture interval to nextthink here and send it to client for better
        // lerp timing, but only if interval is not 0.1 (which client assumes)
        ent->sendinterval = false;
        if(!ent->free && (ent->v).*TNextThink &&
            (ent->v.movetype == MOVETYPE_STEP || ent->v.frame != oldframe))
        {
            int i = Q_rint(((ent->v).*TNextThink - thinktime) * 255);
            if(i >= 0 && i < 256 && i != 25 && i != 26)
            {
                // 25 and 26 are close enough to 0.1 to not send
                ent->sendinterval = true;
            }
        }
        // johnfitz
    }

    return !ent->free;
}

bool SV_RunThink(edict_t* ent)
{
    return //
        SV_RunThinkImpl<&entvars_t::nextthink, &entvars_t::think, true>(ent) &&
        SV_RunThinkImpl<&entvars_t::nextthink2, &entvars_t::think2, false>(ent);
}

/*
==================
SV_Impact

Two entities have touched, so run their touch functions
==================
*/
void SV_Impact(edict_t* e1, edict_t* e2, func_t entvars_t::*impactFunc)
{
    const int old_self = pr_global_struct->self;
    const int old_other = pr_global_struct->other;

    pr_global_struct->time = qcvm->time;
    if(e1->v.*impactFunc && e1->v.solid != SOLID_NOT)
    {
        pr_global_struct->self = EDICT_TO_PROG(e1);
        pr_global_struct->other = EDICT_TO_PROG(e2);
        PR_ExecuteProgram(e1->v.*impactFunc);
    }

    if(e2->v.*impactFunc && e2->v.solid != SOLID_NOT)
    {
        pr_global_struct->self = EDICT_TO_PROG(e2);
        pr_global_struct->other = EDICT_TO_PROG(e1);
        PR_ExecuteProgram(e2->v.*impactFunc);
    }

    pr_global_struct->self = old_self;
    pr_global_struct->other = old_other;
}


/*
==================
ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define STOP_EPSILON 0.1

int ClipVelocity(
    const qvec3& in, const qvec3& normal, qvec3& out, float overbounce)
{
    int blocked = 0;

    if(normal[2] > 0)
    {
        blocked |= 1; // floor
    }

    if(!normal[2])
    {
        blocked |= 2; // step
    }

    const float backoff = DotProduct(in, normal) * overbounce;

    for(int i = 0; i < 3; i++)
    {
        const float change = normal[i] * backoff;
        out[i] = in[i] - change;

        if(out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
        {
            out[i] = 0;
        }
    }

    return blocked;
}

[[nodiscard]] bool SV_GorillaCollisionActiveForPlayer(
    const edict_t* const ent) noexcept
{
    return vr_gorilla_locomotion.value != 0.f && ent == sv_player &&
           ent->v.health > 0 && ent->v.movetype == MOVETYPE_WALK;
}

bool SV_GorillaBounceBodyFromTrace(
    edict_t* const ent, const trace_t& trace, const qvec3& incomingVelocity)
{
    if(!SV_GorillaCollisionActiveForPlayer(ent) || !trace.ent ||
        quake::util::traceHitGround(trace))
    {
        return false;
    }

    const qvec3 normal = trace.plane.normal;
    const qfloat inwardSpeed = DotProduct(incomingVelocity, normal);
    if(inwardSpeed >= -20._qf)
    {
        return false;
    }

    constexpr qfloat restitution = 0.55_qf;
    qvec3 bounceVelocity =
        incomingVelocity - normal * ((1._qf + restitution) * inwardSpeed);

    constexpr qfloat minOutwardSpeed = 90._qf;
    const qfloat outwardSpeed = DotProduct(bounceVelocity, normal);
    if(outwardSpeed < minOutwardSpeed)
    {
        bounceVelocity += normal * (minOutwardSpeed - outwardSpeed);
    }

    const qfloat maxBounceSpeed =
        static_cast<qfloat>(vr_gorilla_max_jump_speed.value);
    const qfloat bounceSpeed = glm::length(bounceVelocity);
    if(maxBounceSpeed > 0._qf && bounceSpeed > maxBounceSpeed)
    {
        bounceVelocity = safeNormalize(bounceVelocity) * maxBounceSpeed;
    }

    ent->v.velocity = bounceVelocity;
    quake::util::removeFlag(ent, FL_ONGROUND);
    return true;
}

/*
============
SV_FlyMove

The basic solid body movement clip that slides along multiple planes
Returns the clipflags if the velocity was modified (hit something solid)
1 = floor
2 = wall / step
4 = dead stop
If steptrace is not nullptr, the trace of any vertical wall hit will be stored
============
*/
#define MAX_CLIP_PLANES 5
int SV_FlyMove(edict_t* ent, float time, trace_t* steptrace)
{
    constexpr int numbumps = 4;

    const auto primal_velocity = ent->v.velocity;

    qvec3 planes[MAX_CLIP_PLANES];
    qvec3 original_velocity = ent->v.velocity;
    qvec3 new_velocity;

    float time_left = time;

    int blocked = 0;
    int numplanes = 0;

    for(int bumpcount = 0; bumpcount < numbumps; bumpcount++)
    {
        if(!ent->v.velocity[0] && !ent->v.velocity[1] && !ent->v.velocity[2])
        {
            break;
        }

        const auto end = ent->v.origin + time_left * ent->v.velocity;

        trace_t trace =
            SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs, end, false, ent);

        if(trace.allsolid)
        {
            // entity is trapped in another solid
            ent->v.velocity = vec3_zero;
            return 3;
        }

        if(trace.fraction > 0)
        {
            // actually covered some distance
            ent->v.origin = trace.endpos;
            original_velocity = ent->v.velocity;
            numplanes = 0;
        }

        if(trace.fraction == 1)
        {
            break; // moved the entire distance
        }

        if(!trace.ent)
        {
            Sys_Error("SV_FlyMove: !trace.ent");
        }

        if(quake::util::traceHitGround(trace))
        {
            blocked |= 1; // floor
            if(trace.ent->v.solid == SOLID_BSP)
            {
                quake::util::addFlag(ent, FL_ONGROUND);
                ent->v.groundentity = EDICT_TO_PROG(trace.ent);
            }
        }

        if(!trace.plane.normal[2])
        {
            blocked |= 2; // step
            if(steptrace)
            {
                *steptrace = trace; // save for player extrafriction
            }
        }

        //
        // run the impact function
        //
        SV_Impact(ent, trace.ent, &entvars_t::touch);

        if(ent->free)
        {
            break; // removed by the impact function
        }

        time_left -= time_left * trace.fraction;

        // cliped to another plane
        if(numplanes >= MAX_CLIP_PLANES)
        {
            // this shouldn't really happen
            ent->v.velocity = vec3_zero;
            return 3;
        }

        planes[numplanes] = trace.plane.normal;
        numplanes++;

        //
        // modify original_velocity so it parallels all of the clip planes
        //
        int i, j;
        for(i = 0; i < numplanes; i++)
        {
            ClipVelocity(original_velocity, planes[i], new_velocity, 1);
            for(j = 0; j < numplanes; j++)
            {
                if(j != i)
                {
                    if(DotProduct(new_velocity, planes[j]) < 0)
                    {
                        break; // not ok
                    }
                }
            }
            if(j == numplanes)
            {
                break;
            }
        }

        if(i != numplanes)
        {
            // go along this plane
            ent->v.velocity = new_velocity;
        }
        else
        {
            // go along the crease
            if(numplanes != 2)
            {
                //				Con_Printf ("clip velocity, numplanes ==
                //%i\n",numplanes);
                ent->v.velocity = vec3_zero;
                return 7;
            }

            const auto dir = glm::cross(planes[0], planes[1]);
            const auto d = DotProduct(dir, ent->v.velocity);
            ent->v.velocity = dir * d;
        }

        //
        // if original velocity is against the original velocity, stop dead
        // to avoid tiny occilations in sloping corners
        //
        if(DotProduct(ent->v.velocity, primal_velocity) <= 0)
        {
            ent->v.velocity = vec3_zero;
            return blocked;
        }
    }

    return blocked;
}


/*
============
SV_AddGravity

============
*/
float SV_AddGravityImpl(const float ent_gravity)
{
    return (double)ent_gravity * (double)sv_gravity.value * host_frametime;
}

float SV_AddGravityImpl(edict_t* ent)
{
    eval_t* val = GetEdictFieldValue(ent, ED_FindFieldOffset("gravity"));
    const float ent_gravity = (val && val->_float) ? val->_float : 1.0;

    return SV_AddGravityImpl(ent_gravity);
}

void SV_AddGravity(edict_t* ent)
{
    ent->v.velocity[2] -= SV_AddGravityImpl(ent);
}

/*
===============================================================================

PUSHMOVE

===============================================================================
*/

static void SV_PushEntityImpact(edict_t* ent, const trace_t& trace)
{
    if(!trace.ent)
    {
        return;
    }

    SV_Impact(ent, trace.ent, &entvars_t::touch);
}

/*
============
SV_PushEntity

Does not change the entities velocity at all
============
*/
trace_t SV_PushEntity(edict_t* ent, const qvec3& push)
{
    const auto start = ent->v.origin - push;
    const auto end = ent->v.origin + push;

    trace_t trace;
    if(ent->v.movetype == MOVETYPE_FLYMISSILE)
    {
        trace =
            SV_Move(start, ent->v.mins, ent->v.maxs, end, MOVE_MISSILE, ent);
    }
    else if(ent->v.solid == SOLID_TRIGGER || ent->v.solid == SOLID_NOT ||
            ent->v.solid == SOLID_NOT_BUT_TOUCHABLE)
    {
        // only clip against bmodels
        trace =
            SV_Move(start, ent->v.mins, ent->v.maxs, end, MOVE_NOMONSTERS, ent);
    }
    else
    {
        trace = SV_Move(start, ent->v.mins, ent->v.maxs, end, MOVE_NORMAL, ent);
    }

    ent->v.origin = trace.endpos;

    SV_LinkEdict(ent, true);
    SV_PushEntityImpact(ent, trace);

    return trace;
}

/*
============
SV_PushMove
============
*/
void SV_PushMove(edict_t* pusher, float movetime)
{
    // When changing this, test the following:
    // * Lift in E1M1
    // * Platform (controlled by button) in E1M1
    // * Crusher in E1M3
    // * Big doors in E1M3
    // * Slow elevator in E2M6
    // * Crusher in HIP3M4

    if(!pusher->v.velocity[0] && !pusher->v.velocity[1] &&
        !pusher->v.velocity[2])
    {
        pusher->v.ltime += movetime;
        return;
    }

    const auto move = pusher->v.velocity * movetime;
    const auto pusherNewMins = pusher->v.absmin + move;
    const auto pusherNewMaxs = pusher->v.absmax + move;

    const auto oldPushorig = pusher->v.origin;

    // move the pusher to it's final position

    pusher->v.origin += move;
    pusher->v.ltime += movetime;
    SV_LinkEdict(pusher, false);

    // johnfitz -- dynamically allocate
    const int mark = Hunk_LowMark(); // johnfitz
    const auto moved_edict = Hunk_Alloc<edict_t*>(qcvm->num_edicts);
    const auto moved_from = Hunk_Alloc<qvec3>(qcvm->num_edicts);
    // johnfitz

    // see if any solid entities are inside the final position
    int num_moved = 0;
    edict_t* check = NEXT_EDICT(qcvm->edicts);
    for(int e = 1; e < qcvm->num_edicts; e++, check = NEXT_EDICT(check))
    {
        if(check->free)
        {
            continue;
        }

        if(check->v.movetype == MOVETYPE_PUSH ||
            check->v.movetype == MOVETYPE_NONE ||
            check->v.movetype == MOVETYPE_NOCLIP)
        {
            continue;
        }

        // if the entity is standing on the pusher, it will definitely be moved
        if(!quake::util::hasFlag(check, FL_ONGROUND) ||
            PROG_TO_EDICT(check->v.groundentity) != pusher)
        {
            if(!quake::util::boxIntersection(check->v.absmin, check->v.absmax,
                   pusherNewMins, pusherNewMaxs))
            {
                continue;
            }

            // see if the ent's bbox is inside the pusher's final position

            const qvec3 minBottom{0.f, 0.f, -1.f};

            const bool checkIntoSolid =
                SV_TestEntityPositionCustomOrigin(check, check->v.origin);

            trace_t traceBuffer;
            qvec3 offsetBuffer;

            const bool checkOnTopOfPusher =
                quake::util::checkGroundCollision(MOVE_NOMONSTERS, check,
                    traceBuffer, offsetBuffer, minBottom, 0.f, 0.f) &&
                traceBuffer.ent == pusher;

            if(!checkIntoSolid && !checkOnTopOfPusher)
            {
                continue;
            }
        }

        // remove the onground flag for non-players
        if(check->v.movetype != MOVETYPE_WALK)
        {
            quake::util::removeFlag(check, FL_ONGROUND);
        }

        const qvec3 entorig = check->v.origin;
        moved_from[num_moved] = check->v.origin;
        moved_edict[num_moved] = check;
        ++num_moved;

        // try moving the contacted entity
        pusher->v.solid = SOLID_NOT;
        SV_PushEntity(check, move);
        pusher->v.solid = SOLID_BSP;

        if(move[2] > 0)
        {
            quake::util::addFlag(check, FL_ONGROUND);
            check->v.groundentity = EDICT_TO_PROG(pusher);
        }

        const auto checkBlock = [&](edict_t* ent, qvec3 adjUpMove)
        {
            if(adjUpMove[2] < 0)
            {
                adjUpMove[2] *= -1.f;
            }

            const trace_t trace =
                SV_Move(ent->v.origin + adjUpMove, ent->v.mins, ent->v.maxs,
                    ent->v.origin + adjUpMove, MOVE_NORMAL, ent);

            return trace.startsolid;
        };

        // if it is still inside the pusher, block
        const bool block = checkBlock(check, move);

        if(block)
        {
            // fail the move
            if(check->v.mins[0] == check->v.maxs[0])
            {
                continue;
            }

            if(check->v.solid == SOLID_NOT || check->v.solid == SOLID_TRIGGER ||
                check->v.solid == SOLID_NOT_BUT_TOUCHABLE)
            {
                continue;
            }

            check->v.origin = entorig;
            SV_LinkEdict(check, true);

            pusher->v.origin = oldPushorig;
            SV_LinkEdict(pusher, false);
            pusher->v.ltime -= movetime;

            // if the pusher has a "blocked" function, call it
            // otherwise, just stay in place until the obstacle is gone
            if(pusher->v.blocked)
            {
                pr_global_struct->self = EDICT_TO_PROG(pusher);
                pr_global_struct->other = EDICT_TO_PROG(check);
                PR_ExecuteProgram(pusher->v.blocked);
            }

            // move back any entities we already moved
            for(int i = 0; i < num_moved; i++)
            {
                moved_edict[i]->v.origin = moved_from[i];
                SV_LinkEdict(moved_edict[i], false);
            }

            Hunk_FreeToLowMark(mark); // johnfitz
            return;
        }
    }

    Hunk_FreeToLowMark(mark); // johnfitz
}

/*
================
SV_Physics_Pusher

================
*/
void SV_Physics_Pusher(edict_t* ent)
{
    const float oldltime = ent->v.ltime;
    const float thinktime = ent->v.nextthink;

    float movetime;
    if(thinktime < ent->v.ltime + host_frametime)
    {
        movetime = thinktime - ent->v.ltime;
        if(movetime < 0)
        {
            movetime = 0;
        }
    }
    else
    {
        movetime = host_frametime;
    }

    if(movetime)
    {
        SV_PushMove(ent, movetime); // advances ent->v.ltime if not blocked
    }

    if(thinktime > oldltime && thinktime <= ent->v.ltime)
    {
        ent->v.nextthink = 0;
        pr_global_struct->time = qcvm->time;
        pr_global_struct->self = EDICT_TO_PROG(ent);
        pr_global_struct->other = EDICT_TO_PROG(qcvm->edicts);
        PR_ExecuteProgram(ent->v.think);
        if(ent->free)
        {
            return;
        }
    }
}


/*
===============================================================================

CLIENT MOVEMENT

===============================================================================
*/

/*
=============
SV_CheckStuck

This is a big hack to try and fix the rare case of getting stuck in the world
clipping hull.
=============
*/
void SV_CheckStuck(edict_t* ent)
{
    if(!SV_TestEntityPosition(ent))
    {
        ent->v.oldorigin = ent->v.origin;
        return;
    }

    const qvec3 org = ent->v.origin;
    ent->v.origin = ent->v.oldorigin;

    if(!SV_TestEntityPosition(ent))
    {
        Con_DPrintf("Unstuck.\n");
        SV_LinkEdict(ent, true);
        return;
    }

    for(int z = 0; z < 18; z++)
    {
        for(int i = -1; i <= 1; i++)
        {
            for(int j = -1; j <= 1; j++)
            {
                ent->v.origin = org + qvec3{i, j, z};

                if(!SV_TestEntityPosition(ent))
                {
                    Con_DPrintf("Unstuck.\n");
                    SV_LinkEdict(ent, true);
                    return;
                }
            }
        }
    }

    ent->v.origin = org;
    Con_DPrintf("player is stuck.\n");
}


/*
=============
SV_CheckWater
=============
*/
bool SV_CheckWater(edict_t* ent)
{
    const auto prevWaterlevel = ent->v.waterlevel;

    qvec3 point = ent->v.origin;
    point[2] += ent->v.mins[2] + 1;

    ent->v.waterlevel = 0;
    ent->v.watertype = CONTENTS_EMPTY;

    int cont = SV_PointContents(point);
    if(cont <= CONTENTS_WATER)
    {
        ent->v.watertype = cont;
        ent->v.waterlevel = 1;
        point[2] = ent->v.origin[2] + (ent->v.mins[2] + ent->v.maxs[2]) * 0.5f;

        cont = SV_PointContents(point);
        if(cont <= CONTENTS_WATER)
        {
            ent->v.waterlevel = 2;
            point[2] = ent->v.origin[2] + ent->v.view_ofs[2];

            cont = SV_PointContents(point);
            if(cont <= CONTENTS_WATER)
            {
                ent->v.waterlevel = 3;
            }
        }
    }

    if(ent->v.waterlevel != prevWaterlevel)
    {
        ent->v.lastwatertime = qcvm->time;
    }

    return ent->v.waterlevel > 1;
}

/*
============
SV_WallFriction

============
*/
void SV_WallFriction(edict_t* ent, trace_t* trace)
{
    const auto fwd = quake::util::getFwdVecFromPitchYawRoll(ent->v.v_angle);
    qfloat d = DotProduct(trace->plane.normal, fwd);

    d += 0.5_qf;
    if(d >= 0)
    {
        return;
    }

    // cut the tangential velocity
    const auto i = DotProduct(trace->plane.normal, ent->v.velocity);
    const auto into = trace->plane.normal * i;
    const auto side = ent->v.velocity - qvec3(into);

    ent->v.velocity[0] = side[0] * (1 + d);
    ent->v.velocity[1] = side[1] * (1 + d);
}

/*
=====================
SV_TryUnstick

Player has come to a dead stop, possibly due to the problem with limited
float precision at some angle joins in the BSP hull.

Try fixing by pushing one pixel in each direction.

This is a hack, but in the interest of good gameplay...
======================
*/
int SV_TryUnstick(edict_t* ent, qvec3 oldvel)
{
    qvec3 oldorg;
    qvec3 dir;
    int clip;
    trace_t steptrace;

    oldorg = ent->v.origin;
    dir = vec3_zero;

    for(int i = 0; i < 8; i++)
    {
        // try pushing a little in an axial direction
        switch(i)
        {
            case 0:
                dir[0] = 2;
                dir[1] = 0;
                break;
            case 1:
                dir[0] = 0;
                dir[1] = 2;
                break;
            case 2:
                dir[0] = -2;
                dir[1] = 0;
                break;
            case 3:
                dir[0] = 0;
                dir[1] = -2;
                break;
            case 4:
                dir[0] = 2;
                dir[1] = 2;
                break;
            case 5:
                dir[0] = -2;
                dir[1] = 2;
                break;
            case 6:
                dir[0] = 2;
                dir[1] = -2;
                break;
            case 7:
                dir[0] = -2;
                dir[1] = -2;
                break;
        }

        SV_PushEntity(ent, dir);

        // retry the original move
        ent->v.velocity[0] = oldvel[0];
        ent->v.velocity[1] = oldvel[1];
        ent->v.velocity[2] = 0;
        clip = SV_FlyMove(ent, 0.1, &steptrace);

        if(fabs((double)oldorg[1] - (double)ent->v.origin[1]) > 4 ||
            fabs((double)oldorg[0] - (double)ent->v.origin[0]) > 4)
        {
            // Con_DPrintf ("unstuck!\n");
            return clip;
        }

        // go back to the original pos and try again
        ent->v.origin = oldorg;
    }

    ent->v.velocity = vec3_zero;
    return 7; // still not moving
}

/*
=====================
SV_WalkMove

Only used by players
======================
*/
void SV_WalkMove(edict_t* ent, const bool resetOnGround)
{
    //
    // do a regular slide move unless it looks like you ran into a step
    //
    const int oldonground = quake::util::hasFlag(ent, FL_ONGROUND);

    if(resetOnGround)
    {
        quake::util::removeFlag(ent, FL_ONGROUND);
    }

    const qvec3 oldorg = ent->v.origin;
    const qvec3 oldvel = ent->v.velocity;

    trace_t steptrace;
    int clip = SV_FlyMove(ent, host_frametime, &steptrace);

    if((clip & 2) && SV_GorillaBounceBodyFromTrace(ent, steptrace, oldvel))
    {
        return;
    }

    if(!(clip & 2))
    {
        return; // move didn't block on a step
    }

    if(!oldonground && ent->v.waterlevel == 0)
    {
        return; // don't stair up while jumping
    }

    if(ent->v.movetype != MOVETYPE_WALK)
    {
        return; // gibbed by a trigger
    }

    if(sv_nostep.value || (vr_gorilla_locomotion.value && ent == sv_player))
    {
        return;
    }

    if(quake::util::hasFlag(sv_player, FL_WATERJUMP))
    {
        return;
    }

    const qvec3 nosteporg = ent->v.origin;
    const qvec3 nostepvel = ent->v.velocity;

    //
    // try moving up and forward to go up a step
    //
    ent->v.origin = oldorg; // back to start pos

    constexpr float stepsize = 18.f;
    const qvec3 upmove{0.f, 0.f, stepsize};
    const qvec3 downmove{0.f, 0.f, -stepsize + oldvel[2] * host_frametime};

    // move up
    SV_PushEntity(ent, upmove); // FIXME: don't link?

    // move forward
    ent->v.velocity[0] = oldvel[0];
    ent->v.velocity[1] = oldvel[1];
    ent->v.velocity[2] = 0;
    clip = SV_FlyMove(ent, host_frametime, &steptrace);

    // check for stuckness, possibly due to the limited precision of floats
    // in the clipping hulls
    if(clip)
    {
        if(fabs((double)oldorg[1] - (double)ent->v.origin[1]) < 0.03125 &&
            fabs((double)oldorg[0] - (double)ent->v.origin[0]) < 0.03125)
        {
            // stepping up didn't make any progress
            clip = SV_TryUnstick(ent, oldvel);
        }
    }

    // extra friction based on view angle
    if(clip & 2)
    {
        SV_WallFriction(ent, &steptrace);
    }

    // move down
    const trace_t downtrace =
        SV_PushEntity(ent, downmove); // FIXME: don't link?

    if(quake::util::traceHitGround(downtrace))
    {
        if(ent->v.solid == SOLID_BSP)
        {
            quake::util::addFlag(ent, FL_ONGROUND);
            ent->v.groundentity = EDICT_TO_PROG(downtrace.ent);
        }
    }
    else
    {
        // if the push down didn't end up on good ground, use the move
        // without the step up.  This happens near wall / slope
        // combinations, and can cause the player to hop up higher on a
        // slope too steep to climb
        ent->v.origin = nosteporg;
        ent->v.velocity = nostepvel;
    }
}

/*
================
SV_Handtouch

Trigger hand-touching actions (e.g. pick up an item, press a button)
================
*/
void SV_Handtouch(edict_t* ent)
{
    // TODO VR: (P2) cleanup, too much unnecessary tracing and work

    // Utility constants
    const qvec3 handOffsets{2.5f, 2.5f, 2.5f};

    // Figure out tracing boundaries
    // (Largest possible volume containing the hands and the player)
    const auto [origin, mins, maxs] = [&]
    {
        const auto& playerOrigin = ent->v.origin;
        const auto& playerMins = ent->v.mins;
        const auto& playerMaxs = ent->v.maxs;
        const auto playerAbsMin = playerOrigin + playerMins;
        const auto playerAbsMax = playerOrigin + playerMaxs;

        const auto& mainHandOrigin = ent->v.handpos;
        const auto mainHandAbsMin = mainHandOrigin - handOffsets;
        const auto mainHandAbsMax = mainHandOrigin + handOffsets;

        const auto& offHandOrigin = ent->v.offhandpos;
        const auto offHandAbsMin = offHandOrigin - handOffsets;
        const auto offHandAbsMax = offHandOrigin + handOffsets;

        const qvec3 minBound{
            std::min({playerAbsMin.x, mainHandAbsMin.x, offHandAbsMin.x}),
            std::min({playerAbsMin.y, mainHandAbsMin.y, offHandAbsMin.y}),
            std::min({playerAbsMin.z, mainHandAbsMin.z, offHandAbsMin.z})};

        const qvec3 maxBound{
            std::max({playerAbsMax.x, mainHandAbsMax.x, offHandAbsMax.x}),
            std::max({playerAbsMax.y, mainHandAbsMax.y, offHandAbsMax.y}),
            std::max({playerAbsMax.z, mainHandAbsMax.z, offHandAbsMax.z})};

        const auto halfSize = (maxBound - minBound) / 2._qf;
        const auto origin = minBound + halfSize;
        return std::tuple{origin, -halfSize, +halfSize};
    }();

    const auto traceCheck = [&](const trace_t& trace)
    {
        if(!trace.ent)
        {
            return;
        }

        const auto handCollisionCheck =
            [&](const int hand, const qvec3& handPos)
        {
            const float bonus =
                (quake::util::hasFlag(trace.ent, FL_EASYHANDTOUCH))
                    ? VR_GetEasyHandTouchBonus()
                    : 0.f;

            const qvec3 bonusVec{bonus, bonus, bonus};

            const auto aMin =
                trace.ent->v.origin + trace.ent->v.mins - bonusVec;
            const auto aMax =
                trace.ent->v.origin + trace.ent->v.maxs + bonusVec;
            const auto bMin = handPos - handOffsets;
            const auto bMax = handPos + handOffsets;

            if(quake::util::boxIntersection(aMin, aMax, bMin, bMax))
            {
                VR_SetHandtouchParams(hand, ent, trace.ent);
                SV_Impact(ent, trace.ent, &entvars_t::handtouch);
            }
        };

        handCollisionCheck(cVR_OffHand, ent->v.offhandpos);
        handCollisionCheck(cVR_MainHand, ent->v.handpos);
    };

    const auto endHandPos = [&](const qvec3& handPos, const qvec3& handRot)
    {
        const auto fwd = quake::util::getFwdVecFromPitchYawRoll(handRot);
        return handPos + fwd * 1._qf;
    };

    const auto mainHandEnd = endHandPos(ent->v.handpos, ent->v.handrot);
    const auto offHandEnd = endHandPos(ent->v.offhandpos, ent->v.offhandrot);

    traceCheck(SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs, mainHandEnd,
        MOVE_NORMAL, ent));
    traceCheck(SV_Move(origin, mins, maxs, mainHandEnd, MOVE_NORMAL, ent));
    traceCheck(SV_Move(
        ent->v.origin, ent->v.mins, ent->v.maxs, offHandEnd, MOVE_NORMAL, ent));
    traceCheck(SV_Move(origin, mins, maxs, offHandEnd, MOVE_NORMAL, ent));

    const auto traceForHand = [&](const qvec3& handPos, const qvec3& handRot)
    {
        const auto fwd = quake::util::getFwdVecFromPitchYawRoll(handRot);
        const auto end = handPos + fwd * 1._qf;

        return SV_Move(
            handPos, -handOffsets, handOffsets, end, MOVE_NORMAL, ent);
    };

    traceCheck(traceForHand(ent->v.handpos, ent->v.handrot));
    traceCheck(traceForHand(ent->v.offhandpos, ent->v.offhandrot));
}

void SV_VRWpntouch(edict_t* ent)
{
    // TODO VR: (P2) code repetition with vr.cpp setHandPos

    const auto doHand = [&](const HandIdx handIndex)
    {
        const auto& playerOrigin = ent->v.origin;

        const auto worldHandPos = VR_GetWorldHandPos(handIndex, playerOrigin);
        const auto adjPlayerOrigin = VR_GetAdjustedPlayerOrigin(playerOrigin);

        const auto resolvedHandPos =
            VR_GetResolvedHandPos(ent, worldHandPos, adjPlayerOrigin);

        VrGunWallCollision collisionData;
        VR_UpdateGunWallCollisions(
            ent, handIndex, collisionData, resolvedHandPos);

        if(collisionData._ent != nullptr && collisionData._ent->v.vr_wpntouch)
        {
            VR_SetHandtouchParams(handIndex, ent, collisionData._ent);
            SV_Impact(ent, collisionData._ent, &entvars_t::vr_wpntouch);
        }
    };

    doHand(cVR_MainHand);
    doHand(cVR_OffHand);
}

namespace
{
constexpr int cGorillaMaxVelocityHistory = 32;
constexpr int cGorillaMovementLogCapacity = 4096;
constexpr double cGorillaPostLaunchAnchorIgnoreSeconds = 0.12;

struct GorillaHandState
{
    qvec3 lastPos{vec3_zero};
    bool touching{false};
};

struct GorillaLocomotionState
{
    bool initialized{false};
    std::array<GorillaHandState, 2> hands{};
    std::array<qvec3, cGorillaMaxVelocityHistory> velocityHistory{};
    qvec3 velocityAverage{vec3_zero};
    qvec3 lastOrigin{vec3_zero};
    qvec3 lastSafeOrigin{vec3_zero};
    double ignoreAnchorUntilTime{0.0};
    double nextDebugPrintTime{0.0};
    int velocityIndex{0};
    int velocityCount{0};
    int lastLogIndex{-1};
    bool hasLastSafeOrigin{false};
};

struct GorillaMovementLogHand
{
    qvec3 traceStart{vec3_zero};
    qvec3 desiredEnd{vec3_zero};
    qvec3 contactAnchor{vec3_zero};
    qvec3 movement{vec3_zero};
    qvec3 cmdVelocity{vec3_zero};
    qvec3 unstickTraceEnd{vec3_zero};
    qfloat cmdVelocityMagnitude{0.f};
    qfloat unstickDistance{0.f};
    bool colliding{false};
    bool previousTouching{false};
    bool touching{false};
    bool fallbackUsed{false};
    bool unstickAttempted{false};
    bool unstuck{false};
    bool unstickPathBlocked{false};
    bool unstickStartSolid{false};
    bool unstickAllSolid{false};
};

struct GorillaMovementLogFrame
{
    double time{0.0};
    double serverTime{0.0};
    qfloat frametime{0.f};
    int clientIndex{0};
    qvec3 preOrigin{vec3_zero};
    qvec3 postOrigin{vec3_zero};
    qvec3 preVelocity{vec3_zero};
    qvec3 postVelocity{vec3_zero};
    qvec3 headPos{vec3_zero};
    qvec3 bodyMovement{vec3_zero};
    qvec3 actualBodyMovement{vec3_zero};
    qvec3 velocityAverage{vec3_zero};
    qvec3 launch{vec3_zero};
    bool launched{false};
    bool handsAnchored{false};
    bool bodyBounced{false};
    bool playerTraceAllSolid{false};
    bool playerTraceStartSolid{false};
    bool launchContactIntentional{false};
    bool commandTeleporting{false};
    bool mapTeleportDuringMove{false};
    bool mapTeleportDuringFinalLink{false};
    bool teleportCooldownActive{false};
    bool gorillaStateInvalidatedForTeleport{false};
    bool gorillaPlayerUnstuck{false};
    bool finalStuckStartSolid{false};
    bool finalStuckAllSolid{false};
    qfloat teleportTimeFrameStart{0.f};
    qfloat teleportTimeBeforeFinalLink{0.f};
    qfloat teleportTimeAfterFinalLink{0.f};
    qfloat teleportExitNudge{0.f};
    qvec3 commandTeleportTarget{vec3_zero};
    qvec3 originBeforeFinalLink{vec3_zero};
    qvec3 originAfterFinalLink{vec3_zero};
    qvec3 velocityBeforeFinalLink{vec3_zero};
    qvec3 velocityAfterFinalLink{vec3_zero};
    qvec3 anglesAfterFinalLink{vec3_zero};
    qvec3 teleportTargetAfterFinalLink{vec3_zero};
    qvec3 gorillaUnstickFrom{vec3_zero};
    qvec3 gorillaUnstickTo{vec3_zero};
    qvec3 gorillaLastSafeOrigin{vec3_zero};
    qvec3 finalStuckTraceEnd{vec3_zero};
    int flags{0};
    qfloat health{0.f};
    std::array<GorillaMovementLogHand, 2> hands{};
};

std::vector<GorillaLocomotionState> gorillaLocomotionStates;
std::array<GorillaMovementLogFrame, cGorillaMovementLogCapacity>
    gorillaMovementLog{};
double gorillaNextInactiveDebugPrintTime{0.0};
double gorillaNextMovementLogAutoDumpTime{0.0};
int gorillaMovementLogWriteIndex{0};
int gorillaMovementLogCount{0};

[[nodiscard]] bool SV_GorillaLocomotionActiveForPlayer(
    const edict_t* const ent) noexcept
{
    return vr_gorilla_locomotion.value != 0.f && ent->v.health > 0 &&
           ent->v.movetype == MOVETYPE_WALK;
}

[[nodiscard]] qvec3 SV_GorillaPlayerHullMins() noexcept
{
    // Quake BSPs only provide point, player, and large monster hulls. Keep the
    // Gorilla body under a 3-unit diameter so world collision uses point hull 0
    // instead of the baked 56-unit-tall player hull.
    const qfloat radius = std::clamp(
        static_cast<qfloat>(vr_gorilla_body_radius.value), 0.25_qf, 1.4_qf);
    return {-radius, -radius, -radius};
}

[[nodiscard]] qvec3 SV_GorillaPlayerHullMaxs() noexcept
{
    const qfloat radius = std::clamp(
        static_cast<qfloat>(vr_gorilla_body_radius.value), 0.25_qf, 1.4_qf);
    return {radius, radius, radius};
}

void SV_GorillaUpdatePlayerHull(edict_t* const ent) noexcept
{
    const qvec3 mins = SV_GorillaLocomotionActiveForPlayer(ent)
                           ? SV_GorillaPlayerHullMins()
                           : qvec3{-16.f, -16.f, -24.f};

    const qvec3 maxs = SV_GorillaLocomotionActiveForPlayer(ent)
                           ? SV_GorillaPlayerHullMaxs()
                           : qvec3{16.f, 16.f, 32.f};

    ent->v.mins = mins;
    ent->v.maxs = maxs;
    ent->v.size = maxs - mins;
}

[[nodiscard]] bool SV_GorillaOriginClear(
    edict_t* const ent, const qvec3& origin)
{
    const trace_t trace =
        SV_Move(origin, ent->v.mins, ent->v.maxs, origin, MOVE_NORMAL, ent);
    return !trace.allsolid && !trace.startsolid &&
           SV_TestEntityPositionCustomOrigin(ent, origin) == nullptr;
}

void SV_GorillaRememberSafeOrigin(
    GorillaLocomotionState& state, edict_t* const ent)
{
    if(!SV_GorillaOriginClear(ent, ent->v.origin))
    {
        return;
    }

    state.lastSafeOrigin = ent->v.origin;
    state.hasLastSafeOrigin = true;
}

[[nodiscard]] bool SV_GorillaRecoverToLastSafeOrigin(
    GorillaLocomotionState& state, edict_t* const ent, qvec3& from, qvec3& to)
{
    from = ent->v.origin;
    to = state.lastSafeOrigin;

    if(!state.hasLastSafeOrigin || SV_GorillaOriginClear(ent, ent->v.origin))
    {
        return false;
    }

    const qfloat maxRecoveryDistance = std::max(0._qf,
        static_cast<qfloat>(vr_gorilla_player_unstick_distance.value));
    if(maxRecoveryDistance <= 0.001_qf ||
        glm::length(state.lastSafeOrigin - ent->v.origin) > maxRecoveryDistance)
    {
        return false;
    }

    if(!SV_GorillaOriginClear(ent, state.lastSafeOrigin))
    {
        state.hasLastSafeOrigin = false;
        return false;
    }

    ent->v.origin = state.lastSafeOrigin;
    ent->v.oldorigin = state.lastSafeOrigin;
    ent->v.velocity = vec3_zero;
    state.initialized = false;
    SV_LinkEdict(ent, false);
    return true;
}

[[nodiscard]] bool SV_GorillaDebugEnabled() noexcept
{
    return false;
}

[[nodiscard]] bool SV_GorillaShouldPrint(
    GorillaLocomotionState& state, const bool force = false) noexcept
{
    if(!SV_GorillaDebugEnabled())
    {
        return false;
    }

    if(force || realtime >= state.nextDebugPrintTime)
    {
        state.nextDebugPrintTime = realtime + 0.25;
        return true;
    }

    return false;
}

[[nodiscard]] bool SV_GorillaShouldPrintInactive() noexcept
{
    if(!SV_GorillaDebugEnabled() || realtime < gorillaNextInactiveDebugPrintTime)
    {
        return false;
    }

    gorillaNextInactiveDebugPrintTime = realtime + 0.5;
    return true;
}

[[nodiscard]] const char* SV_GorillaHandName(const HandIdx hand) noexcept
{
    return hand == cVR_OffHand ? "off" : "main";
}

[[nodiscard]] qfloat SV_GorillaCmdHandVelocityMagnitudeField(
    const usercmd_t& move, const int hand) noexcept
{
    return hand == cVR_OffHand ? move.offhandvelmag : move.handvelmag;
}

void SV_GorillaPrintVec(const char* const name, const qvec3& v)
{
    Con_Printf("%s=(%.1f %.1f %.1f)", name, v[0], v[1], v[2]);
}

void SV_GorillaWriteCsvVec(FILE* const file, const qvec3& v)
{
    std::fprintf(file, ",%.6f,%.6f,%.6f", v[0], v[1], v[2]);
}

void SV_GorillaWriteCsvHeader(FILE* const file)
{
    std::fprintf(file,
        "time,server_time,dt,client,flags,health,"
        "pre_origin_x,pre_origin_y,pre_origin_z,"
        "post_origin_x,post_origin_y,post_origin_z,"
        "pre_velocity_x,pre_velocity_y,pre_velocity_z,"
        "post_velocity_x,post_velocity_y,post_velocity_z,"
        "head_x,head_y,head_z,"
        "body_movement_x,body_movement_y,body_movement_z,"
        "actual_body_movement_x,actual_body_movement_y,actual_body_movement_z,"
        "velocity_average_x,velocity_average_y,velocity_average_z,"
        "launch_x,launch_y,launch_z,"
        "launched,hands_anchored,body_bounced,player_trace_allsolid,"
        "player_trace_startsolid,launch_contact_intentional,"
        "command_teleporting,map_teleport_during_move,"
        "map_teleport_during_final_link,teleport_cooldown_active,"
        "gorilla_state_invalidated_for_teleport,gorilla_player_unstuck,"
        "final_stuck_startsolid,final_stuck_allsolid,"
        "teleport_time_frame_start,teleport_time_before_final_link,"
        "teleport_time_after_final_link,teleport_exit_nudge,"
        "command_teleport_target_x,command_teleport_target_y,"
        "command_teleport_target_z,"
        "origin_before_final_link_x,origin_before_final_link_y,"
        "origin_before_final_link_z,"
        "origin_after_final_link_x,origin_after_final_link_y,"
        "origin_after_final_link_z,"
        "velocity_before_final_link_x,velocity_before_final_link_y,"
        "velocity_before_final_link_z,"
        "velocity_after_final_link_x,velocity_after_final_link_y,"
        "velocity_after_final_link_z,"
        "angles_after_final_link_x,angles_after_final_link_y,"
        "angles_after_final_link_z,"
        "teleport_target_after_final_link_x,"
        "teleport_target_after_final_link_y,"
        "teleport_target_after_final_link_z,"
        "gorilla_unstick_from_x,gorilla_unstick_from_y,"
        "gorilla_unstick_from_z,"
        "gorilla_unstick_to_x,gorilla_unstick_to_y,"
        "gorilla_unstick_to_z,"
        "gorilla_last_safe_origin_x,gorilla_last_safe_origin_y,"
        "gorilla_last_safe_origin_z,"
        "final_stuck_trace_end_x,final_stuck_trace_end_y,"
        "final_stuck_trace_end_z");

    for(const char* const handName : {"off", "main"})
    {
        std::fprintf(file,
            ",%s_trace_start_x,%s_trace_start_y,%s_trace_start_z,"
            "%s_desired_end_x,%s_desired_end_y,%s_desired_end_z,"
            "%s_contact_anchor_x,%s_contact_anchor_y,%s_contact_anchor_z,"
            "%s_movement_x,%s_movement_y,%s_movement_z,"
            "%s_cmd_velocity_x,%s_cmd_velocity_y,%s_cmd_velocity_z,"
            "%s_unstick_trace_end_x,%s_unstick_trace_end_y,"
            "%s_unstick_trace_end_z,"
            "%s_cmd_velocity_mag_field,%s_cmd_velocity_mag_vec,"
            "%s_unstick_distance,"
            "%s_colliding,%s_previous_touching,%s_touching,%s_fallback_used,"
            "%s_unstick_attempted,%s_unstuck,%s_unstick_path_blocked,"
            "%s_unstick_startsolid,%s_unstick_allsolid",
            handName, handName, handName, handName, handName, handName,
            handName, handName, handName, handName, handName, handName,
            handName, handName, handName, handName, handName, handName,
            handName, handName, handName, handName, handName, handName,
            handName, handName, handName, handName, handName, handName);
    }

    std::fprintf(file, "\n");
}

void SV_GorillaWriteCsvFrame(
    FILE* const file, const GorillaMovementLogFrame& frame)
{
    std::fprintf(file, "%.6f,%.6f,%.6f,%d,%d,%.1f", frame.time,
        frame.serverTime, frame.frametime, frame.clientIndex, frame.flags,
        frame.health);
    SV_GorillaWriteCsvVec(file, frame.preOrigin);
    SV_GorillaWriteCsvVec(file, frame.postOrigin);
    SV_GorillaWriteCsvVec(file, frame.preVelocity);
    SV_GorillaWriteCsvVec(file, frame.postVelocity);
    SV_GorillaWriteCsvVec(file, frame.headPos);
    SV_GorillaWriteCsvVec(file, frame.bodyMovement);
    SV_GorillaWriteCsvVec(file, frame.actualBodyMovement);
    SV_GorillaWriteCsvVec(file, frame.velocityAverage);
    SV_GorillaWriteCsvVec(file, frame.launch);
    std::fprintf(file, ",%d,%d,%d,%d,%d,%d", frame.launched ? 1 : 0,
        frame.handsAnchored ? 1 : 0, frame.bodyBounced ? 1 : 0,
        frame.playerTraceAllSolid ? 1 : 0,
        frame.playerTraceStartSolid ? 1 : 0,
        frame.launchContactIntentional ? 1 : 0);
    std::fprintf(file, ",%d,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f",
        frame.commandTeleporting ? 1 : 0,
        frame.mapTeleportDuringMove ? 1 : 0,
        frame.mapTeleportDuringFinalLink ? 1 : 0,
        frame.teleportCooldownActive ? 1 : 0,
        frame.gorillaStateInvalidatedForTeleport ? 1 : 0,
        frame.gorillaPlayerUnstuck ? 1 : 0,
        frame.finalStuckStartSolid ? 1 : 0,
        frame.finalStuckAllSolid ? 1 : 0, frame.teleportTimeFrameStart,
        frame.teleportTimeBeforeFinalLink, frame.teleportTimeAfterFinalLink,
        frame.teleportExitNudge);
    SV_GorillaWriteCsvVec(file, frame.commandTeleportTarget);
    SV_GorillaWriteCsvVec(file, frame.originBeforeFinalLink);
    SV_GorillaWriteCsvVec(file, frame.originAfterFinalLink);
    SV_GorillaWriteCsvVec(file, frame.velocityBeforeFinalLink);
    SV_GorillaWriteCsvVec(file, frame.velocityAfterFinalLink);
    SV_GorillaWriteCsvVec(file, frame.anglesAfterFinalLink);
    SV_GorillaWriteCsvVec(file, frame.teleportTargetAfterFinalLink);
    SV_GorillaWriteCsvVec(file, frame.gorillaUnstickFrom);
    SV_GorillaWriteCsvVec(file, frame.gorillaUnstickTo);
    SV_GorillaWriteCsvVec(file, frame.gorillaLastSafeOrigin);
    SV_GorillaWriteCsvVec(file, frame.finalStuckTraceEnd);

    for(const GorillaMovementLogHand& hand : frame.hands)
    {
        SV_GorillaWriteCsvVec(file, hand.traceStart);
        SV_GorillaWriteCsvVec(file, hand.desiredEnd);
        SV_GorillaWriteCsvVec(file, hand.contactAnchor);
        SV_GorillaWriteCsvVec(file, hand.movement);
        SV_GorillaWriteCsvVec(file, hand.cmdVelocity);
        SV_GorillaWriteCsvVec(file, hand.unstickTraceEnd);
        std::fprintf(file, ",%.6f,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d,%d,%d",
            hand.cmdVelocityMagnitude, glm::length(hand.cmdVelocity),
            hand.unstickDistance,
            hand.colliding ? 1 : 0, hand.previousTouching ? 1 : 0,
            hand.touching ? 1 : 0, hand.fallbackUsed ? 1 : 0,
            hand.unstickAttempted ? 1 : 0, hand.unstuck ? 1 : 0,
            hand.unstickPathBlocked ? 1 : 0,
            hand.unstickStartSolid ? 1 : 0, hand.unstickAllSolid ? 1 : 0);
    }

    std::fprintf(file, "\n");
}

[[nodiscard]] int SV_GorillaRecordMovementLog(
    const GorillaMovementLogFrame& frame) noexcept
{
    const int index = gorillaMovementLogWriteIndex;
    gorillaMovementLog[index] = frame;
    gorillaMovementLogWriteIndex =
        (gorillaMovementLogWriteIndex + 1) % cGorillaMovementLogCapacity;
    gorillaMovementLogCount =
        std::min(gorillaMovementLogCount + 1, cGorillaMovementLogCapacity);
    return index;
}

void SV_GorillaDumpMovementLog(const bool quiet)
{
    char path[MAX_OSPATH];
    q_snprintf(path, sizeof(path), "%s/gorilla_locomotion_debug.csv",
        com_gamedir);

    FILE* const file = std::fopen(path, "w");
    if(file == nullptr)
    {
        if(!quiet)
        {
            Con_Printf("gorilla: failed to write %s\n", path);
        }
        return;
    }

    SV_GorillaWriteCsvHeader(file);

    const double cutoffTime = realtime - 10.0;
    const int firstIndex =
        (gorillaMovementLogWriteIndex - gorillaMovementLogCount +
            cGorillaMovementLogCapacity) %
        cGorillaMovementLogCapacity;

    int framesWritten = 0;
    for(int i = 0; i < gorillaMovementLogCount; ++i)
    {
        const int index = (firstIndex + i) % cGorillaMovementLogCapacity;
        const GorillaMovementLogFrame& frame = gorillaMovementLog[index];
        if(frame.time < cutoffTime)
        {
            continue;
        }

        SV_GorillaWriteCsvFrame(file, frame);
        ++framesWritten;
    }

    std::fclose(file);
    if(!quiet)
    {
        Con_Printf("gorilla: wrote %d movement frames to %s\n", framesWritten,
            path);
    }
}

void SV_GorillaMaybeAutoDumpMovementLog()
{
    if(gorillaMovementLogCount <= 0)
    {
        return;
    }

    if(gorillaNextMovementLogAutoDumpTime <= 0.0)
    {
        gorillaNextMovementLogAutoDumpTime = realtime + 10.0;
        return;
    }

    if(realtime < gorillaNextMovementLogAutoDumpTime)
    {
        return;
    }

    SV_GorillaDumpMovementLog(true);
    gorillaNextMovementLogAutoDumpTime = realtime + 10.0;
}

[[nodiscard]] bool SV_GorillaUpdateLastMovementLogTeleport(
    const edict_t* const ent, const int clientIndex,
    const qfloat teleportTimeFrameStart, const qvec3& originBeforeFinalLink,
    const qvec3& velocityBeforeFinalLink,
    const qfloat teleportTimeBeforeFinalLink, const bool commandTeleporting)
{
    if(clientIndex < 0 ||
        clientIndex >= static_cast<int>(gorillaLocomotionStates.size()))
    {
        return false;
    }

    GorillaLocomotionState& state = gorillaLocomotionStates[clientIndex];
    if(state.lastLogIndex < 0)
    {
        return false;
    }

    GorillaMovementLogFrame& frame = gorillaMovementLog[state.lastLogIndex];
    constexpr qfloat teleportTimeEpsilon = 0.001_qf;
    const qfloat teleportTimeAfterFinalLink = ent->v.teleport_time;
    const trace_t stuckTrace = SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs,
        ent->v.origin, MOVE_NORMAL, const_cast<edict_t*>(ent));

    const bool mapTeleportDuringMove =
        !commandTeleporting &&
        teleportTimeBeforeFinalLink >
            teleportTimeFrameStart + teleportTimeEpsilon;
    const bool mapTeleportDuringFinalLink =
        !commandTeleporting &&
        teleportTimeAfterFinalLink >
            teleportTimeBeforeFinalLink + teleportTimeEpsilon;
    const bool invalidateForTeleport =
        commandTeleporting || mapTeleportDuringMove || mapTeleportDuringFinalLink;

    frame.commandTeleporting = commandTeleporting;
    frame.mapTeleportDuringMove = mapTeleportDuringMove;
    frame.mapTeleportDuringFinalLink = mapTeleportDuringFinalLink;
    frame.teleportCooldownActive = teleportTimeAfterFinalLink > qcvm->time;
    frame.gorillaStateInvalidatedForTeleport = invalidateForTeleport;
    frame.finalStuckStartSolid = stuckTrace.startsolid;
    frame.finalStuckAllSolid = stuckTrace.allsolid;
    frame.teleportTimeFrameStart = teleportTimeFrameStart;
    frame.teleportTimeBeforeFinalLink = teleportTimeBeforeFinalLink;
    frame.teleportTimeAfterFinalLink = teleportTimeAfterFinalLink;
    frame.teleportExitNudge =
        static_cast<qfloat>(vr_gorilla_teleport_exit_nudge.value);
    frame.commandTeleportTarget = ent->v.teleport_target;
    frame.originBeforeFinalLink = originBeforeFinalLink;
    frame.originAfterFinalLink = ent->v.origin;
    frame.velocityBeforeFinalLink = velocityBeforeFinalLink;
    frame.velocityAfterFinalLink = ent->v.velocity;
    frame.anglesAfterFinalLink = ent->v.angles;
    frame.teleportTargetAfterFinalLink = ent->v.teleport_target;
    frame.finalStuckTraceEnd = stuckTrace.endpos;
    return invalidateForTeleport;
}

void SV_GorillaMarkLastMovementLogUnstick(GorillaLocomotionState& state,
    const edict_t* const ent, const qvec3& from, const qvec3& to)
{
    if(state.lastLogIndex < 0)
    {
        return;
    }

    GorillaMovementLogFrame& frame = gorillaMovementLog[state.lastLogIndex];
    frame.gorillaPlayerUnstuck = true;
    frame.gorillaUnstickFrom = from;
    frame.gorillaUnstickTo = to;
    frame.gorillaLastSafeOrigin = state.lastSafeOrigin;
    frame.originAfterFinalLink = ent->v.origin;
    frame.velocityAfterFinalLink = ent->v.velocity;
    frame.finalStuckStartSolid = false;
    frame.finalStuckAllSolid = false;
    frame.finalStuckTraceEnd = ent->v.origin;
}

[[nodiscard]] bool SV_GorillaValidHandPosition(const qvec3& pos) noexcept
{
    return glm::length(pos) > 0.001_qf;
}

[[nodiscard]] qvec3 SV_GorillaCmdHandPos(
    const usercmd_t& move, const HandIdx hand, bool& usedFallback) noexcept
{
    const qvec3 raw =
        hand == cVR_OffHand ? move.offhandrawpos : move.handrawpos;
    if(SV_GorillaValidHandPosition(raw))
    {
        usedFallback = false;
        return raw;
    }

    usedFallback = true;
    return hand == cVR_OffHand ? move.offhandpos : move.handpos;
}

[[nodiscard]] qvec3 SV_GorillaPlayerHeadPos(const edict_t* const ent) noexcept
{
    qvec3 head = ent->v.origin + ent->v.view_ofs;
    head[2] += vr_floor_offset.value;
    head[2] -= vr_gorilla_view_drop.value;
    return head;
}

[[nodiscard]] qvec3 SV_GorillaClampHandToReach(
    const qvec3& headPos, const qvec3& handPos) noexcept
{
    const qfloat maxArmLength =
        std::max(1._qf, static_cast<qfloat>(vr_gorilla_max_arm_length.value));

    const qvec3 diff = handPos - headPos;
    const qfloat len = glm::length(diff);

    if(len <= maxArmLength)
    {
        return handPos;
    }

    return headPos + safeNormalize(diff) * maxArmLength;
}

[[nodiscard]] qvec3 SV_GorillaProjectOnPlane(
    const qvec3& value, const qvec3& normal) noexcept
{
    return value - normal * DotProduct(value, normal);
}

[[nodiscard]] qfloat SV_GorillaBspPointHullTraceRadius(
    const qfloat radius, const qfloat precision) noexcept
{
    // Quake BSP models only have point/player/monster hulls. A swept box that is
    // 3+ units wide selects the baked player hull, which can collide well in
    // front of the rendered wall. Keep Gorilla hands on hull 0 for map geometry.
    constexpr qfloat maxPointHullRadius = 1.4_qf;
    return std::clamp(radius * precision, 0.25_qf, maxPointHullRadius);
}

[[nodiscard]] bool SV_GorillaHandUnstickPathClear(edict_t* const ent,
    const qvec3& headPos, const qvec3& currentHand, const qfloat radius,
    const qfloat precision, trace_t& trace)
{
    const qvec3 toHand = currentHand - headPos;
    const qfloat distance = glm::length(toHand);
    const qfloat tracedRadius =
        SV_GorillaBspPointHullTraceRadius(radius, precision);

    trace = {};
    trace.fraction = 1.f;
    trace.endpos = currentHand;

    if(distance <= tracedRadius + 0.001_qf)
    {
        return true;
    }

    const qvec3 direction = toHand / distance;
    const qvec3 traceEnd =
        headPos + direction * std::max(0._qf, distance - tracedRadius);
    const qvec3 extents{tracedRadius, tracedRadius, tracedRadius};

    trace = SV_Move(headPos, -extents, extents, traceEnd, MOVE_NORMAL, ent);
    if(trace.allsolid || trace.startsolid || quake::util::hitSomething(trace))
    {
        return false;
    }

    const trace_t rayTrace = SV_MoveTrace(headPos, traceEnd, MOVE_NORMAL, ent);
    if(rayTrace.allsolid || rayTrace.startsolid ||
        quake::util::hitSomething(rayTrace))
    {
        trace = rayTrace;
        return false;
    }

    trace.endpos = traceEnd;
    return true;
}

[[nodiscard]] bool SV_GorillaHandTrace(edict_t* const ent,
    const qvec3& start, const qfloat radius, const qvec3& movement,
    const qfloat precision, qvec3& endPosition, trace_t& hitInfo)
{
    if(glm::length(movement) <= 0.001_qf)
    {
        endPosition = start;
        return false;
    }

    const qfloat tracedRadius =
        SV_GorillaBspPointHullTraceRadius(radius, precision);
    const qvec3 extents{tracedRadius, tracedRadius, tracedRadius};

    hitInfo = SV_Move(start, -extents, extents, start + movement, MOVE_NORMAL,
        ent);

    if(quake::util::hitSomething(hitInfo) || hitInfo.startsolid)
    {
        endPosition = hitInfo.endpos;
        return true;
    }

    const trace_t rayTrace =
        SV_MoveTrace(start, start + movement, MOVE_NORMAL, ent);
    if(quake::util::hitSomething(rayTrace) || rayTrace.startsolid)
    {
        hitInfo = rayTrace;
        endPosition = start;
        return true;
    }

    endPosition = start + movement;
    return false;
}

[[nodiscard]] bool SV_GorillaIterativeHandTrace(edict_t* const ent,
    const qvec3& start, const qfloat radius, const qvec3& movement,
    const qfloat precision, qvec3& endPosition, const bool singleHand)
{
    trace_t hitInfo;
    if(SV_GorillaHandTrace(
           ent, start, radius, movement, precision, endPosition, hitInfo))
    {
        const qvec3 firstPosition = endPosition;
        const qfloat slip = singleHand ? 0.001_qf
                                       : static_cast<qfloat>(
                                             vr_gorilla_slide_factor.value);

        const qvec3 wantedEnd = start + movement;
        const qvec3 slideMovement =
            SV_GorillaProjectOnPlane(wantedEnd - firstPosition,
                hitInfo.plane.normal) *
            slip;

        trace_t slideHit;
        if(SV_GorillaHandTrace(ent, firstPosition, radius, slideMovement,
               precision * precision, endPosition, slideHit))
        {
            return true;
        }

        const qvec3 slideEnd = firstPosition + slideMovement;
        if(SV_GorillaHandTrace(ent, slideEnd, radius, wantedEnd - slideEnd,
               precision * precision * precision, endPosition, slideHit))
        {
            return true;
        }

        endPosition = firstPosition;
        return true;
    }

    qvec3 normalizedMovement = safeNormalize(movement);
    if(glm::length(normalizedMovement) == 0.f)
    {
        endPosition = vec3_zero;
        return false;
    }

    trace_t smallHit;
    if(SV_GorillaHandTrace(ent, start, radius,
           normalizedMovement * (glm::length(movement) + radius * 0.34_qf),
           precision * 0.66_qf, endPosition, smallHit))
    {
        endPosition = start;
        return true;
    }

    endPosition = vec3_zero;
    return false;
}

void SV_GorillaStoreVelocity(
    GorillaLocomotionState& state, const qvec3& movement)
{
    const int historySize = std::clamp(static_cast<int>(
                                           vr_gorilla_history_size.value),
        1, cGorillaMaxVelocityHistory);
    state.velocityCount = std::min(state.velocityCount, historySize);
    state.velocityIndex %= historySize;

    const qvec3 frameVelocity =
        host_frametime > 0.0 ? movement / static_cast<qfloat>(host_frametime)
                             : vec3_zero;

    state.velocityIndex = (state.velocityIndex + 1) % historySize;
    const qvec3 oldestVelocity = state.velocityHistory[state.velocityIndex];
    state.velocityAverage +=
        (frameVelocity - oldestVelocity) / static_cast<qfloat>(historySize);
    state.velocityHistory[state.velocityIndex] = frameVelocity;
    state.velocityCount = std::min(state.velocityCount + 1, historySize);
}

[[nodiscard]] qfloat SV_GorillaCmdHandVelocityMagnitude(
    const usercmd_t& move, const int hand) noexcept
{
    return glm::length(hand == cVR_OffHand ? move.offhandvel : move.handvel);
}

[[nodiscard]] qvec3 SV_GorillaCmdHandVelocity(
    const usercmd_t& move, const int hand) noexcept
{
    return hand == cVR_OffHand ? move.offhandvel : move.handvel;
}

[[nodiscard]] bool SV_GorillaLaunchContactIsIntentional(
    const usercmd_t& move, const std::array<bool, 2>& colliding)
{
    const qfloat minJumpHandSpeed = std::max(
        static_cast<qfloat>(vr_gorilla_min_jump_hand_speed.value), 0._qf);
    for(int hand = 0; hand < 2; ++hand)
    {
        if(!colliding[hand])
        {
            continue;
        }

        if(SV_GorillaCmdHandVelocityMagnitude(move, hand) >=
            minJumpHandSpeed)
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] qvec3 SV_GorillaFastestCollidingHandVelocity(
    const usercmd_t& move, const std::array<bool, 2>& colliding) noexcept
{
    qvec3 handVelocity{vec3_zero};
    qfloat handSpeed = 0.f;
    for(int hand = 0; hand < 2; ++hand)
    {
        if(colliding[hand])
        {
            const qvec3 candidate = SV_GorillaCmdHandVelocity(move, hand);
            const qfloat candidateSpeed = glm::length(candidate);
            if(candidateSpeed > handSpeed)
            {
                handVelocity = candidate;
                handSpeed = candidateSpeed;
            }
        }
    }

    return handVelocity;
}

void SV_GorillaResetState(
    GorillaLocomotionState& state, edict_t* const ent, const usercmd_t& move)
{
    const double nextDebugPrintTime = state.nextDebugPrintTime;
    state = {};
    state.initialized = true;
    state.nextDebugPrintTime = nextDebugPrintTime;

    bool mainFallback;
    bool offFallback;
    state.hands[cVR_MainHand].lastPos =
        SV_GorillaCmdHandPos(move, cVR_MainHand, mainFallback);
    state.hands[cVR_OffHand].lastPos =
        SV_GorillaCmdHandPos(move, cVR_OffHand, offFallback);
    state.lastOrigin = ent->v.origin;
    SV_GorillaRememberSafeOrigin(state, ent);

    if(SV_GorillaShouldPrint(state, true))
    {
        Con_Printf("gorilla: reset client state origin=(%.1f %.1f %.1f) "
                   "mainFallback=%d offFallback=%d\n",
            ent->v.origin[0], ent->v.origin[1], ent->v.origin[2],
            mainFallback ? 1 : 0, offFallback ? 1 : 0);
    }
}

void SV_GorillaClearAnchoredVelocity(edict_t* const ent);

bool SV_GorillaApplyLaunch(GorillaLocomotionState& state, edict_t* const ent,
    const qvec3& bodyMovement, const qvec3& collidingHandVelocity,
    const bool launchContactIntentional, qvec3& launch)
{
    const qfloat velocityLimit = std::max(
        static_cast<qfloat>(vr_gorilla_velocity_limit.value), 95._qf);
    constexpr qfloat handMetersPerSecondToQuakeUnits = 95._qf;
    const qfloat collidingHandSpeed = glm::length(collidingHandVelocity);
    const qfloat handDrivenSourceSpeed =
        collidingHandSpeed * handMetersPerSecondToQuakeUnits;

    if(!launchContactIntentional)
    {
        launch = vec3_zero;
        return false;
    }

    const qvec3 currentVelocity =
        host_frametime > 0.0 ? bodyMovement / static_cast<qfloat>(host_frametime)
                             : vec3_zero;
    const qfloat currentSpeed = glm::length(currentVelocity);
    const bool handFastEnough = handDrivenSourceSpeed > velocityLimit;
    if(currentSpeed <= velocityLimit * 0.35_qf && !handFastEnough)
    {
        launch = vec3_zero;
        return false;
    }

    const qfloat speed = glm::length(state.velocityAverage);
    const bool averageFastEnough = speed > velocityLimit;
    const bool currentFastEnough = currentSpeed > velocityLimit;
    if(!averageFastEnough && !currentFastEnough && !handFastEnough)
    {
        launch = vec3_zero;
        return false;
    }

    const qfloat jumpMultiplier = std::max(
        static_cast<qfloat>(vr_gorilla_jump_multiplier.value), 0._qf);

    qvec3 launchDirection = currentSpeed > 0.001_qf
                                 ? currentVelocity / currentSpeed
                                 : vec3_zero;
    qfloat launchSourceSpeed =
        averageFastEnough ? speed : (currentFastEnough ? currentSpeed : 0._qf);
    if(averageFastEnough)
    {
        const qvec3 averageDirection = state.velocityAverage / speed;
        const qfloat averageAlignment =
            DotProduct(averageDirection, launchDirection);
        if(averageAlignment < 0.25_qf)
        {
            launchSourceSpeed =
                currentFastEnough ? currentSpeed : std::min(speed, currentSpeed);
        }
    }

    if(currentFastEnough && currentSpeed > launchSourceSpeed)
    {
        launchSourceSpeed = currentSpeed;
    }

    if(handDrivenSourceSpeed > launchSourceSpeed)
    {
        launchSourceSpeed = handDrivenSourceSpeed;
    }

    if(handFastEnough)
    {
        const qvec3 handLaunchDirection =
            safeNormalize(-collidingHandVelocity);
        if(glm::length(handLaunchDirection) > 0.001_qf)
        {
            launchDirection = handLaunchDirection;
        }
    }

    launch = launchDirection * launchSourceSpeed * jumpMultiplier;

    const qfloat launchSpeed = glm::length(launch);
    const qfloat maxJumpSpeed =
        static_cast<qfloat>(vr_gorilla_max_jump_speed.value);
    if(maxJumpSpeed > 0._qf && launchSpeed > maxJumpSpeed)
    {
        launch = safeNormalize(launch) * maxJumpSpeed;
    }

    const qfloat maxVerticalSpeed =
        static_cast<qfloat>(vr_gorilla_max_vertical_speed.value);
    if(maxVerticalSpeed > 0._qf && launch[2] > maxVerticalSpeed)
    {
        const qfloat verticalScale = maxVerticalSpeed / launch[2];
        launch *= verticalScale;
    }

    ent->v.velocity = launch;
    SV_GorillaClearAnchoredVelocity(ent);
    ent->v.velocity = launch;
    if(launch[2] > 0.f)
    {
        quake::util::removeFlag(ent, FL_ONGROUND);
    }

    return true;
}

void SV_GorillaClearAnchoredVelocity(edict_t* const ent)
{
    ent->v.velocity = vec3_zero;

    static int jumpFlagOffset = -2;
    if(jumpFlagOffset == -2)
    {
        jumpFlagOffset = ED_FindFieldOffset("jump_flag");
    }

    if(eval_t* const jumpFlag = GetEdictFieldValue(ent, jumpFlagOffset))
    {
        jumpFlag->_float = 0.f;
    }
}

bool SV_GorillaLocomotion(
    edict_t* const ent, const int clientIndex, const usercmd_t& move)
{
    if(clientIndex < 0)
    {
        return false;
    }

    const qvec3 preOrigin = ent->v.origin;
    const qvec3 preVelocity = ent->v.velocity;
    const int preFlags = ent->v.flags;
    const qfloat preHealth = ent->v.health;

    if(!SV_GorillaLocomotionActiveForPlayer(ent))
    {
        if(SV_GorillaShouldPrintInactive())
        {
            Con_Printf("gorilla: inactive client=%d enabled=%.0f health=%.1f "
                       "movetype=%.0f\n",
                clientIndex, vr_gorilla_locomotion.value, ent->v.health,
                ent->v.movetype);
        }

        if(clientIndex >= 0 &&
            clientIndex < static_cast<int>(gorillaLocomotionStates.size()))
        {
            gorillaLocomotionStates[clientIndex].initialized = false;
            gorillaLocomotionStates[clientIndex].lastLogIndex = -1;
        }
        return false;
    }

    bool mainFallback;
    bool offFallback;
    const qvec3 mainRawOrResolved =
        SV_GorillaCmdHandPos(move, cVR_MainHand, mainFallback);
    const qvec3 offRawOrResolved =
        SV_GorillaCmdHandPos(move, cVR_OffHand, offFallback);

    if(!SV_GorillaValidHandPosition(mainRawOrResolved) &&
        !SV_GorillaValidHandPosition(offRawOrResolved))
    {
        if(SV_GorillaShouldPrintInactive())
        {
            Con_Printf("gorilla: no hand input client=%d rawMainLen=%.3f "
                       "rawOffLen=%.3f resolvedMainLen=%.3f "
                       "resolvedOffLen=%.3f\n",
                clientIndex, glm::length(move.handrawpos),
                glm::length(move.offhandrawpos), glm::length(move.handpos),
                glm::length(move.offhandpos));
        }

        if(clientIndex < static_cast<int>(gorillaLocomotionStates.size()))
        {
            gorillaLocomotionStates[clientIndex].initialized = false;
            gorillaLocomotionStates[clientIndex].lastLogIndex = -1;
        }
        return false;
    }

    if(clientIndex >= static_cast<int>(gorillaLocomotionStates.size()))
    {
        gorillaLocomotionStates.resize(clientIndex + 1);
    }

    GorillaLocomotionState& state = gorillaLocomotionStates[clientIndex];
    state.lastLogIndex = -1;
    if(!state.initialized)
    {
        SV_GorillaResetState(state, ent, move);
        return false;
    }

    qvec3 frameStartUnstickFrom;
    qvec3 frameStartUnstickTo;
    if(SV_GorillaRecoverToLastSafeOrigin(
           state, ent, frameStartUnstickFrom, frameStartUnstickTo))
    {
        return false;
    }

    const qfloat radius =
        std::max(0.25_qf, static_cast<qfloat>(vr_gorilla_hand_radius.value));
    const qfloat precision = std::clamp(
        static_cast<qfloat>(vr_gorilla_precision.value), 0.5_qf, 0.9999_qf);

    qvec3 headPos = SV_GorillaPlayerHeadPos(ent);
    std::array<qvec3, 2> currentHands{
        SV_GorillaClampHandToReach(headPos, offRawOrResolved),
        SV_GorillaClampHandToReach(headPos, mainRawOrResolved)};

    std::array<bool, 2> colliding{};
    std::array<qvec3, 2> handMovement{};
    std::array<qvec3, 2> handTraceStart{};
    std::array<qvec3, 2> handDesiredEnd{};
    std::array<qvec3, 2> handUnstickTraceEnd{};
    std::array<qfloat, 2> handUnstickDistance{};
    std::array<bool, 2> handUnstickAttempted{};
    std::array<bool, 2> handUnstuck{};
    std::array<bool, 2> handUnstickPathBlocked{};
    std::array<bool, 2> handUnstickStartSolid{};
    std::array<bool, 2> handUnstickAllSolid{};
    const std::array<bool, 2> fallbackUsed{offFallback, mainFallback};
    const std::array<bool, 2> previousTouching{
        state.hands[cVR_OffHand].touching,
        state.hands[cVR_MainHand].touching};

    const bool previouslyBothTouching =
        state.hands[cVR_OffHand].touching &&
        state.hands[cVR_MainHand].touching;

    for(int hand = 0; hand < 2; ++hand)
    {
        const qvec3 gravityBias{0.f, 0.f,
            static_cast<qfloat>(-2.f * sv_gravity.value * host_frametime *
                                host_frametime)};

        const qvec3 distanceTraveled =
            currentHands[hand] - state.hands[hand].lastPos + gravityBias;
        handTraceStart[hand] = state.hands[hand].lastPos;
        handDesiredEnd[hand] = currentHands[hand];

        qvec3 finalPosition;
        const bool singleHand = !previouslyBothTouching;
        if(SV_GorillaIterativeHandTrace(ent, state.hands[hand].lastPos, radius,
               distanceTraveled, precision, finalPosition, singleHand))
        {
            handMovement[hand] =
                state.hands[hand].touching
                    ? state.hands[hand].lastPos - currentHands[hand]
                    : finalPosition - currentHands[hand];
            colliding[hand] = true;
        }
    }

    qvec3 bodyMovement{vec3_zero};
    if((colliding[cVR_OffHand] || state.hands[cVR_OffHand].touching) &&
        (colliding[cVR_MainHand] || state.hands[cVR_MainHand].touching))
    {
        bodyMovement =
            (handMovement[cVR_OffHand] + handMovement[cVR_MainHand]) / 2._qf;
    }
    else
    {
        bodyMovement = handMovement[cVR_OffHand] + handMovement[cVR_MainHand];
    }

    qvec3 actualBodyMovement{vec3_zero};
    bool bodyBounced = false;
    bool playerTraceAllSolid = false;
    bool playerTraceStartSolid = false;
    if(glm::length(bodyMovement) > 0.001_qf)
    {
        const qvec3 oldOrigin = ent->v.origin;
        const trace_t playerTrace = SV_Move(ent->v.origin, ent->v.mins,
            ent->v.maxs, ent->v.origin + bodyMovement, MOVE_NORMAL, ent);
        playerTraceAllSolid = playerTrace.allsolid;
        playerTraceStartSolid = playerTrace.startsolid;

        if(!playerTrace.allsolid)
        {
            ent->v.origin = playerTrace.endpos;
            actualBodyMovement = ent->v.origin - oldOrigin;
            headPos += actualBodyMovement;
            for(qvec3& hand : currentHands)
            {
                hand += actualBodyMovement;
            }

            if(quake::util::hitSomething(playerTrace))
            {
                const qvec3 bodyVelocity =
                    host_frametime > 0.0
                        ? bodyMovement / static_cast<qfloat>(host_frametime)
                        : bodyMovement;
                bodyBounced = SV_GorillaBounceBodyFromTrace(
                    ent, playerTrace, bodyVelocity);
            }
        }
    }

    for(int hand = 0; hand < 2; ++hand)
    {
        qvec3 finalPosition;
        const qvec3 distanceTraveled =
            currentHands[hand] - state.hands[hand].lastPos;

        const bool singleHand = !((colliding[cVR_OffHand] ||
                                      state.hands[cVR_OffHand].touching) &&
                                  (colliding[cVR_MainHand] ||
                                      state.hands[cVR_MainHand].touching));

        if(SV_GorillaIterativeHandTrace(ent, state.hands[hand].lastPos, radius,
               distanceTraveled, precision, finalPosition, singleHand))
        {
            state.hands[hand].lastPos = finalPosition;
            colliding[hand] = true;
        }
        else
        {
            state.hands[hand].lastPos = currentHands[hand];
        }
    }

    const bool launchContactIntentional =
        SV_GorillaLaunchContactIsIntentional(move, colliding);
    const qvec3 collidingHandVelocity =
        SV_GorillaFastestCollidingHandVelocity(move, colliding);
    SV_GorillaStoreVelocity(
        state, launchContactIntentional ? actualBodyMovement : vec3_zero);

    qvec3 launch{vec3_zero};
    bool launched = false;
    if((colliding[cVR_OffHand] || colliding[cVR_MainHand]))
    {
        launched = SV_GorillaApplyLaunch(state, ent, bodyMovement,
            collidingHandVelocity, launchContactIntentional, launch);
    }

    if(launched)
    {
        state.ignoreAnchorUntilTime =
            realtime + cGorillaPostLaunchAnchorIgnoreSeconds;

        for(int hand = 0; hand < 2; ++hand)
        {
            state.hands[hand].touching = false;
            colliding[hand] = false;
        }
    }
    else if(realtime < state.ignoreAnchorUntilTime)
    {
        for(int hand = 0; hand < 2; ++hand)
        {
            if(colliding[hand])
            {
                state.hands[hand].touching = false;
                colliding[hand] = false;
            }
        }
    }

    const bool anyHandContact =
        colliding[cVR_OffHand] || colliding[cVR_MainHand];
    const bool handsAnchored = anyHandContact && !launched && !bodyBounced;
    if(handsAnchored)
    {
        SV_GorillaClearAnchoredVelocity(ent);
    }

    for(int hand = 0; hand < 2; ++hand)
    {
        handUnstickTraceEnd[hand] = currentHands[hand];

        if(!colliding[hand])
        {
            state.hands[hand].touching = false;
            continue;
        }

        const qvec3 toCurrentHand =
            currentHands[hand] - state.hands[hand].lastPos;
        handUnstickDistance[hand] = glm::length(toCurrentHand);

        const qfloat handUnstickDistanceThreshold = std::max(
            0._qf, static_cast<qfloat>(vr_gorilla_unstick_distance.value));
        if(handUnstickDistance[hand] > handUnstickDistanceThreshold)
        {
            handUnstickAttempted[hand] = true;

            trace_t unstickTrace;
            const bool pathClear = SV_GorillaHandUnstickPathClear(ent, headPos,
                currentHands[hand], radius, precision, unstickTrace);
            handUnstickTraceEnd[hand] = unstickTrace.endpos;
            handUnstickStartSolid[hand] = unstickTrace.startsolid;
            handUnstickAllSolid[hand] = unstickTrace.allsolid;
            handUnstickPathBlocked[hand] = !pathClear;

            if(pathClear)
            {
                state.hands[hand].lastPos = currentHands[hand];
                colliding[hand] = false;
                handUnstuck[hand] = true;
            }
        }

        state.hands[hand].touching = colliding[hand];
    }

    GorillaMovementLogFrame logFrame;
    logFrame.time = realtime;
    logFrame.serverTime = qcvm->time;
    logFrame.frametime = static_cast<qfloat>(host_frametime);
    logFrame.clientIndex = clientIndex;
    logFrame.preOrigin = preOrigin;
    logFrame.postOrigin = ent->v.origin;
    logFrame.preVelocity = preVelocity;
    logFrame.postVelocity = ent->v.velocity;
    logFrame.headPos = headPos;
    logFrame.bodyMovement = bodyMovement;
    logFrame.actualBodyMovement = actualBodyMovement;
    logFrame.velocityAverage = state.velocityAverage;
    logFrame.launch = launch;
    logFrame.launched = launched;
    logFrame.handsAnchored = handsAnchored;
    logFrame.bodyBounced = bodyBounced;
    logFrame.playerTraceAllSolid = playerTraceAllSolid;
    logFrame.playerTraceStartSolid = playerTraceStartSolid;
    logFrame.launchContactIntentional = launchContactIntentional;
    logFrame.teleportTimeFrameStart = ent->v.teleport_time;
    logFrame.teleportTimeBeforeFinalLink = ent->v.teleport_time;
    logFrame.teleportTimeAfterFinalLink = ent->v.teleport_time;
    logFrame.teleportExitNudge =
        static_cast<qfloat>(vr_gorilla_teleport_exit_nudge.value);
    logFrame.commandTeleportTarget = ent->v.teleport_target;
    logFrame.originBeforeFinalLink = ent->v.origin;
    logFrame.originAfterFinalLink = ent->v.origin;
    logFrame.velocityBeforeFinalLink = ent->v.velocity;
    logFrame.velocityAfterFinalLink = ent->v.velocity;
    logFrame.anglesAfterFinalLink = ent->v.angles;
    logFrame.teleportTargetAfterFinalLink = ent->v.teleport_target;
    logFrame.gorillaUnstickFrom = ent->v.origin;
    logFrame.gorillaUnstickTo = ent->v.origin;
    logFrame.gorillaLastSafeOrigin =
        state.hasLastSafeOrigin ? state.lastSafeOrigin : ent->v.origin;
    logFrame.finalStuckTraceEnd = ent->v.origin;
    logFrame.flags = preFlags;
    logFrame.health = preHealth;

    for(int hand = 0; hand < 2; ++hand)
    {
        GorillaMovementLogHand& logHand = logFrame.hands[hand];
        logHand.traceStart = handTraceStart[hand];
        logHand.desiredEnd = handDesiredEnd[hand];
        logHand.contactAnchor = state.hands[hand].lastPos;
        logHand.movement = handMovement[hand];
        logHand.cmdVelocity = SV_GorillaCmdHandVelocity(move, hand);
        logHand.unstickTraceEnd = handUnstickTraceEnd[hand];
        logHand.cmdVelocityMagnitude =
            SV_GorillaCmdHandVelocityMagnitudeField(move, hand);
        logHand.unstickDistance = handUnstickDistance[hand];
        logHand.colliding = colliding[hand];
        logHand.previousTouching = previousTouching[hand];
        logHand.touching = state.hands[hand].touching;
        logHand.fallbackUsed = fallbackUsed[hand];
        logHand.unstickAttempted = handUnstickAttempted[hand];
        logHand.unstuck = handUnstuck[hand];
        logHand.unstickPathBlocked = handUnstickPathBlocked[hand];
        logHand.unstickStartSolid = handUnstickStartSolid[hand];
        logHand.unstickAllSolid = handUnstickAllSolid[hand];
    }

    state.lastLogIndex = SV_GorillaRecordMovementLog(logFrame);
    SV_GorillaMaybeAutoDumpMovementLog();

    if(vr_gorilla_debug.value != 0.f)
    {
        vr_gorilla_debug_state.valid = true;
        vr_gorilla_debug_state.origin = ent->v.origin;
        vr_gorilla_debug_state.body_movement = bodyMovement;
        vr_gorilla_debug_state.actual_body_movement = actualBodyMovement;
        vr_gorilla_debug_state.velocity_average = state.velocityAverage;
        vr_gorilla_debug_state.launch = launch;
        vr_gorilla_debug_state.launched = launched;
        vr_gorilla_debug_state.hands_anchored = handsAnchored;
        vr_gorilla_debug_state.body_bounced = bodyBounced;
        vr_gorilla_debug_state.player_trace_allsolid = playerTraceAllSolid;
        vr_gorilla_debug_state.player_trace_startsolid = playerTraceStartSolid;
        vr_gorilla_debug_state.launch_contact_intentional =
            launchContactIntentional;

        for(int hand = 0; hand < 2; ++hand)
        {
            vr_gorilla_debug_hand_t& debugHand =
                vr_gorilla_debug_state.hands[hand];
            debugHand.trace_start = handTraceStart[hand];
            debugHand.desired_end = handDesiredEnd[hand];
            debugHand.contact_anchor = state.hands[hand].lastPos;
            debugHand.movement = handMovement[hand];
            debugHand.velocity = SV_GorillaCmdHandVelocity(move, hand);
            debugHand.colliding = colliding[hand];
            debugHand.touching = state.hands[hand].touching;
            debugHand.previous_touching = previousTouching[hand];
        }
    }
    else
    {
        vr_gorilla_debug_state.valid = false;
    }

    const bool contactChanged =
        previousTouching[cVR_OffHand] != state.hands[cVR_OffHand].touching ||
        previousTouching[cVR_MainHand] != state.hands[cVR_MainHand].touching;

    if(SV_GorillaShouldPrint(state, contactChanged || launched))
    {
        Con_Printf("gorilla: client=%d contact off=%d main=%d fallback off=%d "
                   "main=%d avgSpeed=%.1f launched=%d allsolid=%d "
                   "startsolid=%d\n",
            clientIndex, state.hands[cVR_OffHand].touching ? 1 : 0,
            state.hands[cVR_MainHand].touching ? 1 : 0,
            fallbackUsed[cVR_OffHand] ? 1 : 0,
            fallbackUsed[cVR_MainHand] ? 1 : 0,
            glm::length(state.velocityAverage), launched ? 1 : 0,
            playerTraceAllSolid ? 1 : 0, playerTraceStartSolid ? 1 : 0);

        Con_Printf("gorilla: ");
        SV_GorillaPrintVec("head", headPos);
        Con_Printf(" ");
        SV_GorillaPrintVec("origin", ent->v.origin);
        Con_Printf(" ");
        SV_GorillaPrintVec("body", bodyMovement);
        Con_Printf(" ");
        SV_GorillaPrintVec("actual", actualBodyMovement);
        Con_Printf(" ");
        SV_GorillaPrintVec("velAvg", state.velocityAverage);
        Con_Printf(" ");
        SV_GorillaPrintVec("launch", launch);
        Con_Printf("\n");

        for(const HandIdx hand : {cVR_OffHand, cVR_MainHand})
        {
            Con_Printf("gorilla: hand=%s ", SV_GorillaHandName(hand));
            SV_GorillaPrintVec("current", currentHands[hand]);
            Con_Printf(" ");
            SV_GorillaPrintVec("last", state.hands[hand].lastPos);
            Con_Printf(" ");
            SV_GorillaPrintVec("move", handMovement[hand]);
            Con_Printf(" reach=%.1f\n",
                glm::length(currentHands[hand] - headPos));
        }
    }

    state.lastOrigin = ent->v.origin;
    return handsAnchored;
}

void SV_GorillaStorePostPhysicsOrigin(
    edict_t* const ent, const int clientIndex) noexcept
{
    if(clientIndex < 0 ||
        clientIndex >= static_cast<int>(gorillaLocomotionStates.size()))
    {
        return;
    }

    GorillaLocomotionState& state = gorillaLocomotionStates[clientIndex];
    if(state.initialized && SV_GorillaLocomotionActiveForPlayer(ent))
    {
        state.lastOrigin = ent->v.origin;
        SV_GorillaRememberSafeOrigin(state, ent);
    }
}
} // namespace

void SV_GorillaDumpMovementLog_f()
{
    SV_GorillaDumpMovementLog(false);
}



/*
================
SV_Physics_Client

Player character actions
================
*/
void SV_Physics_Client(edict_t* ent, int num)
{
    if(!svs.clients[num - 1].active)
    {
        return; // unconnected slot
    }

    const qfloat teleportTimeFrameStart = ent->v.teleport_time;

    //
    // call standard client pre-think
    //
    pr_global_struct->time = qcvm->time;
    pr_global_struct->self = EDICT_TO_PROG(ent);
    PR_ExecuteProgram(pr_global_struct->PlayerPreThink);

    SV_GorillaUpdatePlayerHull(ent);

    //
    // do a move
    //
    SV_CheckVelocity(ent);

    //
    // VR hands
    //
    const bool gorillaHandsAnchored =
        SV_GorillaLocomotion(ent, num - 1, svs.clients[num - 1].cmd);
    SV_Handtouch(ent);
    if(!SV_GorillaLocomotionActiveForPlayer(ent))
    {
        SV_VRWpntouch(ent);
    }

    //
    // decide which move function to call
    //
    const bool commandTeleporting =
        quake::util::hasFlag(ent->v.vrbits0, QVR_VRBITS0_TELEPORTING);
    if(commandTeleporting)
    {
        if(!SV_RunThink(ent))
        {
            return;
        }

        ent->v.teleport_time = qcvm->time + 0.3;
        ent->v.origin = ent->v.teleport_target;
        ent->v.oldorigin = ent->v.teleport_target;
    }
    else
    {
        switch((int)ent->v.movetype)
        {
            case MOVETYPE_NONE:
            {
                if(!SV_RunThink(ent))
                {
                    return;
                }

                break;
            }

            case MOVETYPE_WALK:
            {
                if(!SV_RunThink(ent))
                {
                    return;
                }

                if(!gorillaHandsAnchored && !SV_CheckWater(ent) &&
                    !(quake::util::hasFlag(ent, FL_WATERJUMP)))
                {
                    SV_AddGravity(ent);
                }

                if(!gorillaHandsAnchored)
                {
                    SV_CheckStuck(ent);
                    SV_WalkMove(ent, true /* reset onground */);
                }

                break;
            }

            case MOVETYPE_TOSS: [[fallthrough]];
            case MOVETYPE_BOUNCE:
            {
                SV_Physics_Toss(ent);
                break;
            }

            case MOVETYPE_FLY:
            {
                if(!SV_RunThink(ent))
                {
                    return;
                }

                SV_FlyMove(ent, host_frametime, nullptr);
                break;
            }

            case MOVETYPE_NOCLIP:
            {
                if(!SV_RunThink(ent))
                {
                    return;
                }

                ent->v.origin +=
                    static_cast<float>(host_frametime) * ent->v.velocity;
                break;
            }

            default:
            {
                Sys_Error(
                    "SV_Physics_client: bad movetype %i", (int)ent->v.movetype);
            }
        }

        // --------------------------------------------------------------------
        // VR: Room scale movement for entities.
        {
            const auto restoreVel = ent->v.velocity;

            ent->v.velocity[0] = ent->v.roomscalemove[0];
            ent->v.velocity[1] = ent->v.roomscalemove[1];
            ent->v.velocity[2] = 0.f;

            switch((int)ent->v.movetype)
            {
                case MOVETYPE_WALK:
                {
                    SV_CheckStuck(ent);
                    SV_WalkMove(ent, false /* reset onground */);

                    break;
                }

                case MOVETYPE_NONE: [[fallthrough]];
                case MOVETYPE_TOSS: [[fallthrough]];
                case MOVETYPE_BOUNCE: break;

                case MOVETYPE_FLY:
                {
                    SV_FlyMove(ent, host_frametime, nullptr);

                    break;
                }

                case MOVETYPE_NOCLIP:
                {
                    ent->v.origin +=
                        static_cast<float>(host_frametime) * ent->v.velocity;

                    break;
                }

                default:
                {
                    Sys_Error("SV_Physics_client: bad movetype %i",
                        (int)ent->v.movetype);
                }
            }

            ent->v.velocity = restoreVel;
        }
        // --------------------------------------------------------------------
    }

    //
    // call standard player post-think
    //
    const qvec3 originBeforeFinalLink = ent->v.origin;
    const qvec3 velocityBeforeFinalLink = ent->v.velocity;
    const qfloat teleportTimeBeforeFinalLink = ent->v.teleport_time;
    SV_LinkEdict(ent, true);
    const bool gorillaInvalidateForTeleport =
        SV_GorillaUpdateLastMovementLogTeleport(ent, num - 1,
            teleportTimeFrameStart, originBeforeFinalLink,
            velocityBeforeFinalLink, teleportTimeBeforeFinalLink,
            commandTeleporting);
    if(gorillaInvalidateForTeleport && SV_GorillaLocomotionActiveForPlayer(ent))
    {
        if(num - 1 >= static_cast<int>(gorillaLocomotionStates.size()))
        {
            gorillaLocomotionStates.resize(num);
        }
        GorillaLocomotionState& state = gorillaLocomotionStates[num - 1];
        state.hasLastSafeOrigin = false;
        SV_GorillaRememberSafeOrigin(state, ent);
        state.initialized = false;
        state.lastLogIndex = -1;
    }
    else if(SV_GorillaLocomotionActiveForPlayer(ent) && num - 1 >= 0 &&
            num - 1 < static_cast<int>(gorillaLocomotionStates.size()))
    {
        GorillaLocomotionState& state = gorillaLocomotionStates[num - 1];
        qvec3 unstickFrom;
        qvec3 unstickTo;
        if(SV_GorillaRecoverToLastSafeOrigin(
               state, ent, unstickFrom, unstickTo))
        {
            SV_GorillaMarkLastMovementLogUnstick(
                state, ent, unstickFrom, unstickTo);
        }
        else
        {
            SV_GorillaRememberSafeOrigin(state, ent);
        }
    }
    SV_GorillaStorePostPhysicsOrigin(ent, num - 1);

    pr_global_struct->time = qcvm->time;
    pr_global_struct->self = EDICT_TO_PROG(ent);

    PR_ExecuteProgram(pr_global_struct->PlayerPostThink);
}

//============================================================================

/*
=============
SV_Physics_None

Non moving objects can only think
=============
*/
void SV_Physics_None(edict_t* ent)
{
    // regular thinking
    SV_RunThink(ent);
}

/*
=============
SV_Physics_Noclip

A moving object that doesn't obey physics
=============
*/
void SV_Physics_Noclip(edict_t* ent)
{
    // regular thinking
    if(!SV_RunThink(ent))
    {
        return;
    }

    ent->v.angles += static_cast<float>(host_frametime) * ent->v.avelocity;
    ent->v.origin += static_cast<float>(host_frametime) * ent->v.velocity;

    SV_LinkEdict(ent, false);
}

/*
==============================================================================

TOSS / BOUNCE

==============================================================================
*/

/*
=============
SV_CheckWaterTransition

=============
*/
void SV_CheckWaterTransition(edict_t* ent)
{
    const int cont = SV_PointContents(ent->v.origin);
    const auto prevWaterlevel = ent->v.waterlevel;

    if(!ent->v.watertype)
    {
        // just spawned here
        ent->v.watertype = cont;
        ent->v.waterlevel = 1;
        return;
    }

    const float watertimeDiff = qcvm->time - ent->v.lastwatertime;

    if(cont <= CONTENTS_WATER)
    {
        if(ent->v.watertype == CONTENTS_EMPTY && watertimeDiff > 0.2f)
        {
            // just crossed into water
            SV_StartSound(ent, nullptr, 0, sv_sound_watersplash.string, 255, 1);
        }

        ent->v.watertype = cont;
        ent->v.waterlevel = 1;

        if(ent->v.waterlevel != prevWaterlevel)
        {
            ent->v.lastwatertime = qcvm->time;
        }
    }
    else
    {
        if(ent->v.watertype != CONTENTS_EMPTY && watertimeDiff > 0.2f)
        {
            // just crossed into water
            SV_StartSound(ent, nullptr, 0, sv_sound_watersplash.string, 255, 1);
        }

        ent->v.watertype = CONTENTS_EMPTY;
        ent->v.waterlevel = cont;

        if(ent->v.waterlevel != prevWaterlevel)
        {
            ent->v.lastwatertime = qcvm->time;
        }
    }
}

/*
=============
SV_Physics_Toss

Toss, bounce, and fly movement.  When onground, do nothing.
=============
*/
void SV_Physics_Toss(edict_t* ent)
{
    // regular thinking
    if(!SV_RunThink(ent))
    {
        return;
    }

    // update "on ground" status, stop/bounce if on ground
    {
        qvec3 vel = ent->v.velocity;

        if(ent->v.movetype != MOVETYPE_FLY &&
            ent->v.movetype != MOVETYPE_FLYMISSILE)
        {
            vel[2] -= SV_AddGravityImpl(ent);
        }

        const qvec3 move = vel * static_cast<float>(host_frametime);

        trace_t traceBuffer;
        qvec3 offsetBuffer;

        if(!quake::util::checkGroundCollision(MOVE_NOMONSTERS, ent, traceBuffer,
               offsetBuffer, move, 0._qf, 0._qf))
        {
            if(quake::util::hasFlag(ent, FL_ONGROUND))
            {
                // remove on ground, if entity is not on ground anymore
                quake::util::removeFlag(ent, FL_ONGROUND);
            }
        }
        else
        {
            const float backoff =
                ent->v.movetype == MOVETYPE_BOUNCE ? 1.5_qf : 1._qf;

            ClipVelocity(ent->v.velocity, traceBuffer.plane.normal,
                ent->v.velocity, backoff);

            if(ent->v.velocity[2] < 60 || ent->v.movetype != MOVETYPE_BOUNCE)
            {
                if(!quake::util::hasFlag(ent, FL_ONGROUND))
                {
                    quake::util::addFlag(ent, FL_ONGROUND);

                    ent->v.groundentity = EDICT_TO_PROG(traceBuffer.ent);
                    ent->v.velocity = ent->v.avelocity = vec3_zero;
                    ent->v.origin = qvec3(traceBuffer.endpos) - ent->v.mins[2] -
                                    qvec3(offsetBuffer);

                    SV_LinkEdict(ent, true);
                    SV_PushEntityImpact(ent, traceBuffer);
                }

                return;
            }
        }
    }

    SV_CheckVelocity(ent);

    // add gravity
    if(ent->v.movetype != MOVETYPE_FLY &&
        ent->v.movetype != MOVETYPE_FLYMISSILE)
    {
        SV_AddGravity(ent);
    }

    // move angles
    ent->v.angles += static_cast<float>(host_frametime) * ent->v.avelocity;

    // move origin
    const qvec3 move = ent->v.velocity * static_cast<float>(host_frametime);

    const trace_t trace = SV_PushEntity(ent, move);
    if(quake::util::hitSomething(trace) && !ent->free)
    {
        const float backoff = ent->v.movetype == MOVETYPE_BOUNCE ? 1.5f : 1.f;
        ClipVelocity(
            ent->v.velocity, trace.plane.normal, ent->v.velocity, backoff);
    }

    // check for in water
    SV_CheckWaterTransition(ent);
}

/*
===============================================================================

STEPPING MOVEMENT

===============================================================================
*/

/*
=============
SV_Physics_Step

Monsters freefall when they don't have a ground entity, otherwise
all movement is done with discrete steps.

This is also used for objects that have become still on the ground, but
will fall if the floor is pulled out from under them.
=============
*/
void SV_Physics_Step(edict_t* ent)
{
    bool hitsound;

    // freefall if not onground
    if(!quake::util::hasAnyFlag(ent, FL_ONGROUND, FL_FLY, FL_SWIM))
    {
        if(ent->v.velocity[2] < sv_gravity.value * -0.1)
        {
            hitsound = true;
        }
        else
        {
            hitsound = false;
        }

        SV_AddGravity(ent);
        SV_CheckVelocity(ent);
        SV_FlyMove(ent, host_frametime, nullptr);
        SV_LinkEdict(ent, true);

        if(quake::util::hasFlag(ent, FL_ONGROUND)) // just hit ground
        {
            if(hitsound)
            {
                SV_StartSound(ent, nullptr, 0, sv_sound_land.string, 255, 1);
            }
        }
    }

    // regular thinking
    SV_RunThink(ent);

    SV_CheckWaterTransition(ent);
}


//============================================================================

/*
================
SV_Physics

================
*/
void SV_Physics()
{
    int i;
    int entity_cap; // For sv_freezenonclients
    edict_t* ent;

    // let the progs know that a new frame has started
    pr_global_struct->self = EDICT_TO_PROG(qcvm->edicts);
    pr_global_struct->other = EDICT_TO_PROG(qcvm->edicts);
    pr_global_struct->time = qcvm->time;
    PR_ExecuteProgram(pr_global_struct->StartFrame);

    // SV_CheckAllEnts ();

    //
    // treat each object in turn
    //
    ent = qcvm->edicts;

    if(sv_freezenonclients.value)
    {
        entity_cap =
            svs.maxclients + 1; // Only run physics on clients and the world
    }
    else
    {
        entity_cap = qcvm->num_edicts;
    }

    // for (i=0 ; i<qcvm->num_edicts ; i++, ent = NEXT_EDICT(ent))
    for(i = 0; i < entity_cap; i++, ent = NEXT_EDICT(ent))
    {
        if(ent->free)
        {
            continue;
        }

        if(pr_global_struct->force_retouch)
        {
            SV_LinkEdict(ent, true); // force retouch even for stationary
        }

        if(i > 0 && i <= svs.maxclients)
        {
            SV_Physics_Client(ent, i);
        }
        else if(ent->v.movetype == MOVETYPE_PUSH)
        {
            SV_Physics_Pusher(ent);
        }
        else if(ent->v.movetype == MOVETYPE_NONE)
        {
            SV_Physics_None(ent);
        }
        else if(ent->v.movetype == MOVETYPE_NOCLIP)
        {
            SV_Physics_Noclip(ent);
        }
        else if(ent->v.movetype == MOVETYPE_STEP)
        {
            SV_Physics_Step(ent);
        }
        else if(ent->v.movetype == MOVETYPE_TOSS ||
                ent->v.movetype == MOVETYPE_BOUNCE ||
                ent->v.movetype == MOVETYPE_FLY ||
                ent->v.movetype == MOVETYPE_FLYMISSILE)
        {
            SV_Physics_Toss(ent);
        }
        else
        {
            Sys_Error("SV_Physics: bad movetype %i", (int)ent->v.movetype);
        }
    }

    if(pr_global_struct->force_retouch)
    {
        pr_global_struct->force_retouch--;
    }

    if(!sv_freezenonclients.value)
    {
        qcvm->time += host_frametime;
    }
}
