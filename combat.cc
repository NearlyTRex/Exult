/*
 *	combat.h - Combat scheduling.
 *
 *  Copyright (C) 2000-2001  The Exult Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "combat.h"
#include "combat_opts.h"
#include "gamewin.h"
#include "gameclk.h"
#include "gamemap.h"
#include "actors.h"
#include "paths.h"
#include "Astar.h"
#include "actions.h"
#include "items.h"
#include "effects.h"
#include "Audio.h"
#include "ready.h"
#include "game.h"
#include "monstinf.h"
#include "ucmachine.h"
#include "game.h"
#include "Gump_manager.h"
#include "spellbook.h"

#ifndef UNDER_CE
using std::cout;
using std::endl;
using std::rand;
#endif
using std::list;

unsigned long Combat_schedule::battle_time = (unsigned long) (-30000);
unsigned long Combat_schedule::battle_end_time = 0;

bool Combat::paused = false;
int Combat::difficulty = 0;
Combat::Mode Combat::mode = Combat::original;
bool Combat::show_hits = false;

extern bool combat_trace;

const int dex_to_attack = 30;

/*
 *	Is a given ammo shape in a given family.
 */

bool In_ammo_family(int shnum, int family)
	{
	if (shnum == family)
		return true;
	Ammo_info *ainf = ShapeID::get_info(shnum).get_ammo_info();
	return (ainf != 0 && ainf->get_family_shape() == family);
	}

/*
 *	Start music if battle has recently started.
 */

void Combat_schedule::start_battle
	(
	)
	{
	if (started_battle)
		return;
					// But only if Avatar is main char.
	if (gwin->get_camera_actor() != gwin->get_main_actor())
		return;
	unsigned long curtime = Game::get_ticks();
					// .5 minute since last start?
	if (curtime - battle_time >= 30000)
		{
		Audio::get_ptr()->start_music_combat(rand()%2 ? 
					CSAttacked1 : CSAttacked2, 0);
		battle_time = curtime;
		battle_end_time = curtime - 1;
		}
	started_battle = true;
	}

/*
 *	This (static) method is called when a monster dies.  It checks to
 *	see if there are still hostile NPC's around.  If not, it plays
 *	'victory' music.
 */

void Combat_schedule::monster_died
	(
	)
	{
	if (battle_end_time >= battle_time)// Battle raging?
		return;			// No, it's over.
	Actor_vector nearby;		// Get all nearby NPC's.
	gwin->get_nearby_npcs(nearby);
	for (Actor_vector::const_iterator it = nearby.begin(); 
						it != nearby.end(); ++it)
		{
		Actor *actor = *it;
		if (!actor->is_dead() && 
			actor->get_attack_mode() != Actor::flee &&
			actor->get_effective_alignment() >= Npc_actor::hostile)
			return;		// Still possible enemies.
		}
	battle_end_time = Game::get_ticks();
					// Figure #seconds battle lasted.
	unsigned long len = (battle_end_time - battle_time)/1000;
	bool hard = len > 15 && (rand()%60 < len);
	Audio::get_ptr()->start_music_combat (hard ? CSBattle_Over
							: CSVictory, 0);
	}

/*
 *	Can a given shape teleport? summon?  turn invisible?
 */

static bool Can_teleport
	(
	Actor *npc
	)
	{
	Monster_info *minfo;
	int shnum = npc->get_shapenum();
	switch (shnum)
		{
	case 534:			// Wisp.
	case 154:
	case 445:
	case 446:			// Mages.
	case 354:
	case 519:			// Liche.
		return true;
	default:
		minfo = ShapeID::get_info(shnum).get_monster_info();
		return minfo ? minfo->can_teleport() : false;
		}
	}

static bool Can_summon
	(
	Actor *npc
	)
	{
	Monster_info *minfo;
	int shnum = npc->get_shapenum();
	switch (shnum)
		{
	case 154:
	case 445:
	case 446:			// Mages.
	case 354:
	case 519:			// Liche.
		return true;
	default:
		minfo = ShapeID::get_info(shnum).get_monster_info();
		return minfo ? minfo->can_summon() : false;
		}
	}

static bool Can_be_invisible
	(
	Actor *npc
	)
	{
	Monster_info *minfo;
	int shnum = npc->get_shapenum();
	switch (shnum)
		{
	case 504:			// Dragon.
	case 299:
	case 317:			// Ghosts.
	case 154:
	case 445:
	case 446:			// Mages.
	case 354:
	case 519:			// Liche.
		return true;
	default:
		minfo = ShapeID::get_info(shnum).get_monster_info();
		return minfo ? minfo->can_be_invisible() : false;
		}
	}

/*
 *	Certain monsters (wisps, mages) can teleport during battle.
 */

