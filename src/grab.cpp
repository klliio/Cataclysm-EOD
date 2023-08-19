#include "game.h" // IWYU pragma: associated

#include <algorithm>
#include <cstdlib>

#include "avatar.h"
#include "debug.h"
#include "map.h"
#include "messages.h"
#include "rng.h"
#include "sounds.h"
#include "tileray.h"
#include "translations.h"
#include "units_utility.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"

bool game::grabbed_veh_move( const tripoint &dp )
{
    const optional_vpart_position grabbed_vehicle_vp = m.veh_at( u.pos() + u.grab_point );
    if( !grabbed_vehicle_vp ) {
        add_msg( m_info, _( "No vehicle at grabbed point." ) );
        u.grab( object_type::NONE );
        return false;
    }
    vehicle *grabbed_vehicle = &grabbed_vehicle_vp->vehicle();
    if( !grabbed_vehicle ||
        !grabbed_vehicle->handle_potential_theft( get_avatar() ) ) {
        return false;
    }
    const int grabbed_part = grabbed_vehicle_vp->part_index();
    if( monster *mon = grabbed_vehicle->get_harnessed_animal() ) {
        add_msg( m_info, _( "You cannot move this vehicle whilst your %s is harnessed!" ),
                 mon->get_name() );
        u.grab( object_type::NONE );
        return false;
    }
    const vehicle *veh_under_player = veh_pointer_or_null( m.veh_at( u.pos() ) );
    if( grabbed_vehicle == veh_under_player ) {
        u.grab_point = -dp;
        return false;
    }

    tripoint dp_veh = -u.grab_point;
    const tripoint prev_grab = u.grab_point;
    tripoint next_grab = u.grab_point;

    bool zigzag = false;

    if( dp == prev_grab ) {
        // We are pushing in the direction of vehicle
        dp_veh = dp;
    } else if( std::abs( dp.x + dp_veh.x ) != 2 && std::abs( dp.y + dp_veh.y ) != 2 ) {
        // Not actually moving the vehicle, don't do the checks
        u.grab_point = -( dp + dp_veh );
        return false;
    } else if( ( dp.x == prev_grab.x || dp.y == prev_grab.y ) &&
               next_grab.x != 0 && next_grab.y != 0 ) {
        // Zig-zag (or semi-zig-zag) pull: player is diagonal to vehicle
        // and moves away from it, but not directly away
        dp_veh.x = dp.x == -dp_veh.x ? 0 : dp_veh.x;
        dp_veh.y = dp.y == -dp_veh.y ? 0 : dp_veh.y;

        next_grab = -dp_veh;
        zigzag = true;
    } else {
        // We are pulling the vehicle
        next_grab = -dp;
    }

    // Make sure the mass and pivot point are correct
    grabbed_vehicle->invalidate_mass();

    int kg_per_str = 100;

    //if vehicle is rollable we modify kg_per_str based on a function of movecost per wheel.

    const auto &wheel_indices = grabbed_vehicle->wheelcache;
    if( grabbed_vehicle->valid_wheel_config() ) {
        //determine movecost for terrain touching wheels
        float avg_move_cost = 0.0f;
        const tripoint vehpos = grabbed_vehicle->global_pos3();
        for( int p : wheel_indices ) {
            const tripoint wheel_pos = vehpos + grabbed_vehicle->part( p ).precalc[0];
            const float mapcost = m.move_cost( wheel_pos, grabbed_vehicle ) * 1.0f;
            avg_move_cost += mapcost / wheel_indices.size();
        }
        //set strength check threshold
        //if vehicle has many or only one wheel (shopping cart), it is as if it had four.
        if( wheel_indices.size() > 4 || wheel_indices.size() == 1 ) {
            kg_per_str /= avg_move_cost / 2;
        } else {
            kg_per_str /= ( 4 / wheel_indices.size() ) / ( avg_move_cost / 2 );
        }
        //finally, adjust by the off-road coefficient (always 1.0 on a road, as low as 0.1 off road.)
        kg_per_str *= grabbed_vehicle->k_traction( get_map().vehicle_wheel_traction( *grabbed_vehicle ) );
        // Cap at 10 times worse than base; no matter how bad the terrain and the wheels can possibly be,
        // having no wheels would never be better than having them, though it is an extremely unlikely scenario.
        kg_per_str = std::max( 10, kg_per_str );
    } else {
        kg_per_str /= 10;
        //since it has no wheels assume it has the worst off roading possible (0.1)
    }
    // TODO: take into account engines, if any; they cause rolling resistance beyond what their weight alone would assume.
    // That would effectively add extra mass to the vehicle at this step, depending on engine power.
    const int str_req = grabbed_vehicle->total_mass() / units::from_kilogram(
                            kg_per_str ); //strength required to move vehicle.

    // ARM_STR governs dragging heavy things
    const int str = u.get_arm_str();

    //final strength check and outcomes
    ///\ARM_STR determines ability to drag vehicles
    if( str_req <= str ) {
        //calculate exertion factor and movement penalty
        ///\EFFECT_STR increases speed of dragging vehicles
        u.moves -= 400 * str_req / std::max( 1, str );
        ///\EFFECT_STR decreases stamina cost of dragging vehicles
        u.mod_stamina( -200 * str_req / std::max( 1, str ) );
        // TODO: make dragging vehicles raise the activity level upon moving depending on str to str_req ratio, causing weariness faster than simply walking.
        // TODO: once this is done, make sure that dragging vehicles never results in activity level lower than moving with no vehicle would.
        const int ex = dice( 1, 6 ) - 1 + str_req;
        if( ex > str + 1 ) {
            // Pain and movement penalty if exertion exceeds character strength
            add_msg( m_bad, _( "You strain yourself to move the %s!" ), grabbed_vehicle->name );
            u.moves -= 200;
            u.mod_pain( 1 );
        } else if( ex >= str ) {
            // Movement is slow if exertion nearly equals character strength
            add_msg( _( "It takes some time to move the %s." ), grabbed_vehicle->name );
            u.moves -= 200;
        }
        if( !grabbed_vehicle->valid_wheel_config() ) {
            //if vehicle has no wheels str_req make a noise.
            // TODO: Probably should only happen on hard surfaces such as concrete, but not on soil.
            sounds::sound( grabbed_vehicle->global_pos3(), str_req * 2, sounds::sound_t::movement,
                           _( "a scraping noise." ), true, "misc", "scraping" );
        }
    } else {
        // No moves actually spent, because the move attempt can't succeed.
        add_msg( m_info, _( "You need at least %d strength to move the %s." ), str_req,
                 grabbed_vehicle->name );
        return true;
    }

    std::string blocker_name = _( "errors in movement code" );
    const auto get_move_dir = [&]( const tripoint & dir, const tripoint & from ) {
        tileray mdir;

        mdir.init( dir.xy() );
        units::angle turn = normalize( mdir.dir() - grabbed_vehicle->face.dir() );
        if( grabbed_vehicle->is_on_ramp && turn == 180_degrees ) {
            add_msg( m_bad, _( "The %s can't be turned around while on a ramp." ), grabbed_vehicle->name );
            return tripoint_zero;
        }
        grabbed_vehicle->turn( turn );
        grabbed_vehicle->face = tileray( grabbed_vehicle->turn_dir );
        grabbed_vehicle->precalc_mounts( 1, mdir.dir(), grabbed_vehicle->pivot_point() );
        grabbed_vehicle->pos -= grabbed_vehicle->pivot_displacement();

        // Grabbed part has to stay at distance 1 to the player
        // and in roughly the same direction.
        const tripoint new_part_pos = grabbed_vehicle->global_pos3() +
                                      grabbed_vehicle->part( grabbed_part ).precalc[ 1 ];
        const tripoint expected_pos = u.pos() + dp + from;
        const tripoint actual_dir = tripoint( ( expected_pos - new_part_pos ).xy(), 0 );

        // Set player location to illegal value so it can't collide with vehicle.
        const tripoint player_prev = u.pos();
        u.setpos( tripoint_zero );
        std::vector<veh_collision> colls;
        const bool failed = grabbed_vehicle->collision( colls, actual_dir, true );
        u.setpos( player_prev );
        if( !colls.empty() ) {
            blocker_name = colls.front().target_name;
        }
        return failed ? tripoint_zero : actual_dir;
    };

    // First try the move as intended
    // But if that fails and the move is a zig-zag, try to recover:
    // Try to place the vehicle in the position player just left rather than "flattening" the zig-zag
    tripoint final_dp_veh = get_move_dir( dp_veh, next_grab );
    if( final_dp_veh == tripoint_zero && zigzag ) {
        final_dp_veh = get_move_dir( -prev_grab, -dp );
        next_grab = -dp;
    }

    if( final_dp_veh == tripoint_zero ) {
        // TODO: this check should happen before strength check, so that time and effort are never wasted trying to push vehicle into an impassable tile, and sound of moving wheelless vehicle doesn't happen as well.
        add_msg( _( "The %s collides with %s." ), grabbed_vehicle->name, blocker_name );
        u.grab_point = prev_grab;
        return true;
    }

    u.grab_point = next_grab;

    m.displace_vehicle( *grabbed_vehicle, final_dp_veh );
    m.rebuild_vehicle_level_caches();

    if( grabbed_vehicle ) {
        m.level_vehicle( *grabbed_vehicle );
        grabbed_vehicle->check_falling_or_floating();
        if( grabbed_vehicle->is_falling ) {
            add_msg( _( "You let go of the %1$s as it starts to fall." ), grabbed_vehicle->disp_name() );
            u.grab( object_type::NONE );
            m.drop_vehicle( final_dp_veh );
            return true;
        }
    } else {
        debugmsg( "Grabbed vehicle disappeared" );
        return false;
    }

    for( int p : wheel_indices ) {
        if( one_in( 2 ) ) {
            vehicle_part &vp_wheel = grabbed_vehicle->part( p );
            tripoint wheel_p = grabbed_vehicle->global_part_pos3( vp_wheel );
            grabbed_vehicle->handle_trap( wheel_p, vp_wheel );
        }
    }

    return false;

}
