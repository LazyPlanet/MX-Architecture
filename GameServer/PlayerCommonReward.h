#pragma once

#include <memory>
#include <functional>

#include "P_Header.h"
#include "MXLog.h"

namespace Adoter
{
namespace pb = google::protobuf;

class PlayerCommonReward : public std::enable_shared_from_this<PlayerCommonReward> 
{
public:
	static PlayerCommonReward& Instance()
	{
		static PlayerCommonReward _instance;
		return _instance;
	}

	bool DeliverReward(std::shared_ptr<Player> player, int64_t global_id)
	{
		if (!player || global_id <= 0) return false;

		const auto message = AssetInstance.Get(global_id);
		if (!message) return false;

		const auto common_reward = dynamic_cast<const Asset::CommonReward*>(message);
		if (!common_reward) return false;

		for (const auto& reward : common_reward->rewards())
		{
			int64_t common_limit_id = reward.common_limit_id();
			if (player->IsCommonLimit(common_limit_id)) return false; //该奖励已经超过领取限制

			int32_t count = reward.count();

			switch (reward.reward_type())
			{
				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_DIAMOND:
				{
					player->GainDiamond(Asset::DIAMOND_CHANGED_TYPE_GENERAL_REWARD, count);
				}
				break;

				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_HUANLEDOU:
				{
					player->GainHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GENERAL_REWARD, count);
				}
				break;
				
				case Asset::CommonReward_REWARD_TYPE_REWARD_TYPE_ITEM:
				{
					player->GainItem(reward.item_id(), count);
				}
				break;

				default:
				{

				}
				break;
			}

			LOG(INFO, "player_id:{} global_id:{} common_limit_id:{} reward_type:{}", player->GetID(), global_id, common_limit_id, reward.reward_type());

			player->AddCommonLimit(common_limit_id); 
		}
		return true;
	}

};

#define CommonRewardInstance PlayerCommonReward::Instance()

}
