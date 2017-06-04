#pragma once

#include <hiredis.h>
#include <string>
#include <iostream>

#include "MXLog.h" 

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

	redisContext* _client;
public:
	~Redis() { redisFree(_client); }

	Redis() 
	{ 
		_client = redisConnectWithTimeout(_hostname.c_str(), _port, _timeout);
	}

	Redis(std::string& hostname, int32_t port) : _hostname(hostname), _port(port)
	{ 
		_client = redisConnectWithTimeout(hostname.c_str(), port, _timeout);
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
		
		LOG(TRACE, "create player_id:{} success", player_id);

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
};

}
