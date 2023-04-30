#include "character.h"
#include "flag.h"
#include "map.h"
#include "messages.h"
#include "morale.h"
#include "map_iterator.h"

static const efftype_id effect_sleep( "sleep" );
static const efftype_id effect_took_prozac( "took_prozac" );
static const efftype_id effect_took_xanax( "took_xanax" );

static const itype_id itype_foodperson_mask( "foodperson_mask" );
static const itype_id itype_foodperson_mask_on( "foodperson_mask_on" );

static const trait_id trait_HOARDER( "HOARDER" );
static const trait_id trait_M_IMMUNE( "M_IMMUNE" );
static const trait_id trait_NUDIST( "NUDIST" );
static const trait_id trait_NOMAD( "NOMAD" );
static const trait_id trait_NOMAD2( "NOMAD2" );
static const trait_id trait_NOMAD3( "NOMAD3" );
static const trait_id trait_PROF_FOODP( "PROF_FOODP" );

static const json_character_flag json_flag_TOUGH_FEET( "TOUGH_FEET" );

static const sub_bodypart_str_id sub_body_part_torso_upper( "torso_upper" );
static const sub_bodypart_str_id sub_body_part_leg_hip_l( "leg_hip_l" );
static const sub_bodypart_str_id sub_body_part_leg_hip_r( "leg_hip_r" );

void Character::update_morale()
{
    morale->decay( 1_minutes );
    apply_persistent_morale();
}

void Character::hoarder_morale_penalty()
{
    // For hoarders holsters count as a flat -1 penalty for being empty, we also give them a 25% allowence on their pockets below 1000_ml
    int pen = ( ( free_space() - holster_volume() ) - ( small_pocket_volume() / 4 ) ) / 125_ml;
    pen += empty_holsters();
    if( pen > 70 ) {
        pen = 70;
    }
    if( pen <= 0 ) {
        pen = 0;
    }
    if( has_effect( effect_took_xanax ) ) {
        pen = pen / 7;
    } else if( has_effect( effect_took_prozac ) ) {
        pen = pen / 2;
    }
    if( pen > 0 ) {
        add_morale( MORALE_PERM_HOARDER, -pen, -pen, 1_minutes, 1_minutes, true );
    }
}

void Character::apply_persistent_morale()
{
    // None of the below currently applies if character is sleeping. This both makes sense and increases performance during sleep.
    if( !has_effect( effect_sleep ) ) {
        // Hoarders get a morale penalty if they're not carrying a full inventory.
        if( has_trait( trait_HOARDER ) ) {
            hoarder_morale_penalty();
        }
        // Nomads get a morale penalty if they stay near the same overmap tiles too long.
        if( has_trait( trait_NOMAD ) || has_trait( trait_NOMAD2 ) || has_trait( trait_NOMAD3 ) ) {
            nomad_morale_penalty();
        }

        if( has_trait( trait_PROF_FOODP ) ) {
            foodperson_morale_penalty();
        }
        map &here = get_map();
        const bool swimmable = here.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, pos() ) &&
                               here.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, pos() );
        const bool mycus_on_fungus = here.has_flag_ter_or_furn( ter_furn_flag::TFLAG_FUNGUS, pos() ) &&
                                     has_trait( trait_M_IMMUNE );
        // TODO: Currently there is a small delay between wearing proper clothing and morale penalty disappearing. There shouldn't be.
        if( ( !swimmable || in_vehicle ) && !has_flag( json_flag_TOUGH_FEET ) &&
            !worn_with_flag( flag_TOUGH_FEET ) && !mycus_on_fungus ) {
            barefoot_morale_penalty();
        }

        naked_morale_penalty();
    }
}

void Character::nomad_morale_penalty()
{
    const tripoint_abs_omt ompos = global_omt_location();
    float total_time = 0.0f;
    // Check how long we've stayed in any overmap tile within 5 of us.
    const int max_dist = 5;
    for( const tripoint_abs_omt &pos : points_in_radius( ompos, max_dist ) ) {
        const float dist = rl_dist( ompos, pos );
        if( dist > max_dist ) {
            continue;
        }
        const auto iter = overmap_time.find( pos.xy() );
        if( iter == overmap_time.end() ) {
            continue;
        }
        // Count time in own tile fully, tiles one away as 4/5, tiles two away as 3/5, etc.
        total_time += to_moves<float>( iter->second ) * ( max_dist - dist ) / max_dist;
    }
    // Characters with higher tiers of Nomad suffer worse morale penalties, faster.
    int max_unhappiness;
    float min_time;
    float max_time;
    if( has_trait( trait_NOMAD ) ) {
        max_unhappiness = 20;
        min_time = to_moves<float>( 12_hours );
        max_time = to_moves<float>( 1_days );
    } else if( has_trait( trait_NOMAD2 ) ) {
        max_unhappiness = 40;
        min_time = to_moves<float>( 4_hours );
        max_time = to_moves<float>( 8_hours );
    } else { // traid_NOMAD3
        max_unhappiness = 60;
        min_time = to_moves<float>( 1_hours );
        max_time = to_moves<float>( 2_hours );
    }
    // The penalty starts at 1 at min_time and scales up to max_unhappiness at max_time.
    const float t = ( total_time - min_time ) / ( max_time - min_time );
    const int pen = std::ceil( lerp_clamped( 0, max_unhappiness, t ) );
    if( pen > 0 ) {
        add_morale( MORALE_PERM_NOMAD, -pen, -pen, 1_minutes, 1_minutes, true );
    }
}

