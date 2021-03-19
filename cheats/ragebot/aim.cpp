// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

#include "aim.h"
#include "..\misc\misc.h"
#include "..\misc\logs.h"
#include "..\autowall\autowall.h"
#include "..\misc\prediction_system.h"
#include "..\fakewalk\slowwalk.h"
#include "..\lagcompensation\local_animations.h"

void aim::run(CUserCmd* cmd)
{
	backup.clear();
	targets.clear();
	scanned_targets.clear();
	final_target.reset();
	should_stop = false;

	if (!g_cfg.ragebot.enable)
		return;

	automatic_revolver(cmd);
	prepare_targets();

	if (g_ctx.globals.weapon->is_non_aim())
		return;

	if (g_ctx.globals.current_weapon == -1)
		return;

	scan_targets();

	if (!should_stop && g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop_modifiers[AUTOSTOP_PREDICTIVE])
	{
		for (auto& target : targets)
		{
			if (!target.last_record->valid())
				continue;

			scan_data last_data;

			target.last_record->adjust_player();
			scan(target.last_record, last_data, g_ctx.globals.eye_pos);

			if (!last_data.valid())
				continue;

			should_stop = true;
			break;
		}
	}

	if (!automatic_stop(cmd))
		return;

	if (scanned_targets.empty())
		return;

	find_best_target();

	if (!final_target.data.valid())
		return;

	fire(cmd);
}

void aim::automatic_revolver(CUserCmd* cmd)
{
	if (!m_engine()->IsActiveApp())
		return;

	if (g_ctx.globals.weapon->m_iItemDefinitionIndex() != WEAPON_REVOLVER)
		return;

	if (cmd->m_buttons & IN_ATTACK)
		return;

	cmd->m_buttons &= ~IN_ATTACK2;

	static auto r8cock_time = 0.0f;
	auto server_time = TICKS_TO_TIME(g_ctx.globals.backup_tickbase);

	if (g_ctx.globals.weapon->can_fire(false))
	{
		if (r8cock_time <= server_time)
		{
			if (g_ctx.globals.weapon->m_flNextSecondaryAttack() <= server_time)
				r8cock_time = server_time + TICKS_TO_TIME(13);
			else
				cmd->m_buttons |= IN_ATTACK2;
		}
		else
			cmd->m_buttons |= IN_ATTACK;
	}
	else
	{
		r8cock_time = server_time + TICKS_TO_TIME(13);
		cmd->m_buttons &= ~IN_ATTACK;
	}

	g_ctx.globals.revolver_working = true;
}

void aim::prepare_targets()
{
	for (auto i = 1; i < m_globals()->m_maxclients; i++)
	{
		if (g_cfg.player_list.white_list[i])
			continue;

		auto e = (player_t*)m_entitylist()->GetClientEntity(i);

		if (!e->valid(true, false))
			continue;

		auto records = &player_records[i];

		if (records->empty())
			continue;

		targets.emplace_back(target(e, get_record(records, false), get_record(records, true)));
	}

	for (auto& target : targets)
		backup.emplace_back(adjust_data(target.e));
}

static bool compare_records(const optimized_adjust_data& first, const optimized_adjust_data& second)
{
	if (first.shot != second.shot)
		return first.shot;
	else if (first.speed != second.speed)
		return first.speed > second.speed;

	return first.simulation_time < second.simulation_time;
}

adjust_data* aim::get_record(std::deque <adjust_data>* records, bool history)
{
	if (history)
	{
		std::deque <optimized_adjust_data> optimized_records;

		for (auto i = 0; i < records->size(); ++i)
		{
			auto record = &records->at(i);
			optimized_adjust_data optimized_record;

			optimized_record.i = i;
			optimized_record.player = record->player;
			optimized_record.simulation_time = record->simulation_time;
			optimized_record.speed = record->velocity.Length();
			optimized_record.shot = record->shot;

			optimized_records.emplace_back(optimized_record);
		}

		if (optimized_records.size() < 2)
			return nullptr;

		std::sort(optimized_records.begin(), optimized_records.end(), compare_records);

		for (auto& optimized_record : optimized_records)
		{
			auto record = &records->at(optimized_record.i);

			if (!record->valid())
				continue;

			return record;
		}
	}
	else
	{
		for (auto i = 0; i < records->size(); ++i)
		{
			auto record = &records->at(i);

			if (!record->valid())
				continue;

			return record;
		}
	}

	return nullptr;
}

