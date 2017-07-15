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
	const std::string _password = "!QAZ%TGB&UJM9ol.";

	redisContext* _client;
public:
	~Redis() { redisFree(_client); }
	
	static Redis& Instance()
	{
		static Redis _instance;
		return _instance;
	}

	Redis() 
	{ 
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

		if (reply->type != REDIS_REPLY_INTEGER) return 0;
		
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

		if (reply->type == REDIS_REPLY_NIL) return false;
		
		if (reply->type != REDIS_REPLY_STRING) return false;
		
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
		
		if (reply->type != REDIS_REPLY_STRING) return false;

		freeReplyObject(reply);
		
		return true;
	}
	
	int64_t CreateRoom()
	{
		redisReply* reply = (redisReply*)redisCommand(_client, "Incr room_counter");
		if (!reply) return 0;

		if (reply->type != REDIS_REPLY_INTEGER) return 0;
		
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

	bool GetUser(std::string username, Asset::User& user)
	{
		std::string command = "Get user:" + username;
		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());

		if (!reply) return false;

		if (reply->type == REDIS_REPLY_NIL) return false;
		
		if (reply->type != REDIS_REPLY_STRING) return false;

		if (reply->len == 0) return false;

		auto success = user.ParseFromArray(reply->str, reply->len);

		freeReplyObject(reply);

		return success;
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

		freeReplyObject(reply);
		
		return true;
	}
	
	bool GetGuestAccount(std::string& account)
	{
		redisReply* reply = (redisReply*)redisCommand(_client, "Incr guest_counter");
		if (!reply) return false;

		if (reply->type != REDIS_REPLY_INTEGER) return false;
		
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

		if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 1) return false;

		if (reply->element[0]->elements != 2) return false;

		if (!reply->element[0]->element[0]->str || !reply->element[0]->element[1]->str) return false;

		double longitude = atof(reply->element[0]->element[0]->str);
		double latitude = atof(reply->element[0]->element[1]->str);

		location.set_longitude(longitude);
		location.set_latitude(latitude);
		
		return true;
	}

	double GetDistance(int64_t player_id1, int64_t player_id2)
	{
		std::string command = "geodist players_location player:" + std::to_string(player_id1) + " player:" + std::to_string(player_id2);

		redisReply* reply = (redisReply*)redisCommand(_client, command.c_str());
		if (!reply) return -1.0;

		if (reply->type == REDIS_REPLY_NIL || !reply->str) return -1.0;

		double dist = atof(reply->str);
		return dist;
	}
};

#define RedisInstance Redis::Instance()

}