void Character::foodperson_morale_penalty()
{
    // Losing your face is distressing
    // TODO: make this into a flag to enable multiple variants of Foodperson masks
    if( !( is_wearing( itype_foodperson_mask ) ||
           is_wearing( itype_foodperson_mask_on ) ) ) {
        add_morale( MORALE_PERM_NOFACE, -20, -20, 1_minutes, 1_minutes, true );
    } else if( is_wearing( itype_foodperson_mask ) ||
               is_wearing( itype_foodperson_mask_on ) ) {
        rem_morale( MORALE_PERM_NOFACE );
    }

    if( is_wearing( itype_foodperson_mask_on ) ) {
        add_morale( MORALE_PERM_FPMODE_ON, 10, 10, 1_minutes, 1_minutes, true );
    } else {
        rem_morale( MORALE_PERM_FPMODE_ON );
    }
}

void Character::barefoot_morale_penalty()
{
    int pen = 0;
    if( !is_wearing_shoes( side::LEFT ) ) {
        pen += 2;
    }
    if( !is_wearing_shoes( side::RIGHT ) ) {
        pen += 2;
    }
    map &here = get_map();
    // No specific logic here; generally people just find it much less acceptable to walk barefoot outoors than indoors.
    if( !in_vehicle && here.is_outside( pos() ) ) {
        pen *= 3;
    }
    if( pen > 0 ) {
        add_morale( MORALE_PERM_BAREFOOT, -pen, -pen, 1_minutes, 1_minutes, true );
    } else {
        rem_morale( MORALE_PERM_BAREFOOT );
    }
}

void Character::naked_morale_penalty()
{
    int pen = 0;
    pen += wearing_something_on( sub_body_part_torso_upper ) ? 0 : ( male ? 2 : 10 );
    // Hips are currently the sub body parts occupied by underwear (there's no groin subpart), so that is why they are checked here.
    pen += wearing_something_on( sub_body_part_leg_hip_l ) ? 0 : 10;
    pen += wearing_something_on( sub_body_part_leg_hip_r ) ? 0 : 10;
    pen = std::min( pen, 20 );
    if( has_trait( trait_NUDIST ) ) {
        pen = ( -pen ) / 2; // Nudists get a morale bonus instead of penalty, but it's halved.
    }
    if( pen != 0 ) {
        add_morale( MORALE_PERM_NAKED, -pen, -pen, 1_minutes, 1_minutes, true );
    } else {
        rem_morale( MORALE_PERM_NAKED );
    }
}

int Character::get_morale_level() const
{
    return morale->get_level();
}

void Character::add_morale( const morale_type &type, int bonus, int max_bonus,
                            const time_duration &duration, const time_duration &decay_start,
                            bool capped, const itype *item_type )
{
    morale->add( type, bonus, max_bonus, duration, decay_start, capped, item_type );
}

int Character::has_morale( const morale_type &type ) const
{
    return morale->has( type );
}

void Character::rem_morale( const morale_type &type, const itype *item_type )
{
    morale->remove( type, item_type );
}

void Character::clear_morale()
{
    morale->clear();
}

bool Character::has_morale_to_read() const
{
    return get_morale_level() >= -40;
}

void outfit::check_and_recover_morale( player_morale &test_morale ) const
{
    for( const item &wit : worn ) {
        test_morale.on_item_wear( wit );
    }
}

void Character::check_and_recover_morale()
{
    player_morale test_morale;

    worn.check_and_recover_morale( test_morale );

    for( const trait_id &mut : get_mutations() ) {
        test_morale.on_mutation_gain( mut );
    }

    for( auto &elem : *effects ) {
        for( std::pair<const bodypart_id, effect> &_effect_it : elem.second ) {
            const effect &e = _effect_it.second;
            test_morale.on_effect_int_change( e.get_id(), e.get_intensity(), e.get_bp() );
        }
    }

    test_morale.on_stat_change( "hunger", get_hunger() );
    test_morale.on_stat_change( "thirst", get_thirst() );
    test_morale.on_stat_change( "fatigue", get_fatigue() );
    test_morale.on_stat_change( "pain", get_pain() );
    test_morale.on_stat_change( "pkill", get_painkiller() );
    test_morale.on_stat_change( "perceived_pain", get_perceived_pain() );
    test_morale.on_stat_change( "radiation", get_rad() );

    apply_persistent_morale();

    if( !morale->consistent_with( test_morale ) ) {
        *morale = player_morale( test_morale ); // Recover consistency
        add_msg_debug( debugmode::DF_CHARACTER, "%s morale was recovered.", disp_name( true ) );
    }
}