int aim::get_minimum_damage(bool visible, int health)
{
	if (key_binds::get().get_key_bind_state(4 + g_ctx.globals.current_weapon))
	{
		if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_override_damage > 100)
			return health + g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_override_damage - 100;
		else
			return math::clamp(g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_override_damage, 1, health);
	}
	else
	{
		if (visible)
		{
			if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_visible_damage > 100)
				return health + g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_visible_damage - 100;
			else
				return math::clamp(g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_visible_damage, 1, health);
		}
		else
		{
			if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_damage > 100)
				return health + g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_damage - 100;
			else
				return math::clamp(g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].minimum_damage, 1, health);
		}
	}
}

void aim::scan_targets()
{
	if (targets.empty())
		return;

	for (auto& target : targets)
	{
		if (target.history_record->valid())
		{
			scan_data last_data;

			if (target.last_record->valid())
			{
				target.last_record->adjust_player();
				scan(target.last_record, last_data);
			}

			scan_data history_data;

			target.history_record->adjust_player();
			scan(target.history_record, history_data);

			if (last_data.valid() && last_data.damage > history_data.damage)
				scanned_targets.emplace_back(scanned_target(target.last_record, last_data));
			else if (history_data.valid())
				scanned_targets.emplace_back(scanned_target(target.history_record, history_data));
		}
		else
		{
			if (!target.last_record->valid())
				continue;

			scan_data last_data;

			target.last_record->adjust_player();
			scan(target.last_record, last_data);

			if (!last_data.valid())
				continue;

			scanned_targets.emplace_back(scanned_target(target.last_record, last_data));
		}
	}
}

bool aim::automatic_stop(CUserCmd* cmd)
{
	if (!should_stop)
		return true;

	if (!g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop)
		return true;

	if (g_ctx.globals.slowwalking)
		return true;

	if (!(g_ctx.local()->m_fFlags() & FL_ONGROUND && engineprediction::get().backup_data.flags & FL_ONGROUND))
		return true;

	if (g_ctx.globals.weapon->is_empty())
		return true;

	if (!g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop_modifiers[AUTOSTOP_BETWEEN_SHOTS] && !g_ctx.globals.weapon->can_fire(false))
		return true;

	auto animlayer = g_ctx.local()->get_animlayers()[1];

	if (animlayer.m_nSequence)
	{
		auto activity = g_ctx.local()->sequence_activity(animlayer.m_nSequence);

		if (activity == ACT_CSGO_RELOAD && animlayer.m_flWeight > 0.0f)
			return true;
	}

	auto weapon_info = g_ctx.globals.weapon->get_csweapon_info();

	if (!weapon_info)
		return true;

	auto max_speed = 0.33f * (g_ctx.globals.scoped ? weapon_info->flMaxPlayerSpeedAlt : weapon_info->flMaxPlayerSpeed);

	if (engineprediction::get().backup_data.velocity.Length2D() < max_speed)
		slowwalk::get().create_move(cmd);
	else
	{
		Vector direction;
		Vector real_view;

		math::vector_angles(engineprediction::get().backup_data.velocity, direction);
		m_engine()->GetViewAngles(real_view);

		direction.y = real_view.y - direction.y;

		Vector forward;
		math::angle_vectors(direction, forward);

		static auto cl_forwardspeed = m_cvar()->FindVar(crypt_str("cl_forwardspeed"));
		static auto cl_sidespeed = m_cvar()->FindVar(crypt_str("cl_sidespeed"));

		auto negative_forward_speed = -cl_forwardspeed->GetFloat();
		auto negative_side_speed = -cl_sidespeed->GetFloat();

		auto negative_forward_direction = forward * negative_forward_speed;
		auto negative_side_direction = forward * negative_side_speed;

		cmd->m_forwardmove = negative_forward_direction.x;
		cmd->m_sidemove = negative_side_direction.y;

		if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop_modifiers[AUTOSTOP_FORCE_ACCURACY])
			return false;
	}

	return true;
}

