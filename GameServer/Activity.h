#pragma once

#include <memory>
#include <functional>

#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/date_time/posix_time/conversion.hpp"

#include "Timer.h"
#include "P_Header.h"
#include "Asset.h"

namespace Adoter
{
namespace pb = google::protobuf;

class Activity : public std::enable_shared_from_this<Activity> 
{
private:
	int32_t _heart_count = 0;
	std::unordered_map<int64_t/*global_id*/, Asset::Activity*> _activity; //可以通过GMT命令进行修改
	std::unordered_map<int64_t/*global_id*/, bool/*opened*/> _state;
public:

	static Activity& Instance()
	{
		static Activity _instance;
		return _instance;
	}

	bool Load()
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ACTIVITY);

		for (auto it = messages.begin(); it != messages.end(); ++it)
		{
			const auto activity = dynamic_cast<const Asset::Activity*>(*it);
			if (!activity) return false;

			_activity.emplace(activity->common_prop().global_id(), activity);
		}

		return true;
	}
	
	bool IsOpen(int64_t global_id)
	{
		auto it = _state.find(global_id);
		if (it == _state.end()) return false; //如果没有，则认为是关闭状态

		return it->second;
	}
	
	void Update(int32_t diff)
	{
		++_heart_count;

		bool has_changed = false;

		for (auto it = _activity.begin(); it != _activity.end(); ++it)
		{
			boost::posix_time::ptime start_time(boost::posix_time::time_from_string(it->second->start_time()));
			auto start_time_t = boost::posix_time::to_time_t(start_time);

			boost::posix_time::ptime stop_time(boost::posix_time::time_from_string(it->second->stop_time()));
			auto stop_time_t = boost::posix_time::to_time_t(stop_time);
			
			std::time_t cur_t = CommonTimerInstance.GetTime();

			if (cur_t < start_time_t || cur_t > stop_time_t) 
			{
				if (_state[it->first])
				{
					has_changed = true; //状态变化
					_state[it->first] = false;
				}
			}
			else
			{
				if (!_state[it->first])
				{
					has_changed = true; //状态变化
					_state[it->first] = true;
				}
			}
		}

		if (has_changed)
		{
			Asset::SyncActivity message; //数据变化

			for (auto it = _state.begin(); it != _state.end(); ++it)
			{
				auto activity = message.mutable_activity_list()->Add();
				activity->set_activity_id(it->first);
				activity->set_open(it->second);
			}

			PlayerInstance.BroadCast(message);
		}
	}

};

#define ActivityInstance Activity::Instance()

}

