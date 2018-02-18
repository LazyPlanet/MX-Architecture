#include <string>

#include "Clan.h"
#include "RedisManager.h"
#include "Timer.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

void Clan::Update()
{
	if (_dirty) Save();
}

void Clan::OnApply(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;
	
	auto player_id = player->GetID();
	auto oper_type = message->oper_type();

	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [player_id](const Asset::SystemMessage& message){
				return player_id == message.player_id();	
			});

	if (it == _stuff.mutable_message_list()->end())
	{
		auto system_message = _stuff.mutable_message_list()->Add();
		system_message->set_player_id(player_id);
		system_message->set_name(player->GetName());
		system_message->set_oper_time(TimerInstance.GetTime());
		system_message->set_oper_type(oper_type);
	}
	else
	{
		it->set_oper_time(TimerInstance.GetTime());
		it->set_oper_type(oper_type);
	}

	message->set_oper_result(Asset::ERROR_SUCCESS);
	player->SendProtocol(message);

	_dirty = true;
}

void Clan::OnAgree(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;
	
	auto member_id = message->dest_player_id();
	auto oper_type = message->oper_type();

	//
	//申请列表状态更新
	//
	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [member_id](const Asset::SystemMessage& message){
				return member_id == message.player_id();	
			});

	if (it == _stuff.mutable_message_list()->end()) return; //尚未申请记录
	
	if (oper_type == it->oper_type()) return; //状态一致

	it->set_oper_time(TimerInstance.GetTime());
	it->set_oper_type(oper_type);

	//
	//成员列表更新
	//
	auto it_ = std::find_if(_stuff.mutable_member_list()->begin(), _stuff.mutable_member_list()->end(), [member_id](const Asset::Clan_Member& member){
				return member_id == member.player_id();
			});

	if (it_ != _stuff.mutable_member_list()->end()) return; //已经是成员

	auto member_ptr = _stuff.mutable_member_list()->Add();
	member_ptr->set_player_id(it->player_id());
	member_ptr->set_name(it->name());
	member_ptr->set_status(Asset::CLAN_MEM_STATUS_TYPE_AVAILABLE);

	message->set_oper_result(Asset::ERROR_SUCCESS);
	player->SendProtocol(message);

	_dirty = true;
}
	
void Clan::OnDisAgree(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;
	
	auto member_id = message->dest_player_id();
	auto oper_type = message->oper_type();

	//
	//申请列表状态更新
	//
	auto it = std::find_if(_stuff.mutable_message_list()->begin(), _stuff.mutable_message_list()->end(), [member_id](const Asset::SystemMessage& message){
				return member_id == message.player_id();	
			});

	if (it == _stuff.mutable_message_list()->end()) return; //尚未申请记录
	
	if (oper_type == it->oper_type()) return; //状态一致

	it->set_oper_time(TimerInstance.GetTime());
	it->set_oper_type(oper_type);

	message->set_oper_result(Asset::ERROR_SUCCESS);
	player->SendProtocol(message);

	_dirty = true;
}

void Clan::OnChangedInformation(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!player || !message) return;

	auto hoster_id = _stuff.hoster_id();

	if (hoster_id != player->GetID())
	{
		message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
		player->SendProtocol(message);
		return;
	}

	auto name = message->name();
	auto announcement = message->announcement();

	if (name.size()) _stuff.set_name(name);
	if (announcement.size()) _stuff.set_name(announcement);

	message->set_oper_result(Asset::ERROR_SUCCESS);
	player->SendProtocol(message);
	
	_dirty = true;
}

void Clan::Save(bool force)
{
	if (force)
	{
		RedisInstance.Save("clan:" + std::to_string(_clan_id), _stuff);
	}
	else if (!_dirty)
	{
		return;
	}

	_dirty = false;
}

void Clan::OnDisMiss()
{
	_stuff.set_dismiss(true); //解散

	_dirty = true;
}

void ClanManager::Update(int32_t diff)
{
	if (_heart_count % 60 != 0) return;  //3秒

	++_heart_count;

	std::lock_guard<std::mutex> lock(_mutex);
	
	for (auto it = _clans.begin(); it != _clans.end();)
	{
		if (!it->second)
		{
			it = _clans.erase(it);
			continue; 
		}
		else
		{
			it->second->Update();
			++it;
		}
	}
}

void ClanManager::Remove(int64_t clan_id)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (clan_id <= 0) return;

	auto it = _clans.find(clan_id);
	if (it == _clans.end()) return;
		
	if (it->second) 
	{
		it->second->OnDisMiss(); //解散
		it->second->Save();
		it->second.reset();
	}

	_clans.erase(it);
}