void aim::scan(adjust_data* record, scan_data& data, const Vector& shoot_position)
{
	if (!g_ctx.globals.weapon)
		return;

	auto weapon_info = g_ctx.globals.weapon->get_csweapon_info();

	if (!weapon_info)
		return;

	auto hitboxes = get_hitboxes(record);

	if (hitboxes.empty())
		return;

	auto force_safe_points = key_binds::get().get_key_bind_state(3) || g_cfg.player_list.force_safe_points[record->i] || g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].max_misses && g_ctx.globals.missed_shots[record->i] >= g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].max_misses_amount;
	auto best_damage = 0;

	auto minimum_damage = get_minimum_damage(false, record->player->m_iHealth());
	auto minimum_visible_damage = get_minimum_damage(true, record->player->m_iHealth());

	auto get_hitgroup = [](const int& hitbox)
	{
		if (hitbox == HITBOX_HEAD)
			return 0;
		else if (hitbox == HITBOX_PELVIS)
			return 1;
		else if (hitbox == HITBOX_STOMACH)
			return 2;
		else if (hitbox >= HITBOX_LOWER_CHEST && hitbox <= HITBOX_UPPER_CHEST)
			return 3;
		else if (hitbox >= HITBOX_RIGHT_THIGH && hitbox <= HITBOX_LEFT_FOOT)
			return 4;
		else if (hitbox >= HITBOX_RIGHT_HAND && hitbox <= HITBOX_LEFT_FOREARM)
			return 5;

		return -1;
	};

	std::vector <scan_point> points;

	for (auto& hitbox : hitboxes)
	{
		auto current_points = get_points(record, hitbox);

		for (auto& point : current_points)
		{
			point.safe = record->bot || (hitbox_intersection(record->player, record->matrixes_data.zero, hitbox, shoot_position, point.point) && hitbox_intersection(record->player, record->matrixes_data.first, hitbox, shoot_position, point.point) && hitbox_intersection(record->player, record->matrixes_data.second, hitbox, shoot_position, point.point));

			if (!force_safe_points || point.safe)
				points.emplace_back(point);
		}
	}

	if (points.empty())
		return;

	auto body_hitboxes = true;

	for (auto& point : points)
	{
		if (body_hitboxes && (point.hitbox < HITBOX_PELVIS || point.hitbox > HITBOX_UPPER_CHEST))
		{
			body_hitboxes = false;

			if (g_cfg.player_list.force_body_aim[record->i])
				break;

			if (key_binds::get().get_key_bind_state(22))
				break;

			if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].prefer_body_aim && best_damage >= 1 && record->flags & FL_ONGROUND && !record->shot)
				break;
		}

		auto fire_data = autowall::get().wall_penetration(shoot_position, point.point, record->player);

		if (!fire_data.valid)
			continue;

		if (fire_data.damage < 1)
			continue;

		if (!fire_data.visible && !g_cfg.ragebot.autowall)
			continue;

		if (get_hitgroup(fire_data.hitbox) != get_hitgroup(point.hitbox))
			continue;

		auto current_minimum_damage = fire_data.visible ? minimum_visible_damage : minimum_damage;

		if (fire_data.damage >= current_minimum_damage && fire_data.damage >= best_damage)
		{
			if (!should_stop)
			{
				should_stop = true;

				if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop_modifiers[AUTOSTOP_LETHAL] && fire_data.damage < record->player->m_iHealth())
					should_stop = false;
				else if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop_modifiers[AUTOSTOP_VISIBLE] && !fire_data.visible)
					should_stop = false;
				else if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].autostop_modifiers[AUTOSTOP_CENTER] && !point.center)
					should_stop = false;
			}

			if (force_safe_points && !point.safe)
				continue;

			// note: simv0l - check can we kill enemy by one shot in more safe hitboxes.
			if (((record->flags & FL_ONGROUND && g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].prefer_safe_points && point.safe) || (record->flags & FL_ONGROUND && point.hitbox >= HITBOX_PELVIS && point.hitbox <= HITBOX_UPPER_CHEST) || ((!(record->flags & FL_ONGROUND) || record->shot) && point.hitbox == HITBOX_HEAD)) && fire_data.damage >= record->player->m_iHealth())
			{
				best_damage = fire_data.damage;

				data.point = point;
				data.visible = fire_data.visible;
				data.damage = fire_data.damage;
				data.hitbox = fire_data.hitbox;
				return;
			}
			else
			{
				best_damage = fire_data.damage;

				data.point = point;
				data.visible = fire_data.visible;
				data.damage = fire_data.damage;
				data.hitbox = fire_data.hitbox;
			}
		}
	}
}

