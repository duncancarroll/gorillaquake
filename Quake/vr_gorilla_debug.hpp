#pragma once

#include "quakeglm_qvec3.hpp"

#include <array>

struct vr_gorilla_debug_hand_t
{
    qvec3 trace_start{vec3_zero};
    qvec3 desired_end{vec3_zero};
    qvec3 contact_anchor{vec3_zero};
    qvec3 movement{vec3_zero};
    qvec3 velocity{vec3_zero};
    bool colliding{false};
    bool touching{false};
    bool previous_touching{false};
};

struct vr_gorilla_debug_state_t
{
    std::array<vr_gorilla_debug_hand_t, 2> hands{};
    qvec3 origin{vec3_zero};
    qvec3 body_movement{vec3_zero};
    qvec3 actual_body_movement{vec3_zero};
    qvec3 velocity_average{vec3_zero};
    qvec3 launch{vec3_zero};
    bool valid{false};
    bool launched{false};
    bool hands_anchored{false};
    bool body_bounced{false};
    bool player_trace_allsolid{false};
    bool player_trace_startsolid{false};
    bool launch_contact_intentional{false};
};

extern vr_gorilla_debug_state_t vr_gorilla_debug_state;