void ClanManager::Remove(std::shared_ptr<Clan> clan)
{
	if (!clan) return;

	Remove(clan->GetID());
}

void ClanManager::Emplace(int64_t clan_id, std::shared_ptr<Clan> clan)
{
	if (clan_id <= 0 || !clan) return;

	_clans[clan_id] = clan;
}

std::shared_ptr<Clan> ClanManager::GetClan(int64_t clan_id)
{
	auto it = _clans.find(clan_id);
	if (it == _clans.end()) return nullptr;

	return it->second;
}

std::shared_ptr<Clan> ClanManager::Get(int64_t clan_id)
{
	return GetClan(clan_id);
}

int32_t ClanManager::OnOperate(std::shared_ptr<Player> player, Asset::ClanOperation* message)
{
	if (!message || !player) return 1;
	
	defer {
		player->SendProtocol(message); //返回结果
	};

	switch (message->oper_type())
	{
		case Asset::CLAN_OPER_TYPE_CREATE: //创建
		{
			auto clan_limit = dynamic_cast<Asset::ClanLimit*>(AssetInstance.Get(g_const->clan_id()));
			if (!clan_limit) return 6;

			const auto& trim_name = message->name();

			if (trim_name.empty()) 
			{
				message->set_oper_result(Asset::ERROR_CLAN_NAME_EMPTY);
				return 2;
			}
			if ((int32_t)trim_name.size() > clan_limit->name_limit())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NAME_UPPER);
				return 3;
			}
			if (player->GetHosterCount() > clan_limit->create_upper_limit())
			{
				message->set_oper_result(Asset::ERROR_CLAN_HOSTER_UPPER);
				return 4;
			}
			auto clan_id = RedisInstance.CreateClan();
			if (clan_id == 0)
			{
				message->set_oper_result(Asset::ERROR_CLAN_CREATE_INNER);
				return 5;
			}
	
			if (player->GetRoomCard() < clan_limit->room_card_limit())
			{
				message->set_oper_result(Asset::ERROR_CLAN_ROOM_CARD_NOT_ENOUGH); //房卡不足
				return 7;
			}

			player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_CREATE_CLAN, clan_limit->room_card_limit()); //扣除馆长房卡

			message->set_clan_id(clan_id);
			message->set_oper_result(Asset::ERROR_SUCCESS);

			Asset::Clan clan;
			clan.set_clan_id(clan_id);
			clan.set_name(trim_name);
			clan.set_created_time(CommonTimerInstance.GetTime());
			clan.set_hoster_id(player->GetID());
			clan.set_hoster_name(player->GetName());
			clan.set_room_card_count(clan_limit->room_card_limit());

			message->mutable_clan()->CopyFrom(clan); //回传Client

			auto clan_ptr = std::make_shared<Clan>(clan);
			OnCreated(clan_id, clan_ptr); //创建成功
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_JOIN: //申请加入
		{
			auto clan = ClanInstance.Get(message->clan_id());
			if (!clan)
			{
				message->set_oper_result(Asset::ERROR_CLAN_NOT_FOUND); //没找到茶馆
				return 8;
			}

			clan->OnApply(player, message); //申请成功
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_EDIT: //修改基本信息
		{
			auto clan = ClanInstance.Get(message->clan_id());
			if (!clan) return 9;

			clan->OnChangedInformation(player, message);
		}
		break;
	
		case Asset::CLAN_OPER_TYPE_DISMISS: //解散
		{
			auto clan = ClanInstance.Get(message->clan_id());
			if (!clan) return 9;

			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return 10;
			}

			Remove(message->clan_id());
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_AGEE: //同意加入
		{
			auto clan = ClanInstance.Get(message->clan_id());
			if (!clan) return 9;
			
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return 10;
			}
			
			clan->OnAgree(player, message);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_DISAGEE: //拒绝加入
		{
			auto clan = ClanInstance.Get(message->clan_id());
			if (!clan) return 9;
			
			if (clan->GetHoster() != player->GetID())
			{
				message->set_oper_result(Asset::ERROR_CLAN_NO_PERMISSION);
				return 10;
			}
			
			clan->OnDisAgree(player, message);
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_DELETE: //删除成员
		{
		}
		break;
		
		case Asset::CLAN_OPER_TYPE_MEMEBER_QUIT: //主动退出
		{
		}
		break;
	
		default:
		{
			return 0;
		}
		break;
	}

	return 0;
}
	
void ClanManager::OnCreated(int64_t clan_id, std::shared_ptr<Clan> clan)
{
	if (clan_id <= 0 || !clan) return;
		
	Emplace(clan_id, clan);
}

}