std::vector <int> aim::get_hitboxes(adjust_data* record)
{
	std::vector <int> hitboxes;

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(4))
		hitboxes.emplace_back(HITBOX_STOMACH);

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(5))
		hitboxes.emplace_back(HITBOX_PELVIS);

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(2))
		hitboxes.emplace_back(HITBOX_CHEST);

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(1))
		hitboxes.emplace_back(HITBOX_UPPER_CHEST);

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(3))
		hitboxes.emplace_back(HITBOX_LOWER_CHEST);

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(0))
		hitboxes.emplace_back(HITBOX_HEAD);

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(6))
	{
		hitboxes.emplace_back(HITBOX_RIGHT_UPPER_ARM);
		hitboxes.emplace_back(HITBOX_LEFT_UPPER_ARM);
	}

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(7))
	{
		hitboxes.emplace_back(HITBOX_RIGHT_THIGH);
		hitboxes.emplace_back(HITBOX_LEFT_THIGH);

		hitboxes.emplace_back(HITBOX_RIGHT_CALF);
		hitboxes.emplace_back(HITBOX_LEFT_CALF);
	}

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitboxes.at(8))
	{
		hitboxes.emplace_back(HITBOX_RIGHT_FOOT);
		hitboxes.emplace_back(HITBOX_LEFT_FOOT);
	}

	return hitboxes;
}

std::vector <scan_point> aim::get_points(adjust_data* record, int hitbox, bool from_aim)
{
	std::vector <scan_point> points;
	auto model = record->player->GetModel();

	if (!model)
		return points;

	auto hdr = m_modelinfo()->GetStudioModel(model);

	if (!hdr)
		return points;

	auto set = hdr->pHitboxSet(record->player->m_nHitboxSet());

	if (!set)
		return points;

	auto bbox = set->pHitbox(hitbox);

	if (!bbox)
		return points;

	const auto is_zeus = g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_TASER;
	const auto is_knife = g_ctx.globals.weapon->is_knife();

	const auto mod = bbox->radius != -1.0f ? bbox->radius : 0.0f;

	Vector max;
	Vector min;
	math::vector_transform(bbox->bbmax + mod, record->matrixes_data.main[bbox->bone], max);
	math::vector_transform(bbox->bbmin - mod, record->matrixes_data.main[bbox->bone], min);

	const auto center = (min + max) * 0.5f;

	points.emplace_back(scan_point(center, hitbox, true));

	if (!bbox->radius || is_zeus && !is_knife)
		return points;

	if (hitbox == HITBOX_NECK || hitbox == HITBOX_RIGHT_THIGH || hitbox == HITBOX_LEFT_THIGH || hitbox == HITBOX_RIGHT_CALF || hitbox == HITBOX_LEFT_CALF || hitbox == HITBOX_RIGHT_FOOT || hitbox == HITBOX_LEFT_FOOT || hitbox == HITBOX_RIGHT_HAND || hitbox == HITBOX_LEFT_HAND || hitbox == HITBOX_RIGHT_UPPER_ARM || hitbox == HITBOX_LEFT_UPPER_ARM || hitbox == HITBOX_RIGHT_FOREARM || hitbox == HITBOX_LEFT_FOREARM)
		return points;

	const auto cur_angles = math::calculate_angle(center, g_ctx.globals.eye_pos);

	Vector forward;
	math::angle_vectors(cur_angles, forward);

	auto rs = 0.0f;

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].static_point_scale)
	{
		rs = bbox->radius * g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].body_scale;
		if (hitbox == HITBOX_HEAD)
			rs = bbox->radius * g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].head_scale;
	}
	else
	{
		auto transformed_center = center;
		math::vector_transform(transformed_center, record->matrixes_data.main[bbox->bone], transformed_center);

		auto spread = g_ctx.globals.spread + g_ctx.globals.inaccuracy;
		auto distance = transformed_center.DistTo(g_ctx.globals.eye_pos);

		distance /= math::fast_sin(DEG2RAD(90.0f - RAD2DEG(spread)));
		spread = math::fast_sin(spread);

		auto radius = max(bbox->radius - distance * spread, 0.0f);
		rs = bbox->radius * math::clamp(radius / bbox->radius, 0.0f, 1.0f);
	}

	if (rs < 0.2f)
		return points;

	if (is_knife)
	{
		const auto back = forward * bbox->radius;
		points.emplace_back(scan_point(center + back, hitbox, false));
		return points;
	}

	const auto top = Vector(0, 0, 1) * rs;
	const auto right = forward.Cross(Vector(0, 0, 1)) * rs;
	const auto left = Vector(-right.x, -right.y, right.z);

	if (hitbox == HITBOX_HEAD)
		points.emplace_back(scan_point(center + top, hitbox, false));
	points.emplace_back(scan_point(center + right, hitbox, false));
	points.emplace_back(scan_point(center + left, hitbox, false));

	return points;
}

