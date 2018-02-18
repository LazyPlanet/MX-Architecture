#pragma once

#include <mutex>
#include <memory>
#include <unordered_map>
#include <functional>

#include "P_Header.h"
#include "Player.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Clan : public std::enable_shared_from_this<Clan>
{
private:
	Asset::Clan _stuff;
	bool _dirty = false;
	int64_t _clan_id = 0;
public:
	Clan(const Asset::Clan& clan) { _clan_id = clan.clan_id(); _stuff = clan; }

	const Asset::Clan& Get() { return _stuff; }
	int64_t GetID() { return _stuff.clan_id(); }

	void Update();
	void Save(bool force = true);
};

class ClanManager : public std::enable_shared_from_this<ClanManager>
{
private:
	std::mutex _mutex;
	std::unordered_map<int64_t, std::shared_ptr<Clan>> _clans; 
	int64_t _heart_count = 0;
public:
	static ClanManager& Instance()
	{
		static ClanManager _instance;
		return _instance;
	}

	void Update(int32_t diff);

	void Remove(int64_t Clan_id);
	void Remove(std::shared_ptr<Clan> clan);
	void Emplace(int64_t Clan_id, std::shared_ptr<Clan> clan);

	std::shared_ptr<Clan> GetClan(int64_t clan_id);
	std::shared_ptr<Clan> Get(int64_t clan_id);

	int32_t OnOperate(std::shared_ptr<Player> player, Asset::ClanOperation* message);
	void OnCreated(int64_t clan_id, std::shared_ptr<Clan> clan); //创建成功
};

#define ClanInstance ClanManager::Instance()
}