bool Combat_schedule::teleport
	(
	)
	{
	Game_object *trg = npc->get_target();	// Want to get close to targ.
	if (!trg)
		return false;
	unsigned int curtime = SDL_GetTicks();
	if (curtime < teleport_time)
		return false;
	teleport_time = curtime + 2000 + rand()%2000;
	Tile_coord dest = trg->get_tile();
	dest.tx += 4 - rand()%8;
	dest.ty += 4 - rand()%8;
	dest = Map_chunk::find_spot(dest, 3, npc, 1);
	if (dest.tx == -1)
		return false;		// No spot found.
	Tile_coord src = npc->get_tile();
	if (dest.distance(src) > 7 && rand()%2 != 0)
		return false;		// Got to give Avatar a chance to
					//   get away.
					// Create fire-field where he was.
	src.tz = npc->get_chunk()->get_highest_blocked(src.tz,
			src.tx%c_tiles_per_chunk, src.ty%c_tiles_per_chunk);
	if (src.tz < 0)
		src.tz = 0;
	eman->add_effect(new Fire_field_effect(src));
	int sfx = Audio::game_sfx(43);
	Audio::get_ptr()->play_sound_effect(sfx);	// The weird noise.
	npc->move(dest.tx, dest.ty, dest.tz);
					// Show the stars.
	eman->add_effect(new Sprites_effect(7, npc, 0, 0, 0, 0));
	return true;
	}

/*
 *	Some monsters can do the "summon" spell.
 */

bool Combat_schedule::summon
	(
	)
	{
	unsigned int curtime = SDL_GetTicks();
	if (curtime < summon_time)
		return false;
	// Not again for 30-40 seconds.
	summon_time = curtime + 30000 + rand()%10000;
	ucmachine->call_usecode(0x685,
			    npc, Usecode_machine::double_click);
	return true;
	}

/*
 *	Some can turn invisible.
 */

bool Combat_schedule::be_invisible
	(
	)
	{
	unsigned int curtime = SDL_GetTicks();
	if (curtime < invisible_time)
		return false;
	// Not again for 40-60 seconds.
	invisible_time = curtime + 40000 + rand()%20000;
	if (npc->get_flag(Obj_flags::invisible))
		return false;	// Also don't to it again for a while.
	npc->set_flag(Obj_flags::invisible);
	return true;
	}

/*
 *	Off-screen?
 */

inline bool Off_screen
	(
	Game_window *gwin,
	Game_object *npc
	)
	{
					// See if off screen.
	Tile_coord t = npc->get_tile();
	Rectangle screen = gwin->get_win_tile_rect().enlarge(2);
	return (!screen.has_point(t.tx, t.ty));
	}

/*
 *	Find nearby opponents in the 9 surrounding chunks.
 */

void Combat_schedule::find_opponents
	(
	)
{
	opponents.clear();
	Game_window *gwin = Game_window::get_instance();
	Actor_vector nearby;			// Get all nearby NPC's.
	gwin->get_nearby_npcs(nearby);
	nearby.push_back(gwin->get_main_actor());	// Incl. Avatar!
					// See if we're a party member.
	bool in_party = npc->is_in_party();
	int npc_align = npc->get_effective_alignment();
	for (Actor_vector::const_iterator it = nearby.begin(); 
						it != nearby.end(); ++it)
	{
		Actor *actor = *it;
		if (actor->is_dead() || 
		    (actor->get_flag(Obj_flags::invisible) && rand()%10))
			continue;	// Dead or invisible.
		if (npc_align == Npc_actor::friendly &&
		    actor->get_effective_alignment() >= Npc_actor::hostile)
			opponents.push_back(actor);
		else if (npc_align >= Npc_actor::hostile &&
			(actor->is_in_party() ||
			 actor->get_effective_alignment() == 
						Npc_actor::friendly))
			opponents.push_back(actor);
		else if (in_party)
			{		// Attacking party member?
			Game_object *t = actor->get_target();
			if (t && t->get_flag(Obj_flags::in_party))
				opponents.push_back(actor);
			}
	}
					// None found?  Use Avatar's.
	if (opponents.empty() && npc->is_in_party() &&
	    npc != gwin->get_main_actor())
	{
		Game_object *opp = gwin->get_main_actor()->get_target();
		Actor *oppnpc = opp ? opp->as_actor() : 0;
		if (oppnpc && oppnpc != npc)
			opponents.push_back(oppnpc);
	}
}		

/*
 *	Find 'protected' party member's attackers.
 *
 *	Output:	->attacker in opponents, or opponents.end() if not found.
 */

list<Actor*>::iterator Combat_schedule::find_protected_attacker
	(
	)
	{
	if (!npc->is_in_party())	// Not in party?
		return opponents.end();
	Game_window *gwin = Game_window::get_instance();
	Actor *party[9];		// Get entire party, including Avatar.
	int cnt = gwin->get_party(party, 1);
	Actor *prot_actor = 0;
	for (int i = 0; i < cnt; i++)
		if (party[i]->is_combat_protected())
			{
			prot_actor = party[i];
			break;
			}
	if (!prot_actor)		// Not found?
		return opponents.end();
					// Find closest attacker.
	int dist, best_dist = 4*c_tiles_per_chunk;
	list<Actor*>::iterator best_opp = opponents.end();
	for (list<Actor*>::iterator it = opponents.begin(); 
						it != opponents.end(); ++it)
		{
		Actor *opp = *it;
		if (opp->get_target() == prot_actor &&
		    (dist = npc->distance(opp)) < best_dist)
			{
			best_dist = dist;
			best_opp = it;
			}
		}
	if (best_opp == opponents.end())
		return 0;
	if (failures < 5 && yelled && rand()%2 && npc != prot_actor)
		npc->say(first_will_help, last_will_help);
	return best_opp;
	}

/*
 *	Find a foe.
 *
 *	Output:	Opponent that was found.
 */