static bool compare_targets(const scanned_target& first, const scanned_target& second)
{
	if (g_cfg.player_list.high_priority[first.record->i] != g_cfg.player_list.high_priority[second.record->i])
		return g_cfg.player_list.high_priority[first.record->i];

	switch (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].selection_type)
	{
	case 1:
		return first.fov < second.fov;
	case 2:
		return first.distance < second.distance;
	case 3:
		return first.health < second.health;
	case 4:
		return first.data.damage > second.data.damage;
	}

	return false;
}

void aim::find_best_target()
{
	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].selection_type)
		std::sort(scanned_targets.begin(), scanned_targets.end(), compare_targets);

	for (auto& target : scanned_targets)
	{
		if (target.fov > (float)g_cfg.ragebot.field_of_view)
			continue;

		final_target = target;
		final_target.record->adjust_player();
		break;
	}
}

void aim::fire(CUserCmd* cmd)
{
	if (!g_ctx.globals.weapon->can_fire(true))
		return;

	auto aim_angle = math::calculate_angle(g_ctx.globals.eye_pos, final_target.data.point.point).Clamp();

	if (!g_cfg.ragebot.silent_aim)
		m_engine()->SetViewAngles(aim_angle);

	if (!g_cfg.ragebot.autoshoot && !(cmd->m_buttons & IN_ATTACK))
		return;

	auto final_hitchance = 0;

	if (g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitchance)
	{
		auto is_valid_hitchance = calculate_hitchance(final_hitchance);

		if (!is_valid_hitchance)
		{
			auto is_zoomable_weapon = g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_SCAR20 || g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_G3SG1 || g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_SSG08 || g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_AWP || g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_AUG || g_ctx.globals.weapon->m_iItemDefinitionIndex() == WEAPON_SG553;

			if (g_cfg.ragebot.autoscope && is_zoomable_weapon && !g_ctx.globals.weapon->m_zoomLevel())
				cmd->m_buttons |= IN_ATTACK2;

			return;
		}
	}

	auto backtrack_ticks = 0;
	auto net_channel_info = m_engine()->GetNetChannelInfo();

	if (net_channel_info)
	{
		auto original_tickbase = g_ctx.globals.backup_tickbase;
		auto max_tickbase_shift = m_gamerules()->m_bIsValveDS() ? 6 : 16;

		/*if (g_cfg.ragebot.double_tap && g_cfg.ragebot.double_tap_key.key > KEY_NONE && g_cfg.ragebot.double_tap_key.key < KEY_MAX && misc::get().double_tap_key)
		{
			if (!g_ctx.local()->m_bGunGameImmunity() && !(g_ctx.local()->m_fFlags() & FL_FROZEN) && !antiaim::get().freeze_check && misc::get().double_tap_enabled && !g_ctx.globals.weapon->is_grenade() && g_ctx.globals.weapon->m_iItemDefinitionIndex() != WEAPON_TASER && g_ctx.globals.weapon->m_iItemDefinitionIndex() != WEAPON_REVOLVER)
			{
				original_tickbase += min(g_cfg.ragebot.double_tap_shift_value, max_tickbase_shift);
			}
		}

		if (g_cfg.antiaim.hide_shots && g_cfg.antiaim.hide_shots_key.key > KEY_NONE && g_cfg.antiaim.hide_shots_key.key < KEY_MAX && misc::get().hide_shots_key)
		{
			if (!g_ctx.local()->m_bGunGameImmunity() && !(g_ctx.local()->m_fFlags() & FL_FROZEN) && !antiaim::get().freeze_check && misc::get().hide_shots_enabled)
			{
				original_tickbase += min(g_cfg.antiaim.hide_shots_shift_value, max_tickbase_shift);
			}
		}*/

		static auto sv_maxunlag = m_cvar()->FindVar(crypt_str("sv_maxunlag"));

		auto correct = math::clamp(net_channel_info->GetLatency(FLOW_OUTGOING) + net_channel_info->GetLatency(FLOW_INCOMING) + util::get_interpolation(), 0.0f, sv_maxunlag->GetFloat());
		auto delta_time = correct - (TICKS_TO_TIME(original_tickbase) - final_target.record->simulation_time);

		backtrack_ticks = TIME_TO_TICKS(fabs(delta_time));
	}

	static auto get_hitbox_name = [](int hitbox, bool shot_info = false) -> std::string
	{
		switch (hitbox)
		{
		case HITBOX_HEAD:
			return shot_info ? crypt_str("Head") : crypt_str("head");
		case HITBOX_LOWER_CHEST:
			return shot_info ? crypt_str("Lower chest") : crypt_str("lower chest");
		case HITBOX_CHEST:
			return shot_info ? crypt_str("Chest") : crypt_str("chest");
		case HITBOX_UPPER_CHEST:
			return shot_info ? crypt_str("Upper chest") : crypt_str("upper chest");
		case HITBOX_STOMACH:
			return shot_info ? crypt_str("Stomach") : crypt_str("stomach");
		case HITBOX_PELVIS:
			return shot_info ? crypt_str("Pelvis") : crypt_str("pelvis");
		case HITBOX_RIGHT_UPPER_ARM:
		case HITBOX_RIGHT_FOREARM:
		case HITBOX_RIGHT_HAND:
			return shot_info ? crypt_str("Left arm") : crypt_str("left arm");
		case HITBOX_LEFT_UPPER_ARM:
		case HITBOX_LEFT_FOREARM:
		case HITBOX_LEFT_HAND:
			return shot_info ? crypt_str("Right arm") : crypt_str("right arm");
		case HITBOX_RIGHT_THIGH:
		case HITBOX_RIGHT_CALF:
			return shot_info ? crypt_str("Left leg") : crypt_str("left leg");
		case HITBOX_LEFT_THIGH:
		case HITBOX_LEFT_CALF:
			return shot_info ? crypt_str("Right leg") : crypt_str("right leg");
		case HITBOX_RIGHT_FOOT:
			return shot_info ? crypt_str("Left foot") : crypt_str("left foot");
		case HITBOX_LEFT_FOOT:
			return shot_info ? crypt_str("Right foot") : crypt_str("right foot");
		}
	};

	static auto get_resolver_type = [](resolver_type type) -> std::string
	{
		switch (type)
		{
		case ORIGINAL:
			return crypt_str("original ");
		case BRUTEFORCE:
			return crypt_str("bruteforce ");
		case LBY:
			return crypt_str("lby ");
		case LAYERS:
			return crypt_str("layers ");
		case TRACE:
			return crypt_str("trace ");
		case DIRECTIONAL:
			return crypt_str("directional ");
		}
	};

	player_info_t player_info;
	m_engine()->GetPlayerInfo(final_target.record->i, &player_info);

	//// note: simv0l - fuck this shot logs.
	//std::stringstream log;

	//log << crypt_str("Fired shot at ") + (std::string)player_info.szName + crypt_str(": ");
	//if (final_hitchance == INT_MAX)
	//{
	//	log << crypt_str("hitchance: max, ");
	//}
	//else
	//{
	//	log << crypt_str("hitchance: ") + std::to_string(final_hitchance) + crypt_str(", ");
	//}
	//log << crypt_str("hitbox: ") + get_hitbox_name(final_target.data.hitbox) + crypt_str(", ");
	//log << crypt_str("damage: ") + std::to_string(final_target.data.damage) + crypt_str(", ");
	//if (final_target.data.point.safe)
	//{
	//	log << crypt_str("safe: true, ");
	//}
	//else
	//{
	//	log << crypt_str("safe: false, ");
	//}
	//log << crypt_str("ticks: ") + std::to_string(backtrack_ticks) + crypt_str(", ");
	//log << crypt_str("resolver type: ") + get_resolver_type(final_target.record->type) + std::to_string(final_target.record->side);

	//if (g_cfg.misc.events_to_log[EVENTLOG_FIRED])
	//	eventlogs::get().add(log.str());

	cmd->m_viewangles = aim_angle;
	cmd->m_buttons |= IN_ATTACK;
	cmd->m_tickcount = TIME_TO_TICKS(final_target.record->simulation_time + util::get_interpolation());

	last_target_index = final_target.record->i;
	last_shoot_position = g_ctx.globals.eye_pos;
	last_target[last_target_index] = Last_target
	{
		*final_target.record, final_target.data, final_target.distance
	};

	auto shot = &g_ctx.shots.emplace_back();

	shot->last_target = last_target_index;
	shot->side = final_target.record->side;
	shot->fire_tick = m_globals()->m_tickcount;
	shot->shot_info.target_name = player_info.szName;
	shot->shot_info.client_hitbox = get_hitbox_name(final_target.data.hitbox, true);
	shot->shot_info.client_damage = final_target.data.damage;
	shot->shot_info.hitchance = math::clamp(final_hitchance, 0, 100);
	shot->shot_info.backtrack_ticks = backtrack_ticks;
	shot->shot_info.aim_point = final_target.data.point.point;

	lagcompensation::get().player_sets.emplace_back(player_info.steamID64, HISTORY_UNKNOWN, false, 0, 0);

	g_ctx.globals.aimbot_working = true;
	g_ctx.globals.revolver_working = false;
	g_ctx.globals.last_aimbot_shot = m_globals()->m_tickcount;
}


