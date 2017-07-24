#pragma once

#include <hiredis.h>
#include <string>
#include <iostream>

#include "MXLog.h" 
#include "Config.h" 

/*
 * 数据库管理
 *
 * 所有表名称都要小写
 *
 *
 * */

namespace Adoter
{

class Redis 
{
private:
	std::string _hostname = "127.0.0.1";
	int32_t _port = 6379;
	struct timeval _timeout = {1, 500000}; //1.5秒 
	std::string _password = "!QAZ%TGB&UJM9ol.";

	redisContext* _client = nullptr;
public:
	~Redis() { if (_client) redisFree(_client);	}
	
	//
	//Redis多线程限制
	//
	//不能利用单例模式
	//
	static Redis& Instance()
	{
		static Redis _instance;
		return _instance;
	}

	Redis() 
	{ 
		std::string hostname = ConfigInstance.GetString("Redis_ServerIP", "127.0.0.1");
		int32_t port = ConfigInstance.GetInt("Redis_ServerPort", 6379); 
		std::string password = ConfigInstance.GetString("Redis_Password", "!QAZ%TGB&UJM9ol.");

		if (port > 0) _port = port;
		if (!hostname.empty()) _hostname = hostname;
		if (!password.empty()) _password = password;

		_client = redisConnectWithTimeout(_hostname.c_str(), _port, _timeout);

		redisReply* reply= (redisReply*)redisCommand(_client, "auth %s", _password.c_str()); 
		if (reply && reply->type == REDIS_REPLY_ERROR) { 
			LOG(ERROR, "Redis密码错误");
		} 
		freeReplyObject(reply);
	}

	Redis(std::string& hostname, int32_t port) : _hostname(hostname), _port(port)
	{ 
		_client = redisConnectWithTimeout(hostname.c_str(), port, _timeout);
		
		redisReply* reply= (redisReply*)redisCommand(_client, "auth %s", _password.c_str()); 
		if (reply && reply->type == REDIS_REPLY_ERROR) { 
			LOG(ERROR, "Redis密码错误");
		} 
		freeReplyObject(reply);
	}

	redisContext* GetClient() { return _client; }

	redisReply* ExcuteCommand(std::string command)
	{
		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());
		return reply;
	}

	int64_t CreatePlayer()
	{
		redisReply* reply = (redisReply*)redisCommand(_client, "Incr player_counter");
		if (!reply) return 0;

		if (reply->type != REDIS_REPLY_INTEGER) 
		{
			freeReplyObject(reply);
			return 0;
		}
		
		int64_t player_id = reply->integer;
		freeReplyObject(reply);
		
		//
		//角色ID带有服务器ID
		//
		//每个服务器含有65536个角色，从游戏角度数量足够
		//
		int32_t server_id = ConfigInstance.GetInt("ServerID", 1); //服务器ID
		player_id = (server_id << 16) + player_id; 

		return player_id;
	}

	bool GetPlayer(int64_t player_id, Asset::Player& player)
	{
		std::string command = "Get player:" + std::to_string(player_id);
		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());

		if (!reply) return false;

		if (reply->type == REDIS_REPLY_NIL) 
		{
			LOG(ERROR, "获取玩家数据失败，player_id:{} reply->type:{}", player_id, reply->type);
			freeReplyObject(reply);
			return false;
		}
		
		if (reply->type != REDIS_REPLY_STRING) 
		{
			LOG(ERROR, "获取玩家数据失败，player_id:{} reply->type:{}", player_id, reply->type);
			freeReplyObject(reply);
			return false;
		}
		
		auto success = player.ParseFromArray(reply->str, reply->len);
		freeReplyObject(reply);