Game_object *Combat_schedule::find_foe
	(
	int mode			// Mode to use.
	)
{
	if (combat_trace) {
		cout << "'" << npc->get_name() << "' is looking for a foe" << 
									endl;
	}
	int new_align = npc->get_effective_alignment();
	if (new_align != alignment)
		{			// Alignment changed.
		opponents.clear();
		alignment = new_align;
		}
					// Remove any that died.
	for (list<Actor*>::iterator it = opponents.begin(); 
						it != opponents.end(); )
		{
		if ((*it)->is_dead())
			it = opponents.erase(it);
		else
			++it;
		}
	if (opponents.empty())	// No more from last scan?
		{
		find_opponents();	// Find all nearby.
		if (practice_target)	// For dueling.
			return practice_target;
		}
	list<Actor*>::iterator new_opp_link = opponents.end();
	switch ((Actor::Attack_mode) mode)
		{
	case Actor::weakest:
		{
		int str, least_str = 100;
		for (list<Actor*>::iterator it = opponents.begin(); 
					it != opponents.end(); ++it)
			{
			Actor *opp = *it;
			str = opp->get_property(Actor::strength);
			if (str < least_str)
				{
				least_str = str;
				new_opp_link = it;
				}
			}
		break;
		}
	case Actor::strongest:
		{
		int str, best_str = -100;
		for (list<Actor*>::iterator it = opponents.begin(); 
						it != opponents.end(); ++it)
			{
			Actor *opp = *it;
			str = opp->get_property(Actor::strength);
			if (str > best_str)
				{
				best_str = str;
				new_opp_link = it;
				}
			}
		break;
		}
	case Actor::nearest:
		{
		int best_dist = 4*c_tiles_per_chunk;
		for (list<Actor*>::iterator it = opponents.begin(); 
						it != opponents.end(); ++it)
			{
			Actor *opp = *it;
			int dist = npc->distance(opp);
			if (opp->get_attack_mode() == Actor::flee)
				dist += 16;	// Avoid fleeing.
			if (dist < best_dist)
				{
				best_dist = dist;
				new_opp_link = it;
				}
			}
		break;
		}
	case Actor::protect:
		new_opp_link = find_protected_attacker();
		if (new_opp_link != opponents.end())
			break;		// Found one.
					// FALL THROUGH to 'random'.
	case Actor::random:
	default:			// Default to random.
		if (!opponents.empty())
			new_opp_link = opponents.begin();
		break;
		}
	Actor *new_opponent;
	if (new_opp_link == opponents.end())
		new_opponent = 0;
	else
		{
		new_opponent = *new_opp_link;
		opponents.erase(new_opp_link);
		}
	return new_opponent;
	}

/*
 *	Find a foe.
 *
 *	Output:	Opponent that was found.
 */

inline Game_object *Combat_schedule::find_foe
	(
	)
	{
	if (npc->get_attack_mode() == Actor::manual)
		return 0;		// Find it yourself.
	return find_foe(static_cast<int>(npc->get_attack_mode()));
	}

/*
 *	Handle the 'approach' state.
 */

void Combat_schedule::approach_foe
	(
	bool for_projectile		// Want to attack with projectile.
					// FOR NOW:  Called as last resort,
					//  and we try to reach target.
	)
	{
	int dist = for_projectile ? 1 : max_range;
	Game_object *opponent = npc->get_target();
					// Find opponent.
	if (!opponent && !(opponent = find_foe()))
		{
		failures++;
		npc->start(200, 400);	// Try again in 2/5 sec.
		return;			// No one left to fight.
		}
	npc->set_target(opponent);
	Actor::Attack_mode mode = npc->get_attack_mode();
	Game_window *gwin = Game_window::get_instance();
					// Time to run?
	if (mode == Actor::flee || 
	    (mode != Actor::beserk && 
	        (npc->get_type_flags()&MOVE_ALL) != 0 &&
		npc != gwin->get_main_actor() &&
					npc->get_property(Actor::health) < 3))
		{
		run_away();
		return;
		}
	if (rand()%4 == 0 && Can_teleport(npc) &&	// Try 1/4 to teleport.
	    teleport())
		{
		start_battle();
		npc->start(gwin->get_std_delay(), gwin->get_std_delay());
		return;
		}
	PathFinder *path = new Astar();
					// Try this for now:
	Monster_pathfinder_client cost(npc, dist, opponent);
	Tile_coord pos = npc->get_tile();
	if (!path->NewPath(pos, opponent->get_tile(), &cost))
		{			// Failed?  Try nearest opponent.
		failures++;
		bool retry_ok = false;
		if (npc->get_attack_mode() != Actor::manual)
			{
			Game_object *closest = find_foe(Actor::nearest);
			if (closest && closest != opponent)
				{
				opponent = closest;
				npc->set_target(opponent);
				Monster_pathfinder_client cost(npc, dist, 
								opponent);
				retry_ok = (opponent != 0 && path->NewPath(
				  pos, opponent->get_tile(), &cost));
				}
			}
		if (!retry_ok)
			{
			delete path;	// Really failed.  Try again in 
					//  after wandering.
					// Just try to walk towards opponent.
			Tile_coord pos = npc->get_tile();
			Tile_coord topos = opponent->get_tile();
			int dirx = topos.tx > pos.tx ? 2
				: (topos.tx < pos.tx ? -2 : (rand()%3 - 1));
			int diry = topos.ty > pos.ty ? 2
				: (topos.ty < pos.ty ? -2 : (rand()%3 - 1));
			pos.tx += dirx * (1 + rand()%4);
			pos.ty += diry * (1 + rand()%4);
			npc->walk_to_tile(pos, 2*gwin->get_std_delay(), 
							500 + rand()%500);
			failures++;
			return;
			}
		}
	failures = 0;			// Clear count.  We succeeded.
	start_battle();			// Music if first time.
	if (combat_trace) {
		cout << npc->get_name() << " is pursuing " << opponent->get_name()
			 << endl;
	}
					// First time (or 256th), visible?
	if (!yelled && gwin->add_dirty(npc))
		{
		yelled++;
		if (can_yell && rand()%2)// Half the time.
			{
					// Goblin?
			if (Game::get_game_type() == SERPENT_ISLE &&
				 (npc->get_shapenum() == 0x1de ||
				  npc->get_shapenum() == 0x2b3 ||
				  npc->get_shapenum() == 0x2d5 ||
				  npc->get_shapenum() == 0x2e8))
				npc->say(0x4c9, 0x4d1);
	    		else
				npc->say(first_to_battle, last_to_battle);
			}
		}
	int extra_delay = 0;
	if (rand()%5 == 0 && Can_summon(npc) && summon())
		extra_delay = 5000;
	else if (rand()%10 == 0 && Can_be_invisible(npc))
		(void) be_invisible();
					// Walk there, & check half-way.
	npc->set_action(new Approach_actor_action(path, opponent,
							for_projectile));
					// Start walking.  Delay a bit if
					//   opponent is off-screen.
	npc->start(gwin->get_std_delay(), extra_delay +
		(Off_screen(gwin, opponent) ? 
			5*gwin->get_std_delay() : gwin->get_std_delay()));
	}