static std::vector<std::tuple<float, float, float>> precomputed_seeds = {};

__forceinline void aim::build_seed_table()
{
	if (!precomputed_seeds.empty())
		return;

	for (auto i = 0; i < 255; i++) {
		math::random_seed(i + 1);

		const auto pi_seed = math::random_float(0.f, twopi);

		precomputed_seeds.emplace_back(math::random_float(0.f, 1.f),
			sin(pi_seed), cos(pi_seed));
	}
}

bool aim::calculate_hitchance(int& final_hitchance)
{
	// generate look-up-table to enhance performance.
	build_seed_table();

	const auto info = g_ctx.globals.weapon->get_csweapon_info();

	if (!info)
	{
		final_hitchance = 0;
		return true;
	}

	const auto hitchance_cfg = g_cfg.ragebot.weapon[g_ctx.globals.current_weapon].hitchance_amount;

	// performance optimization.
	if ((g_ctx.globals.eye_pos - final_target.data.point.point).Length() > info->flRange)
	{
		final_hitchance = 0;
		return true;
	}

	static auto nospread = m_cvar()->FindVar(crypt_str("weapon_accuracy_nospread"));

	if (nospread->GetBool())
	{
		final_hitchance = INT_MAX;
		return true;
	}

	// calculate inaccuracy.
	const auto weapon_inaccuracy = g_ctx.globals.weapon->get_inaccuracy();

	// calculate start and angle.
	static auto weapon_recoil_scale = m_cvar()->FindVar(crypt_str("weapon_recoil_scale"));
	const auto aim_angle = math::calculate_angle(g_ctx.globals.eye_pos, final_target.data.point.point) - (g_ctx.local()->m_aimPunchAngle() * weapon_recoil_scale->GetFloat());
	auto forward = ZERO;
	auto right = ZERO;
	auto up = ZERO;

	math::angle_vectors(aim_angle, &forward, &right, &up);

	math::fast_vec_normalize(forward);
	math::fast_vec_normalize(right);
	math::fast_vec_normalize(up);

	// keep track of all traces that hit the enemy.
	auto current = 0;

	// setup calculation parameters.
	Vector total_spread, spread_angle, end;
	float inaccuracy, spread_x, spread_y;
	std::tuple<float, float, float>* seed;

	// use look-up-table to find average hit probability.
	for (auto i = 0u; i < 255; i++)  // NOLINT(modernize-loop-convert)
	{
		// get seed.
		seed = &precomputed_seeds[i];

		// calculate spread.
		inaccuracy = std::get<0>(*seed) * weapon_inaccuracy;
		spread_x = std::get<2>(*seed) * inaccuracy;
		spread_y = std::get<1>(*seed) * inaccuracy;
		total_spread = (forward + right * spread_x + up * spread_y);
		total_spread.Normalize();

		// calculate angle with spread applied.
		math::vector_angles(total_spread, spread_angle);

		// calculate end point of trace.
		math::angle_vectors(spread_angle, end);
		end.Normalize();
		end = g_ctx.globals.eye_pos + end * info->flRange;

		// did we hit the hitbox?
		if (hitbox_intersection(final_target.record->player, final_target.record->matrixes_data.main, final_target.data.hitbox, g_ctx.globals.eye_pos, end))
			current++;

		// abort if hitchance is already sufficent.
		if ((static_cast<float>(current) / 255.f) * 100.f >= hitchance_cfg)
		{
			final_hitchance = (static_cast<float>(current) / 255.f) * 100.f;
			return true;
		}

		// abort if we can no longer reach hitchance.
		if ((static_cast<float>(current + 255 - i) / 255.f) * 100.f < hitchance_cfg)
		{
			final_hitchance = (static_cast<float>(current + 255 - i) / 255.f) * 100.f;
			return false;
		}
	}

	final_hitchance = (static_cast<float>(current) / 255.f) * 100.f;
	return (static_cast<float>(current) / 255.f) * 100.f >= hitchance_cfg;
}