		return success;
	}
	
	bool SavePlayer(int64_t player_id, const Asset::Player& player)
	{
		const int player_length = player.ByteSize();
		char player_buff[player_length];
		memset(player_buff, 0, player_length);

		player.SerializeToArray(player_buff, player_length);

		std::string key = "player:" + std::to_string(player_id);

		redisReply* reply = (redisReply*)redisCommand(_client, "Set %s %b", key.c_str(), player_buff, player_length);
		if (!reply) return false;
		
		if (!(reply->type == REDIS_REPLY_STATUS && strcasecmp(reply->str, "OK") == 0)) 
		{
			LOG(ERROR, "保存玩家数据失败，reply->type:{} player_id:{} player:{}", reply->type, player_id, player.ShortDebugString());
		}
		freeReplyObject(reply);
			
		LOG(INFO, "保存玩家数据，player_id:{} player:{}", player_id, player.ShortDebugString());
		
		return true;
	}
	
	int64_t CreateRoom()
	{
		redisReply* reply = (redisReply*)redisCommand(_client, "Incr room_counter");
		if (!reply) return 0;

		if (reply->type != REDIS_REPLY_INTEGER) 
		{
			freeReplyObject(reply);
			return 0;
		}
		
		int64_t room_id = reply->integer;
		freeReplyObject(reply);
		
		//
		//房间ID带有服务器ID
		//
		//每个服务器上限65536个房间，玩家通过房间ID即可获取当前房间所在服务器
		//
		int32_t server_id = ConfigInstance.GetInt("ServerID", 1); //服务器ID
		room_id = (server_id << 16) + room_id;
		
		return room_id;
	}

	int32_t GetUser(std::string username, Asset::User& user)
	{
		std::string command = "Get user:" + username;
		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());

		if (!reply) return 0;
		
		auto type = reply->type;

		if (reply->type == REDIS_REPLY_NIL) 
		{
			freeReplyObject(reply);
			return type;
		}
		
		if (reply->type != REDIS_REPLY_STRING) 
		{
			LOG(ERROR, "获取账号数据失败，username:{} reply->type:{}", username, reply->type);
			freeReplyObject(reply);
			return type;
		}

		if (reply->len == 0) 
		{
			freeReplyObject(reply);
			return type;
		}

		auto success = user.ParseFromArray(reply->str, reply->len);
		if (!success)
		{
			LOG(ERROR, "转换协议数据失败，username:{} reply->str:{} reply->len:{}", username, reply->str, reply->len);
		}

		freeReplyObject(reply);

		return type;
	}
	
	bool SaveUser(std::string username, const Asset::User& user)
	{
		const int user_length = user.ByteSize();
		char user_buff[user_length];
		memset(user_buff, 0, user_length);

		user.SerializeToArray(user_buff, user_length);
		
		std::string key = "user:" + username;

		redisReply* reply = (redisReply*)redisCommand(_client, "Set %s %b", key.c_str(), user_buff, user_length);
		if (!reply) return false;
		
		if (!(reply->type == REDIS_REPLY_STATUS && strcasecmp(reply->str, "OK") == 0)) 
		{
			LOG(ERROR, "保存账号数据失败，username:{} user:{}", username, user.ShortDebugString());
		}
		freeReplyObject(reply);
			
		LOG(INFO, "保存账号数据，username:{} user:{}", username, user.ShortDebugString());

		return true;
	}
	
	bool GetGuestAccount(std::string& account)
	{
		redisReply* reply = (redisReply*)redisCommand(_client, "Incr guest_counter");
		if (!reply) return false;

		if (reply->type != REDIS_REPLY_INTEGER) 
		{
			LOG(ERROR, "获取游客数据失败，account:{} reply->type:{}", account, reply->type);
			freeReplyObject(reply);
			return false;
		}
		
		int64_t guest_id = reply->integer;
		freeReplyObject(reply);
		
		account = "guest_" + std::to_string(guest_id);
		return true;
	}

	bool SetLocation(int64_t player_id, Asset::Location location)
	{
		double longitude = location.longitude();
		double latitude = location.latitude();

		std::string key = "player:" + std::to_string(player_id);
		std::string command = "geoadd players_location " + std::to_string(longitude) + " " + std::to_string(latitude) + " " + key;

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str()); 
		if (!reply) return false;

		return true;
	}

	bool GetLocation(int64_t player_id, Asset::Location& location)
	{
		std::string key = "player:" + std::to_string(player_id);
		std::string command = "geopos players_location " + key;

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());
		if (!reply) return false;

		if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 1) 
		{
			freeReplyObject(reply);
			return false;
		}

		if (reply->element[0]->elements != 2) 
		{
			freeReplyObject(reply);
			return false;
		}

		if (!reply->element[0]->element[0]->str || !reply->element[0]->element[1]->str) 
		{
			freeReplyObject(reply);
			return false;
		}

		double longitude = atof(reply->element[0]->element[0]->str);
		double latitude = atof(reply->element[0]->element[1]->str);
		freeReplyObject(reply);

		location.set_longitude(longitude);
		location.set_latitude(latitude);
		
		return true;
	}

	double GetDistance(int64_t player_id1, int64_t player_id2)
	{
		std::string command = "geodist players_location player:" + std::to_string(player_id1) + " player:" + std::to_string(player_id2);

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());
		if (!reply) return -1.0;

		if (reply->type == REDIS_REPLY_NIL || !reply->str) 
		{
			freeReplyObject(reply);
			return -1.0;
		}

		double dist = atof(reply->str);
		freeReplyObject(reply);
		return dist;
	}
	
	int32_t GetRoomHistory(int64_t room_id, Asset::RoomHistory history)
	{
		std::string command = "Get room_history:" + std::to_string(room_id);
		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());

		if (!reply) return 0;
		
		auto type = reply->type;

		if (reply->type == REDIS_REPLY_NIL) 
		{
			freeReplyObject(reply);
			return type;
		}
		
		if (reply->type != REDIS_REPLY_STRING) 
		{
			freeReplyObject(reply);
			return type;
		}

		if (reply->len == 0) 
		{
			freeReplyObject(reply);
			return type;
		}

		auto success = history.ParseFromArray(reply->str, reply->len);
		if (!success)
		{
			LOG(ERROR, "转换协议数据失败，room_id:{} reply->str:{} reply->len:{}", room_id, reply->str, reply->len);
		}

		freeReplyObject(reply);

		return type;
	}
	
	bool SaveRoomHistory(int64_t room_id, const Asset::RoomHistory& history)
	{
		const int user_length = history.ByteSize();
		char user_buff[user_length];
		memset(user_buff, 0, user_length);

		history.SerializeToArray(user_buff, user_length);
		
		std::string key = "user:" + std::to_string(room_id);

		redisReply* reply = (redisReply*)redisCommand(_client, "Set %s %b", key.c_str(), user_buff, user_length);
		if (!reply) return false;
		
		if (!(reply->type == REDIS_REPLY_STATUS && strcasecmp(reply->str, "OK") == 0)) 
		{
			LOG(ERROR, "保存历史战绩数据失败，room_id:{} history:{}", room_id, history.ShortDebugString());
		}
		freeReplyObject(reply);
			
		LOG(INFO, "保存历史战绩数据，room_id:{} history:{}", room_id, history.ShortDebugString());

		return true;
	}
};

#define RedisInstance Redis::Instance()

}