/*
 *	Check for a useful weapon at a given ready-spot.
 */

static Game_object *Get_usable_weapon
	(
	Actor *npc,
	int index			// Ready-spot to check.
	)
	{
	Game_object *bobj = npc->get_readied(index);
	if (!bobj)
		return 0;
	Shape_info& info = bobj->get_info();
	Weapon_info *winf = info.get_weapon_info();
	if (!winf)
		return 0;		// Not a weapon.
	int ammo = winf->get_ammo_consumed();
	if (ammo)			// Check for readied ammo.
		{//+++++Needs improvement.  Ammo could be in pack.
		Game_object *aobj = npc->get_readied(Actor::ammo);
		if (!aobj || !In_ammo_family(aobj->get_shapenum(), ammo))
			return 0;
		}
	else if (winf->uses_charges() && info.has_quality())
		if (bobj->get_quality() <= 0)
			return 0;	// No charges left.
	if (info.get_ready_type() == two_handed_weapon &&
	    npc->get_readied(Actor::rhand) != 0)
		return 0;		// Needs two free hands.
	return bobj;
	}

/*
 *	Swap weapon with the one in the belt.
 *
 *	Output:	1 if successful.
 */

static int Swap_weapons
	(
	Actor *npc
	)
	{
	int index = Actor::belt;
	Game_object *bobj = Get_usable_weapon(npc, index);
	if (!bobj)
		{
		index = Actor::back2h_spot;
		bobj = Get_usable_weapon(npc, index);
		if (!bobj)
			{		// Do thorough search for NPC's.
			if (!npc->is_in_party())
				return npc->ready_best_weapon();
			else
				return 0;
			}
		}
	Game_object *oldweap = npc->get_readied(Actor::lhand);
	if (oldweap)
		npc->remove(oldweap);
	npc->remove(bobj);
	npc->add(bobj, 1);		// Should go into weapon hand.
	if (oldweap)			// Put old where new one was.
		npc->add_readied(oldweap, index, 1, 1);	
	return 1;
	}

/*
 *	Begin a strike at the opponent.
 */

