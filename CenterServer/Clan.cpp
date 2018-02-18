#include <string>

#include "Clan.h"
#include "RedisManager.h"

namespace Adoter
{

void Clan::Update()
{
	if (_dirty) Save();
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

void ClanManager::Update(int32_t diff)
{
	++_heart_count;

	std::lock_guard<std::mutex> lock(_mutex);
	
	if (_heart_count % (20 * 60 * 30) == 0) //30分钟
	{
	}

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

}