bool aim::hitbox_intersection(player_t* e, matrix3x4_t* matrix, int hitbox, const Vector& start, const Vector& end)
{
	// note: simv0l - pasted from r7 software.
	auto model = e->GetModel();

	if (!model)
		return false;

	auto studio_model = m_modelinfo()->GetStudioModel(model);

	if (!studio_model)
		return false;

	auto studio_set = studio_model->pHitboxSet(e->m_nHitboxSet());

	if (!studio_set)
		return false;

	auto studio_hitbox = studio_set->pHitbox(hitbox);

	if (!studio_hitbox)
		return false;

	Vector min, max;

	const auto is_capsule = studio_hitbox->radius != -1.f;

	if (is_capsule)
	{
		math::vector_transform(studio_hitbox->bbmin, matrix[studio_hitbox->bone], min);
		math::vector_transform(studio_hitbox->bbmax, matrix[studio_hitbox->bone], max);
		const auto dist = math::segment_to_segment(start, end, min, max);

		if (dist < studio_hitbox->radius)
			return true;
	}
	else
	{
		math::vector_transform(math::vector_rotate(studio_hitbox->bbmin, studio_hitbox->rotation), matrix[studio_hitbox->bone], min);
		math::vector_transform(math::vector_rotate(studio_hitbox->bbmax, studio_hitbox->rotation), matrix[studio_hitbox->bone], max);

		math::vector_i_transform(start, matrix[studio_hitbox->bone], min);
		math::vector_i_rotate(end, matrix[studio_hitbox->bone], max);

		if (math::intersect_line_with_bb(min, max, studio_hitbox->bbmin, studio_hitbox->bbmax))
			return true;
	}

	return false;
}