void Combat_schedule::start_strike
	(
	)
	{
	Game_object *opponent = npc->get_target();
	Rectangle npctiles = npc->get_footprint(),
		  opptiles = opponent->get_footprint();
	Rectangle stiles = npctiles,	// Get copy for weapon range.
		  ptiles = npctiles;
					// Get difference in lift.
	int dz = npc->get_lift() - opponent->get_lift();
	if (dz < 0)
		dz = -dz;
					// Close enough to strike?
	if (strike_range && dz < 5 &&	// Same floor?
		stiles.enlarge(strike_range).intersects(opptiles))
		state = strike;
	else if (dz >= 5 ||		// FOR NOW, since is_straight_path()
					//   doesn't check z-coord.
		 !projectile_range ||
					// Enlarge to projectile range.
		 !ptiles.enlarge(projectile_range).intersects(opptiles))
		{
		state = approach;
		approach_foe();		// Get a path.
		return;
		}
	else				// See if we can fire spell/projectile.
		{
		Game_object *aobj;
		bool weapon_dead = false;
		if (spellbook)
			weapon_dead = !spellbook->can_do_spell(npc);
		else if (ammo_shape &&
		    (!(aobj = npc->get_readied(Actor::ammo)) ||
			!In_ammo_family(aobj->get_shapenum(), ammo_shape)))
			{
			if (!npc->ready_ammo())	// Look in pack for ammo.
				weapon_dead = true;
			}
		else if (uses_charges && weapon && weapon->get_quality() <= 0)
			weapon_dead = true;
		if (weapon_dead)
			{		// Out of ammo/reagents/charges.
			if (npc->get_schedule_type() != Schedule::duel)
				{	// Look in pack for ammo.
				if (Swap_weapons(npc))
					Combat_schedule::set_weapon();
				else
					set_hand_to_hand();
				}
			if (!npc->get_info().has_strange_movement())
				npc->change_frame(npc->get_dir_framenum(
							Actor::standing));
			state = approach;
			npc->set_target(0);
			npc->start(200, 500);
			return;
			}
#if 0	/* +++++OLD WAY.  Trolls hung around too long. */
		Tile_coord pos = npc->get_tile();
		Tile_coord opos = opponent->get_tile();
		if (opos.tx < pos.tx)	// Going left?
			pos.tx = npctiles.x;
		else			// Right?
			opos.tx = opptiles.x;
		if (opos.ty < pos.ty)	// Going north?
			pos.ty = npctiles.y;
		else			// South.
			opos.ty = opptiles.y;
		if (!no_blocking &&
		    !Fast_pathfinder_client::is_straight_path(pos, opos))
			{		// Blocked.  Find another spot.
			pos.tx += rand()%7 - 3;
			pos.ty += rand()%7 - 3;
			npc->walk_to_tile(pos, gwin->get_std_delay(), 0);
			state = approach;
			npc->set_target(0);	// And try another enemy.
			return;
			}
#else
		if (!no_blocking &&
		    !Fast_pathfinder_client::is_straight_path(npc, opponent))
			{
			approach_foe(true);
			return;
			}
#endif
		if (!started_battle)
			start_battle();	// Play music if first time.
		state = fire;		// Clear to go.
		}
	if (combat_trace) {
		cout << npc->get_name() << " attacks " << opponent->get_name() << endl;
	}
	int dir = npc->get_direction(opponent);
	signed char frames[12];		// Get frames to show.
	int cnt = npc->get_attack_frames(weapon_shape, projectile_range > 0,
							dir, frames);
	if (cnt)
		npc->set_action(new Frames_actor_action(frames, cnt, gwin->get_std_delay()));
	npc->start();			// Get back into time queue.
	int sfx;			// Play sfx.
	Game_window *gwin = Game_window::get_instance();
	Weapon_info *winf = ShapeID::get_info(weapon_shape).get_weapon_info();
	if (winf && (sfx = winf->get_sfx()) >= 0 &&
					// But only if Ava. involved.
	    (npc == gwin->get_main_actor() || 
				opponent == gwin->get_main_actor()))
		Audio::get_ptr()->play_sound_effect(sfx);
	dex_points -= dex_to_attack;
	}

/*
 *	Run away.
 */

void Combat_schedule::run_away
	(
	)
	{
	Game_window *gwin = Game_window::get_instance();
	fleed++;
					// Might be nice to run from opp...
	int rx = rand();		// Get random position away from here.
	int ry = rand();
	int dirx = 2*(rx%2) - 1;	// Get 1 or -1.
	int diry = 2*(ry%2) - 1;
	Tile_coord pos = npc->get_tile();
	pos.tx += dirx*(8 + rx%8);
	pos.ty += diry*(8 + ry%8);
	npc->walk_to_tile(pos, gwin->get_std_delay(), 0);
	if (fleed == 1 && !npc->get_flag(Obj_flags::si_tournament) &&
					rand()%3 && gwin->add_dirty(npc))
		{
		yelled++;
		if (can_yell)
			npc->say(first_flee, last_flee);
		}
	}

/*
 *	See if a spellbook is readied with a spell
 *	available.
 *
 *	Output:	->spellbook if so, else 0.
 */

Spellbook_object *Combat_schedule::readied_spellbook
	(
	)
	{
	Spellbook_object *book = 0;
					// Check both hands.
	Game_object *obj = npc->get_readied(Actor::lhand);
	if (obj && obj->get_info().get_shape_class() == Shape_info::spellbook)
		{
		book = static_cast<Spellbook_object*> (obj);
		if (book->can_do_spell(npc))
			return book;
		}
	obj = npc->get_readied(Actor::rhand);
	if (obj && obj->get_info().get_shape_class() == Shape_info::spellbook)
		{
		book = static_cast<Spellbook_object*> (obj);
		if (book->can_do_spell(npc))
			return book;
		}
	return 0;
	}

/*
 *	Set weapon 'max_range' and 'ammo'.  Ready a new weapon if needed.
 */

void Combat_schedule::set_weapon
	(
	bool removed			// The weapon was just removed.
	)
	{
	int points;
	spellbook = 0;
	Weapon_info *info = npc->get_weapon(points, weapon_shape, weapon);
	if (!info && !removed &&	// No weapon?
	    !(spellbook = readied_spellbook()) &&	// No spellbook?
					// Not dragging?
	    !gwin->is_dragging() &&
					// And not dueling?
	    npc->get_schedule_type() != Schedule::duel &&
	    state != wait_return)	// And not waiting for boomerang.
		{
		npc->ready_best_weapon();
		info = npc->get_weapon(points, weapon_shape, weapon);
		}
	if (!info)			// Still nothing.
		{
		set_hand_to_hand();
		if (spellbook)		// Did we find a spellbook?
			{
			projectile_range = 10;	// Guessing.
			no_blocking = true;
			}
		}
	else
		{
		projectile_shape = info->get_projectile();
		ammo_shape = info->get_ammo_consumed();
		uses_charges = info->uses_charges() && weapon &&
					weapon->get_info().has_quality();
		strike_range = info->get_striking_range();
		projectile_range = info->get_projectile_range();

		returns = info->returns();
		is_thrown = info->is_thrown();
		no_blocking = info->no_blocking();
		if (ammo_shape)
			{
			Ammo_info *ainfo = 
				ShapeID::get_info(ammo_shape).get_ammo_info();
			if (ainfo)
				no_blocking = 
					no_blocking || ainfo->no_blocking();
			}
		}
	max_range = projectile_range > strike_range ? projectile_range
					: strike_range;
	if (state == strike || state == fire)
		state = approach;	// Got to restart attack.
	}


/*
 *	Set for hand-to-hand combat (no weapon).
 */

void Combat_schedule::set_hand_to_hand
	(
	)
	{
	weapon = 0;
	projectile_shape = ammo_shape = 0;
	projectile_range = 0;
	strike_range = 1;	// Can always bite.
	is_thrown = returns = no_blocking = uses_charges = false;
				// Put aside weapon.
	Game_object *weapon = npc->get_readied(Actor::lhand);
	if (weapon)
		{
		int index = -1;
		if (!npc->get_readied(Actor::belt))
			index = Actor::belt;
		else if (!npc->get_readied(Actor::back2h_spot))
			index = Actor::back2h_spot;
		if (index >= 0)
			{
			npc->remove(weapon);
			npc->add_readied(weapon, index, 1, 1);	
			}
		}
	}

/*
 *	See if we need a new opponent.
 */

inline int Need_new_opponent
	(
	Game_window *gwin,
	Actor *npc
	)
	{
	Game_object *opponent = npc->get_target();
	Actor *act;
					// Nonexistent or dead?
	if (!opponent || 
	    ((act = opponent->as_actor()) != 0 && act->is_dead()) ||
					// Or invisible?
	    (opponent->get_flag(Obj_flags::invisible) && rand()%4 == 0))
		return 1;
					// See if off screen.
	return Off_screen(gwin, opponent) && !Off_screen(gwin, npc);
	}

/*
 *	Use one unit of ammo.  NOTE:  static method.
 *
 *	Output:	Actual ammo shape.
 *		0 if failed.
 */

int Combat_schedule::use_ammo
	(
	Actor *npc,
	int ammo,			// Ammo family shape.
	int proj			// Projectile shape.
	)
	{
	Game_object *aobj = npc->get_readied(Actor::ammo);
	if (!aobj)
		return 0;
	int actual_ammo = aobj->get_shapenum();
	if (!In_ammo_family(actual_ammo, ammo))
		return 0;
	npc->remove(aobj);		// Remove all.
	int quant = aobj->get_quantity();
	Shape_info& info = ShapeID::get_info(proj);
	if (GAME_BG && info.get_ready_type() == triple_crossbow_bolts)
		aobj->modify_quantity(-3);	// Triple crossbows use 3x the ammo.
	else
		aobj->modify_quantity(-1);	// Reduce amount.
	if (quant > 1)			// Still some left?  Put back.
		npc->add_readied(aobj, Actor::ammo);
					// Use actual shape unless a different
					//   projectile was specified.
	return ammo != actual_ammo ? actual_ammo : proj;
	}


/*
 *	Create.
 */

Combat_schedule::Combat_schedule
	(
	Actor *n, 
	Schedule_types 
	prev_sched
	) : Schedule(n), state(initial), prev_schedule(prev_sched),
		weapon(0), weapon_shape(0),
		ammo_shape(0), projectile_shape(0), spellbook(0),
		strike_range(0), projectile_range(0), max_range(0),
		practice_target(0), is_thrown(false), yelled(0),
		no_blocking(false), uses_charges(false),
		started_battle(false), fleed(0), failures(0), teleport_time(0),
		summon_time(0),
		dex_points(0), alignment(n->get_effective_alignment())
	{
	Combat_schedule::set_weapon();
					// Cache some data.
	Game_window *gwin = Game_window::get_instance();
	Monster_info *minf = npc->get_info().get_monster_info();
	can_yell = !minf || !minf->cant_yell();
	unsigned int curtime = SDL_GetTicks();
	summon_time = curtime + 4000;
	invisible_time = curtime + 4500;
	}


/*
 *	Previous action is finished.
 */

void Combat_schedule::now_what
	(
	)
	{
	Game_window *gwin = Game_window::get_instance();
	if (state == initial)		// Do NOTHING in initial state so
		{			//   usecode can, e.g., set opponent.
					// Way far away (50 tiles)?
		if (npc->distance(gwin->get_camera_actor()) > 50)
			{
			npc->set_dormant();
			return;		// Just go dormant.
			}
		state = approach;
		npc->start(200, 200);
		return;
		}
	if (npc->get_flag(Obj_flags::asleep))
		{
		npc->start(200, 1000);	// Check again in a second.
		return;
		}
					// Running away?
	if (npc->get_attack_mode() == Actor::flee)
		{			// If not in combat, stop running.
		if (fleed > 2 && !gwin->in_combat() && 
						npc->get_party_id() >= 0)
					// WARNING:  Destroys ourself.
			npc->set_schedule_type(Schedule::follow_avatar);
		else
			run_away();
		return;
		}
					// Check if opponent still breathes.
	if (Need_new_opponent(gwin, npc))
		{
		npc->set_target(0);
		state = approach;
		}
	Game_object *opponent = npc->get_target();
					// Flag for slimes:
	bool strange = npc->get_info().has_strange_movement() != false;
	switch (state)			// Note:  state's action has finished.
		{
	case approach:
		if (!opponent)
			approach_foe();
		else if (dex_points >= dex_to_attack)
			start_strike();
		else
			{
#if 0
			cout << npc->get_name() << " only has " <<
				dex_points << " dex. points" << endl;
#endif
			dex_points += npc->get_property(Actor::dexterity);
			npc->start(gwin->get_std_delay(),
						gwin->get_std_delay());
			}
		break;
	case strike:			// He hasn't moved away?
		state = approach;
					// Back into queue.
		npc->start(gwin->get_std_delay(), gwin->get_std_delay());
		if (npc->get_footprint().enlarge(strike_range).intersects(
					opponent->get_footprint()))
			{
			int dir = npc->get_direction(opponent);
			if (!strange)	// Avoid messing up slimes.
				npc->change_frame(npc->get_dir_framenum(dir,
							Actor::ready_frame));
					// Glass sword?  Only 1 use.
			if (weapon_shape == 604)
				{
				npc->remove_quantity(1, weapon_shape,
						c_any_qual, c_any_framenum);
				Combat_schedule::set_weapon();
				}
					// This may delete us!
			Actor *safenpc = npc;
			safenpc->set_target(opponent->attacked(npc));
					// Strike but once at objects.
			Game_object *newtarg = safenpc->get_target();
			if (newtarg && !newtarg->as_actor())
				safenpc->set_target(0);
			return;		// We may no longer exist!
			}
		break;
	case fire:			// Range weapon.
		{
		failures = 0;
		state = approach;
					// Save shape (it might change).
		int ashape = ammo_shape, wshape = weapon_shape,
		    pshape = projectile_shape;
		int delay = spellbook ? 6*gwin->get_std_delay() 
				: gwin->get_std_delay();
		if (is_thrown)		// Throwing the weapon?
			{
			if (returns)	// Boomerang?
				{
				ashape = wshape;
				delay = (1 + npc->distance(opponent))*
							gwin->get_std_delay();
				state = wait_return;
				}
			if (npc->remove_quantity(1, wshape,
					c_any_qual, c_any_framenum) == 0)
				{
				npc->add_dirty();
				ashape = wshape;
				Combat_schedule::set_weapon();
				}
			}
		else if (spellbook)
			{		// Cast the spell.
			ashape = 0;	// Just to be on the safe side...
			if (!spellbook->do_spell(npc, true))
				Combat_schedule::set_weapon();
			}
		else if (uses_charges)
			{
			weapon->set_quality(weapon->get_quality() - 1);
			ashape = pshape ? pshape : wshape;
			}
		else
			ashape = ashape ? use_ammo(npc, ashape, pshape)
				: (pshape ? pshape : wshape);
		if (ashape > 0)
			gwin->get_effects()->add_effect(
				new Projectile_effect(npc, opponent,
							ashape, wshape));
		npc->start(gwin->get_std_delay(), delay);
		break;
		}
	case wait_return:		// Boomerang should have returned.
		state = approach;
		dex_points += npc->get_property(Actor::dexterity);
		npc->start(gwin->get_std_delay(), gwin->get_std_delay());
		break;
	default:
		break;
		}
	if (failures > 5 && npc != gwin->get_camera_actor())
	{			// Too many failures.  Give up for now.
		if (combat_trace) {
			cout << npc->get_name() << " is giving up" << endl;
		}
		if (npc->get_party_id() >= 0)
			{		// Party member.
			npc->walk_to_tile(
				gwin->get_main_actor()->get_tile(),
						gwin->get_std_delay());
					// WARNING:  Destroys ourself.
			npc->set_schedule_type(Schedule::follow_avatar);
			}
		else if (!gwin->get_win_rect().intersects(
						gwin->get_shape_rect(npc)))
			{		// Off screen?  Stop trying.
			gwin->get_tqueue()->remove(npc);
			npc->set_dormant();
			}
		else if (npc->get_alignment() == Npc_actor::friendly &&
				prev_schedule != Schedule::combat)
			{		// Return to normal schedule.
			npc->update_schedule(gclock->get_hour()/3, 7);
			if (npc->get_schedule_type() == Schedule::combat)
				npc->set_schedule_type(prev_schedule);
			}
		else
			{		// Wander randomly.
			Tile_coord t = npc->get_tile();
			int dist = 2+rand()%3;
			int newx = t.tx - dist + rand()%(2*dist);
			int newy = t.ty - dist + rand()%(2*dist);
					// Wait a bit.
			npc->walk_to_tile(newx, newy, t.tz, 
					2*gwin->get_std_delay(), rand()%1000);
			}
		}
	}

/*
 *	Npc just went dormant (probably off-screen).
 */

void Combat_schedule::im_dormant
	(
	)
	{
	if (npc->get_effective_alignment() == Npc_actor::friendly && 
		prev_schedule != npc->get_schedule_type() && npc->is_monster())
					// Friendly, so end combat.
		npc->set_schedule_type(prev_schedule);
	}

/*
 *	Leaving combat.
 */

void Combat_schedule::ending
	(
	int /* newtype */
	)
	{
	Game_window *gwin = Game_window::get_instance();
	if (gwin->get_main_actor() == npc && 
					// Not if called from usecode.
	    !gwin->get_usecode()->in_usecode())
		{			// See if being a coward.
		find_opponents();
		bool found = false;	// Find a close-by enemy.
		Tile_coord pos = npc->get_tile();
		for (list<Actor*>::const_iterator it = opponents.begin(); 
						it != opponents.end(); ++it)
			{
			Actor *opp = *it;
			Tile_coord opppos = opp->get_tile();
			if (opppos.distance(pos) < (300/2)/c_tilesize &&
			    Fast_pathfinder_client::is_grabable(pos, opppos))
				{
				found = true;
				break;
				}
			}
		if (found)
			Audio::get_ptr()->start_music_combat(CSRun_Away,
								false);
		}
	}


/*
 *	Create duel schedule.
 */

Duel_schedule::Duel_schedule
	(
	Actor *n
	) : Combat_schedule(n, duel), start(n->get_tile()),
		attacks(0)
	{
	started_battle = true;		// Avoid playing music.
	}

/*
 *	Ready a bow-and-arrows.
 */

void Ready_duel_weapon
	(
	Actor *npc,
	int wshape,			// Weapon shape.
	int ashape			// Ammo shape, or -1.
	)
	{
	Game_map *gmap = Game_window::get_instance()->get_map();
	Game_object *weap = npc->get_readied(Actor::lhand);
	if (!weap || weap->get_shapenum() != wshape)
		{			// Need a bow.
		Game_object *newweap = 
			npc->find_item(wshape, c_any_qual, c_any_framenum);
		if (newweap)		// Have it?
			newweap->remove_this(1);
		else			// Create new one.
			newweap = gmap->create_ireg_object(wshape, 0);
		if (weap)		// Remove old item.
			weap->remove_this(1);
		npc->add(newweap, 1);	// Should go in correct spot.
		if (weap)
			npc->add(weap, 1);
		}
	if (ashape == -1)		// No ammo needed.
		return;
					// Now provide 1-3 arrows.
	Game_object *aobj = npc->get_readied(Actor::ammo);
	if (aobj)
		aobj->remove_this();	// Toss current ammo.
	Game_object *arrows = gmap->create_ireg_object(ashape, 0);
	int extra = rand()%3;		// Add 1 or 2.
	if (extra)
		arrows->modify_quantity(extra);
	npc->add(arrows, 1);		// Should go to right spot.
	}

/*
 *	Find dueling opponents.
 */

void Duel_schedule::find_opponents
	(
	)
	{
	opponents.clear();
	attacks = 0;
	practice_target = 0;
	int r = rand()%3;
	if (r == 0)			// First look for practice targets.
		{			// Archery target:
		practice_target = npc->find_closest(735);
		if (practice_target)	// Need bow-and-arrows.
			Ready_duel_weapon(npc, 597, 722);
		}
	if (!practice_target)		// Fencing dummy or dueling opponent.
		{
		Ready_duel_weapon(npc, 602, -1);
		if (r == 1)
			practice_target = npc->find_closest(860);
		}
	Combat_schedule::set_weapon();
	if (practice_target)
		{
		npc->set_target(practice_target);
		return;			// Just use that.
		}
	Actor_vector vec;		// Find all nearby NPC's.
	npc->find_nearby_actors(vec, c_any_shapenum, 24);
	for (Actor_vector::const_iterator it = vec.begin(); it != vec.end();
									 ++it)
		{
		Actor *opp = *it;
		Game_object *oppopp = opp->get_target();
		if (opp != npc && opp->get_schedule_type() == duel &&
		    (!oppopp || oppopp == npc))
			if (rand()%2)
				opponents.push_back(opp);
			else
				opponents.push_back(opp);
		}
	}

/*
 *	Previous action is finished.
 */

void Duel_schedule::now_what
	(
	)
	{
	if (state == strike || state == fire)
		{
		attacks++;
					// Practice target full?
		if (practice_target && practice_target->get_shapenum() == 735
			&& practice_target->get_framenum() > 0 &&
				practice_target->get_framenum()%3 == 0)
			{
			attacks = 0;	// Break off.
					//++++++Should walk there.
			practice_target->change_frame(0);
			}
		}
	else
		{
		Combat_schedule::now_what();
		return;
		}
	if (attacks%8 == 0)		// Time to break off.
		{
		npc->set_target(0);
		Tile_coord pos = start;
		pos.tx += rand()%24 - 12;
		pos.ty += rand()%24 - 12;
					// Find a free spot.
		Tile_coord dest = Map_chunk::find_spot(pos, 3, npc, 1);
		if (dest.tx == -1 || !npc->walk_path_to_tile(dest, 
					gwin->get_std_delay(), rand()%2000))
					// Failed?  Try again a little later.
			npc->start(250, rand()%3000);
		}
	else
		Combat_schedule::now_what();
	}

/*
 *	Pause/unpause while in combat.
 */

void Combat::toggle_pause
	(
	)
	{
	if (!paused && mode == original)
		return;			// Not doing that sort of thing.
	if (paused)
		{
		resume();		// Text is probably for debugging.
		eman->center_text("Combat resumed");
		}
	else
		{
		gwin->get_tqueue()->pause(SDL_GetTicks());
		eman->center_text("Combat paused");
		paused = true;
		}
	}

/*
 *	Resume.
 */

void Combat::resume
	(
	)
	{
	if (!paused)
		return;
	gwin->get_tqueue()->resume(SDL_GetTicks());
	paused = false;
	}
