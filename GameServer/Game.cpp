#include <vector>
#include <algorithm>

#include "Game.h"
#include "Timer.h"
#include "Asset.h"
#include "MXLog.h"
#include "CommonUtil.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

/////////////////////////////////////////////////////
//一场游戏
/////////////////////////////////////////////////////
void Game::Init(std::shared_ptr<Room> room)
{
	_cards.clear();
	_cards.resize(136);

	std::iota(_cards.begin(), _cards.end(), 1);
	std::vector<int32_t> cards(_cards.begin(), _cards.end());

	std::random_shuffle(cards.begin(), cards.end()); //洗牌

	_cards = std::list<int32_t>(cards.begin(), cards.end());

	/*
	auto log = make_unique<Asset::LogMessage>();
	log->set_type(Asset::GAME_CARDS);
	for (auto card : _cards) log->mutable_cards()->Add(card);
	LOG(INFO, log.get()); 
	*/
	_room = room;
}

bool Game::Start(std::vector<std::shared_ptr<Player>> players)
{
	if (MAX_PLAYER_COUNT != players.size()) return false; //做下检查，是否满足开局条件

	if (!_room)
	{
		DEBUG_ASSERT(false);
		return false;
	}

	//
	//房间(Room)其实是游戏(Game)的管理类
	//
	//复制房间数据到游戏中
	//
	int32_t player_index = 0;

	for (auto player : players)
	{
		_players[player_index++] = player; 

		player->SetRoom(_room);
		player->SetPosition(Asset::POSITION_TYPE(player_index));
	}

	//当前庄家
	auto banker_index = _room->GetBankerIndex();
		
	auto player_banker = GetPlayerByOrder(banker_index);
	if (!player_banker) 
	{
		DEBUG_ASSERT(false);
		return false;
	}
	_banker_player_id  = player_banker->GetID();
	
	OnStart(); //同步本次游戏开局数据：此时玩家没有进入游戏

	//
	//设置游戏数据
	//
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) 
		{
			DEBUG_ASSERT(false);
			return false;
		}
		player->SetGame(shared_from_this());

		DEBUG("player_id:{} player_index:{} start game.", player->GetID(), i);

		int32_t card_count = 13; //正常开启，普通玩家牌数量

		if (banker_index % MAX_PLAYER_COUNT == i) 
		{
			card_count = 14; //庄家牌数量
			_curr_player_index = i; //当前操作玩家
		}

		auto cards = FaPai(card_count);

		player->OnFaPai(cards);  //各个玩家发牌
	}

	return true;
}
	
void Game::OnStart()
{
	if (!_room) return;

	_room->SetBanker(_banker_player_id); //设置庄家

	//游戏数据广播
	Asset::GameInformation info;
	info.set_banker_player_id(_banker_player_id);
	BroadCast(info);
	
	//开局股子广播
	Asset::RandomSaizi saizi;
	saizi.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_START);
	saizi.set_player_id(_banker_player_id); //庄家打股

	for (int i = 0; i < 2; ++i)
	{
		int32_t result = CommonUtil::Random(1, 6);
		saizi.mutable_random_result()->Add(result);
	}
	_room->BroadCast(saizi);
}

bool Game::OnOver()
{
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) 
		{
			ERROR("player_index:{} has not found, maybe it has disconneced.", i);
			continue;
		}
		player->OnGameOver();
	}
	
	_baopai.Clear();
	_oper_limit.Clear();
	_oper_list.clear();

	return true;
}

/////////////////////////////////////////////////////
//
//玩家可操作的状态只有2种，顺序不可变：
//
//(1) 碰、杠、胡牌；
//
//(2) 轮到玩家；
//
/////////////////////////////////////////////////////
	
bool Game::CanPaiOperate(std::shared_ptr<Player> player)
{
	if (/*_oper_limit.time_out() < CommonTimerInstance.GetTime() && 超时检查*/_oper_limit.player_id() == player->GetID()) 
	{
		return true; //玩家操作：碰、杠、胡牌
	}

	auto player_index = GetPlayerOrder(player->GetID());

	if (_curr_player_index == player_index) 
	{
		return true; //轮到该玩家
	}

	CRITICAL("curr_player_index:{} player_index:{} player_id:{} oper_limit_player_id:{}", _curr_player_index, player_index, 
			player->GetID(), _oper_limit.player_id());
	return false;
}

void Game::OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	if (!player || !message || !_room) return;
	
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return; 
	
	DEBUG("player_id:{} server saved infomation from_player_id:{} card_type:{} card_value:{} oper_type:{}, while client sentd to card_type:{} card_value:{} oper_type:{}", player->GetID(), _oper_limit.from_player_id(), _oper_limit.pai().card_type(), _oper_limit.pai().card_value(), _oper_limit.oper_type(), pai_operate->pai().card_type(), pai_operate->pai().card_value(), pai_operate->oper_type());

	if (!CanPaiOperate(player)) 
	{
		player->AlertMessage(Asset::ERROR_GAME_NO_PERMISSION); //没有权限，没到玩家操作，防止外挂
		DEBUG_ASSERT(false); 
	}

	//if (CommonTimerInstance.GetTime() < _oper_limit.time_out()) ClearOperation(); //已经超时，清理缓存以及等待玩家操作的状态

	//如果不是放弃，才是当前玩家的操作
	if (Asset::PAI_OPER_TYPE_GIVEUP != pai_operate->oper_type())
	{
		_curr_player_index = GetPlayerOrder(player->GetID()); //上面检查过，就说明当前该玩家可操作
		BroadCast(message); //广播玩家操作，玩家放弃操作不能广播
	}

	//const auto& pai = _oper_limit.pai(); //缓存的牌
	const auto& pai = pai_operate->pai(); //玩家发上来的牌

	//一个人打牌之后，要检查其余每个玩家手中的牌，且等待他们的操作，直到超时
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		case Asset::PAI_OPER_TYPE_TINGPAI: //听牌
		{
			//
			//(1) 检查各个玩家手里的牌是否满足胡、杠、碰、吃
			//
			//(2) 检查是否到了流局阶段
			//
			//(3) 否则给当前玩家的下家继续发牌
			//
			if (CheckPai(pai, player->GetID())) //有满足要求的玩家
			{
				SendCheckRtn();
			}
			else if (CheckLiuJu())
			{
				return;
			}
			else
			{
				auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; 

				auto player_next = GetPlayerByOrder(next_player_index);
				
				if (!player_next) 
				{
					DEBUG_ASSERT(false);
					return; 
				}
				
				DEBUG("player_id:{} next_player_id:{} _curr_player_index:{} next_player_index:{}", 
						player->GetID(), player_next->GetID(), _curr_player_index, next_player_index);

				auto cards = FaPai(1); 
				auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

				Asset::PaiOperationAlert alert;

				//胡牌检查
				if (player_next->CheckHuPai(card)) //自摸
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(card);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
				}
				else if (player_next->CheckBaoHu(card)) //自摸宝胡
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(card);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
				}

				player_next->OnFaPai(cards); //放入玩家牌里面

				//听牌检查
				std::vector<Asset::PaiElement> ting_list;
				if (player_next->CheckTingPai(ting_list))
				{
					for (auto pai : ting_list) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					}
				}
				
				//杠检查：包括明杠和暗杠
				::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player_next->CheckAllGangPai(gang_list)) 
				{
					for (auto gang : gang_list) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
					}
				}
				
				//旋风杠检查
				auto xf_gang = player_next->CheckXuanFeng();
				if (xf_gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
				}

				if (alert.pais().size()) 
				{
					player_next->SendProtocol(alert); //提示Client

					_oper_limit.set_player_id(player_next->GetID()); //当前可操作玩家
					_oper_limit.set_from_player_id(player_next->GetID()); //当前牌来自玩家，自己抓牌
					_oper_limit.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
				}
				else 
				{
					_curr_player_index = next_player_index;
				}
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_HUPAI: //胡牌
		{
			std::vector<Asset::FAN_TYPE> fan_list;

			if (player->CheckHuPai(pai, fan_list)/*玩家点炮*/ || player->CheckHuPai(fan_list)/*自摸*/ || player->CheckBaoHu(pai)) //正常胡牌或者宝胡
			{
				Calculate(player->GetID(), _oper_limit.from_player_id(), fan_list); //结算

				_room->GameOver(player->GetID()); //胡牌
				_hupai_players.push_back(player->GetID()); 

				OnOver(); //结算之后才是真正结束
			}
			else
			{
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				
				auto player_next = GetNextPlayer(player->GetID());
				if (!player_next) return; 
				
				auto cards = FaPai(1); 
				player_next->OnFaPai(cards);
				
				_curr_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GANGPAI: //杠牌
		case Asset::PAI_OPER_TYPE_ANGANGPAI: //杠牌
		{
			bool ret = player->CheckGangPai(pai, _oper_limit.from_player_id());
			if (!ret) 
			{
				DEBUG_ASSERT(false);
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				return; 
			}
			else
			{
				player->OnGangPai(pai, _oper_limit.from_player_id());
				_curr_player_index = GetPlayerOrder(player->GetID()); //重置当前玩家索引

				////////听牌检查
				std::vector<Asset::PaiElement> pais;

				if (player->CheckTingPai(pais))
				{
					Asset::PaiOperationAlert alert;

					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					}

					if (alert.pais().size()) player->SendProtocol(alert); //提示Client
				}

				ClearOperation(); //清理缓存以及等待玩家操作的状态
			}
		}
		break;

		case Asset::PAI_OPER_TYPE_PENGPAI: //碰牌
		{
			bool ret = player->CheckPengPai(pai);
			if (!ret) 
			{
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				return; 
			}
			else
			{
				player->OnPengPai(pai);
				_curr_player_index = GetPlayerOrder(player->GetID()); //重置当前玩家索引

				////////听牌检查
				std::vector<Asset::PaiElement> pais;

				if (player->CheckTingPai(pais))
				{
					Asset::PaiOperationAlert alert;

					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					}

					if (alert.pais().size()) player->SendProtocol(alert); //提示Client
				}

				ClearOperation(); //清理缓存以及等待玩家操作的状态
			}
		}
		break;

		case Asset::PAI_OPER_TYPE_CHIPAI: //吃牌
		{
			bool ret = player->CheckChiPai(pai);
			if (!ret) 
			{
				player->AlertMessage(Asset::ERROR_GAME_PAI_UNSATISFIED); //没有牌满足条件
				return; 
			}
			else
			{
				player->OnChiPai(pai, message);

				////////听牌检查
				std::vector<Asset::PaiElement> pais;

				if (player->CheckTingPai(pais))
				{
					Asset::PaiOperationAlert alert;

					for (auto pai : pais) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->mutable_pai()->CopyFrom(pai);
						pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
					}

					if (alert.pais().size()) player->SendProtocol(alert); //提示Client
				}

				ClearOperation(); //清理缓存以及等待玩家操作的状态
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GIVEUP: //放弃
		{
			if (SendCheckRtn()) return;
			
			auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //如果有玩家放弃操作，则继续下个玩家

			auto player_next = GetPlayerByOrder(next_player_index);
			if (!player_next) 
			{
				DEBUG_ASSERT(false);
				return; 
			}
			
			TRACE("oper_limit.player_id:{} player_id:{} next_player_id:{} _curr_player_index:{} next_player_index:{}",
				_oper_limit.player_id(), player->GetID(), player_next->GetID(), _curr_player_index, next_player_index);

			auto cards = FaPai(1); 
			auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

			Asset::PaiOperationAlert alert;

			//胡牌检查
			if (player_next->CheckHuPai(card)) //自摸
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_pai()->CopyFrom(card);
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			}
			else if (player_next->CheckBaoHu(card)) //自摸宝胡
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_pai()->CopyFrom(card);
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			}

			player_next->OnFaPai(cards); //放入玩家牌里面
			
			////////听牌检查
			std::vector<Asset::PaiElement> ting_list;
			if (player_next->CheckTingPai(ting_list))
			{
				for (auto pai : ting_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(pai);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
				}
			}
			
			///////杠检查：明杠和暗杠
			::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
			if (player_next->CheckAllGangPai(gang_list)) 
			{
				for (auto gang : gang_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->CopyFrom(gang);
				}
			}

			if (_oper_limit.player_id() == player_next->GetID()) 
			{
				//旋风杠检查
				auto xf_gang = player->CheckXuanFeng();
				if (xf_gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
					player->SendProtocol(alert); //提示Client
				}
				else
				{
					_curr_player_index = next_player_index;
					ClearOperation(); //清理缓存以及等待玩家操作的状态
				}
			}
			else if (alert.pais().size()) 
			{
				player_next->SendProtocol(alert); //提示Client

				_oper_limit.set_player_id(player_next->GetID()); //当前可操作玩家
				_oper_limit.set_from_player_id((player_next->GetID())); //当前牌来自玩家，自己抓牌，所以是自己
				_oper_limit.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
			}
			else 
			{
				_curr_player_index = next_player_index;
			}
		}
		break;

		default:
		{
			//DEBUG("%s:line:%d 服务器接收未能处理的协议，玩家角色:player_id:%ld, 操作类型:%d\n", __func__, __LINE__, player->GetID(), pai_operate->oper_type());
			//DEBUG_ASSERT(false);
			return; //直接退出
		}
		break;
	}
}

void Game::ClearOperation()
{
	//DEBUG("%s:line:%d player_id:%ld\n", __func__, __LINE__, _oper_limit.player_id());
	_oper_limit.Clear(); //清理状态
}
	
void Game::Calculate(int64_t hupai_player_id/*胡牌玩家*/, int64_t dianpao_player_id/*点炮玩家*/, std::vector<Asset::FAN_TYPE>& fan_list)
{
	if (!_room) return;
	
	const auto fan_asset = dynamic_cast<const Asset::RoomFan*>(AssetInstance.Get(g_const->fan_id()));
	if (!fan_asset) return;

	auto get_multiple = [&](const Asset::FAN_TYPE& fan_type)->int32_t {
		auto it = std::find_if(fan_asset->fans().begin(), fan_asset->fans().end(), [fan_type](const Asset::RoomFan_FanElement& element){
			return fan_type == element.fan_type();
		});
		if (it == fan_asset->fans().end()) return 0;
		return pow(2, it->multiple());
	};

	int32_t base_score = 1;

	//玩家角色性检查(比如，庄家胡牌)
	if (IsBanker(hupai_player_id)) 
	{
		fan_list.push_back(Asset::FAN_TYPE_ZHUANG);
	}
	
	Asset::GameCalculate message;
	//
	//胡牌积分，三部分
	//
	//1.各个玩家输牌积分
	//
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return;
		
		auto player_id = player->GetID();
		
		auto record = message.mutable_record()->mutable_list()->Add();
		record->set_player_id(player_id);

		if (hupai_player_id == player_id) continue;

		int32_t score = base_score;

		//牌型基础分值计算
		for (const auto& fan : fan_list)
		{
			score *= get_multiple(fan);
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(fan);
			detail->set_score(-score);

			DEBUG("player_id:{} fan:{} score:{}", player_id, fan, -score);
		}
		
		//
		//操作和玩家牌状态分值计算
		//
		//每个玩家不同
		//
		if (dianpao_player_id == hupai_player_id)
		{
			score *= get_multiple(Asset::FAN_TYPE_ZI_MO); //自摸
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_ZI_MO);
			detail->set_score(-score);
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_ZI_MO, -score);
		}

		if (player_id == dianpao_player_id) 
		{
			score *= get_multiple(Asset::FAN_TYPE_DIAN_PAO); //炮翻番
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_DIAN_PAO);
			detail->set_score(-score);
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_DIAN_PAO, -score);
		}

		if (player->IsBimen()) 
		{
			score *= get_multiple(Asset::FAN_TYPE_BIMEN); //闭门翻番
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_BIMEN);
			detail->set_score(-score);
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_BIMEN, -score);
		}

		record->set_score(-score); //玩家所输积分
			
		DEBUG("玩家:{} 因为牌型和位置输所有积分:{}", player_id, -score);
	}
	
	//
	//2.胡牌玩家积分
	//
	//其他玩家积分之和
	//
	auto get_record = [&](int64_t player_id)->google::protobuf::internal::RepeatedPtrIterator<Adoter::Asset::GameRecord_GameElement> { 
		auto it = std::find_if(message.mutable_record()->mutable_list()->begin(), message.mutable_record()->mutable_list()->end(), 
				[player_id](const Asset::GameRecord_GameElement& ele){
					return player_id == ele.player_id();
			});
		return it;
	};
	
	auto record = get_record(hupai_player_id); 
	if (record == message.mutable_record()->mutable_list()->end()) 
	{
		ERROR("hupai_player_id:{} not found in message list", hupai_player_id);
		return;
	}

	auto total_score = 0;

	for (const auto& list_element : message.record().list())
	{
		if (list_element.player_id() == hupai_player_id) continue;

		total_score -= list_element.score();
		
		for (const auto& element : list_element.details())
		{
			const auto& fan_type = element.fan_type();

			auto it_score = std::find_if(record->mutable_details()->begin(), record->mutable_details()->end(), 
				[fan_type](const Asset::GameRecord_GameElement_DetailElement& detail_element){
					return detail_element.fan_type() == fan_type;
			});
			if (it_score == record->mutable_details()->end()) 
			{
				auto rcd = record->mutable_details()->Add();
				rcd->set_fan_type(element.fan_type());
				rcd->set_score(-element.score());

				/*
				total_score += (-element.score());

				DEBUG("hupai_player_id:{} get score:{} from player_id:{} for fan_type:{}", hupai_player_id, -element.score(), list_element.player_id(), element.fan_type());
				*/
			}
			else
			{
				it_score->set_score(it_score->score() + (-element.score())); //输牌玩家存储的是负数

				/*
				total_score += (-element.score());

				DEBUG("hupai_player_id:{} get score:{} from player_id:{} for fan_type:{}", hupai_player_id, -element.score(), list_element.player_id(), element.fan_type());
				*/
			}
		}
	}

	record->set_score(total_score); //胡牌玩家赢的总积分
	
	//
	//杠牌积分，一个部分
	//
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return;
		
		auto ming_count = player->GetMingGangCount(); 
		auto an_count = player->GetAnGangCount(); 
		auto xf_count = player->GetXuanFengCount(); 

		int32_t ming_score = ming_count * get_multiple(Asset::FAN_TYPE_MING_GANG);
		int32_t an_score = an_count * get_multiple(Asset::FAN_TYPE_AN_GANG);
		int32_t xf_score = xf_count * get_multiple(Asset::FAN_TYPE_XUAN_FENG_GANG);

		auto score = ming_score + an_score + xf_score; //玩家杠牌赢得其他单个玩家积分
				
		DEBUG("player_id:{}, ming_count:{}, an_count:{}, score:{}", player->GetID(), ming_count, an_count, score);

		auto record = message.mutable_record()->mutable_list(i);
		record->set_score(record->score() + score * (MAX_PLAYER_COUNT - 1)); //增加杠牌玩家总杠积分

		//
		//1.杠牌玩家赢得积分列表
		//
		if (ming_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
			detail->set_score(ming_score * (MAX_PLAYER_COUNT - 1));
		}

		if (an_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_AN_GANG);
			detail->set_score(an_score * (MAX_PLAYER_COUNT - 1));
		}
		
		if (xf_count)
		{
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_XUAN_FENG_GANG);
			detail->set_score(xf_score * (MAX_PLAYER_COUNT - 1));
		}

		//
		//2.其他玩家所输积分
		//
		for (int index = 0; index < MAX_PLAYER_COUNT; ++index)
		{
			if (index == i) continue;

			auto record = message.mutable_record()->mutable_list(index);
			record->set_score(record->score() - score); //扣除杠分

			//非杠牌玩家所输积分列表

			if (ming_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_MING_GANG);
				detail->set_score(-ming_score);
			}

			if (an_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_AN_GANG);
				detail->set_score(-an_score);
			}

			if (xf_count)
			{
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_XUAN_FENG_GANG);
				detail->set_score(-xf_score);
			}
		}
	}
	
	//
	//4.点炮包三家
	//
	const auto options = _room->GetOptions();
	auto it_baosanjia = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_BAOSANJIA);
	auto baosanjia = (it_baosanjia != options.extend_type().end()); //是否支持包三家

	if (baosanjia) //包三家
	{
		DEBUG("dianpao_player_id:{} 包三家积分或者钻石、欢乐豆", dianpao_player_id);

		auto it_dianpao = get_record(dianpao_player_id);
		if (it_dianpao == message.mutable_record()->mutable_list()->end()) 
		{
			DEBUG_ASSERT(false && "dianpao_player_id has not found"); //理论不会出现
			return;
		}

		for (auto player : _players)
		{
			if (!player)
			{
				DEBUG_ASSERT(false && "player in game has not found"); //理论不会出现
				continue;
			}

			auto player_id = player->GetID();

			if (player_id == hupai_player_id) continue; //和胡牌玩家无关

			auto it_player = get_record(player_id);
			if (it_player == message.mutable_record()->mutable_list()->end()) 
			{
				DEBUG_ASSERT(false && "player has not found"); //理论不会出现
				continue;
			}

			//点炮玩家付钱
			it_dianpao->set_score(it_player->score() + it_dianpao->score());
			it_player->set_score(0);
		}
	}
	
	message.PrintDebugString();
		
	BroadCast(message);
}
	
void Game::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	if (!_room) 
	{
		DEBUG_ASSERT(false);
		return;
	}
	_room->BroadCast(message, exclude_player_id);
}

void Game::BroadCast(pb::Message& message, int64_t exclude_player_id)
{
	if (!_room) 
	{
		DEBUG_ASSERT(false);
		return;
	}
	_room->BroadCast(message, exclude_player_id);
}

bool Game::SendCheckRtn()
{
	ClearOperation();

	if (_oper_list.size() == 0) return false;

	auto check = [this](Asset::PAI_OPER_TYPE rtn_type, Asset::PaiOperationList& operation)->bool {

		for (const auto& oper : _oper_list)
		{
			auto it = std::find(oper.oper_list().begin(), oper.oper_list().end(), rtn_type);

			if (it != oper.oper_list().end()) 
			{
				operation = oper;

				return true;
			}
		}
		return false;
	};

	Asset::PaiOperationList operation;
	for (int32_t i = Asset::PAI_OPER_TYPE_HUPAI; i <= Asset::PAI_OPER_TYPE_COUNT; ++i)
	{
		auto result = check((Asset::PAI_OPER_TYPE)i, operation);
		if (result) break;
	}
	if (operation.oper_list().size() == 0) 
	{
		//DEBUG("%s:line%d 没有可操作的牌值.\n", __func__, __LINE__);
		return false;
	}

	int64_t player_id = operation.player_id(); 

	_oper_limit.set_player_id(player_id); //当前可操作玩家
	_oper_limit.set_from_player_id(operation.from_player_id()); //当前牌来自玩家
	_oper_limit.mutable_pai()->CopyFrom(operation.pai()); //缓存这张牌
	_oper_limit.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
	
	Asset::PaiOperationAlert alert;

	auto pai_perator = alert.mutable_pais()->Add();
	pai_perator->mutable_pai()->CopyFrom(operation.pai());

	for (auto rtn : operation.oper_list()) 
		pai_perator->mutable_oper_list()->Add(rtn); //可操作牌类型

	if (auto player_to = GetPlayer(player_id)) 
		player_to->SendProtocol(alert); //发给目标玩家

	auto it = std::find_if(_oper_list.begin(), _oper_list.end(), [player_id](const Asset::PaiOperationList& operation) {
		return player_id == operation.player_id();
	});

	if (it != _oper_list.end()) 
	{
		DEBUG("player_id:{} from_player_id:{} card_type:{} card_value:{} oper_type:{}",
				player_id, _oper_limit.from_player_id(), _oper_limit.pai().card_type(), _oper_limit.pai().card_value(), _oper_limit.oper_type());
		_oper_list.erase(it);
	}

	return true;
}
	
/////////////////////////////////////////////////////
//
//检查各个玩家能否对该牌进行操作
//
//返回可操作玩家的索引
//
/////////////////////////////////////////////////////

bool Game::CheckPai(const Asset::PaiElement& pai, int64_t from_player_id)
{
	_oper_list.clear();

	int32_t player_index = GetPlayerOrder(from_player_id); //当前玩家索引
	if (player_index == -1) 
	{
		DEBUG_ASSERT(false);
		return false; //理论上不会出现
	}

	//assert(_curr_player_index == player_index); //理论上一定相同：错误，如果碰牌的玩家出牌就不一定

	int32_t next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;

	for (int32_t i = next_player_index; i < MAX_PLAYER_COUNT - 1 + next_player_index; ++i)
	{
		auto cur_index = i % MAX_PLAYER_COUNT;

		auto player = GetPlayerByOrder(cur_index);
		if (!player) 
		{
			DEBUG_ASSERT(false);
			return false; //理论上不会出现
		}

		if (from_player_id == player->GetID()) continue; //自己不能对自己的牌进行操作

		auto rtn_check = player->CheckPai(pai, from_player_id);
		if (rtn_check.size() == 0) continue; //不能吃、碰、杠和胡牌

		for (auto value : rtn_check)
			DEBUG("operation player can do: cur_player_index:{} next_player_index:{} player_id:{} value:{}", cur_index, next_player_index, player->GetID(), value);
		
		auto it_chi = std::find(rtn_check.begin(), rtn_check.end(), Asset::PAI_OPER_TYPE_CHIPAI);
		if (it_chi != rtn_check.end() && cur_index != next_player_index) rtn_check.erase(it_chi); //只有下家能吃牌
		
		if (rtn_check.size() == 0) continue; 

		///////////////////////////////////////////////////缓存所有操作
		Asset::PaiOperationList pai_operation;
		pai_operation.set_player_id(player->GetID());
		pai_operation.set_from_player_id(from_player_id);
		pai_operation.mutable_pai()->CopyFrom(pai);

		for (auto result : rtn_check) 
			pai_operation.mutable_oper_list()->Add(result);
		_oper_list.push_back(pai_operation);
	}

	return _oper_list.size() > 0;
}

void Game::OnOperateTimeOut()
{
}

std::vector<int32_t> Game::TailPai(size_t card_count)
{
	std::vector<int32_t> cards;
	
	if (_cards.size() < card_count) return cards;

	for (size_t i = 0; i < card_count; ++i)
	{
		int32_t value = _cards.back();	
		cards.push_back(value);
		_cards.pop_back();
	}

	return cards;
}
	
bool Game::CheckLiuJu()
{
	if (_cards.size() > size_t(g_const->liuju_count() + 4)) return false;

	Asset::LiuJu message;

	auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;

	for (int32_t i = next_player_index; i < MAX_PLAYER_COUNT - 1 + next_player_index; ++i)
	{
		auto cur_index = i % MAX_PLAYER_COUNT;

		auto player = GetPlayerByOrder(cur_index);
		if (!player)
		{
			DEBUG_ASSERT(false);
			return false;
		}

		Asset::PaiOperationAlert alert;
				
		auto cards = FaPai(1); 
		auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

		auto ju_element = message.mutable_elements()->Add();
		ju_element->mutable_pai()->CopyFrom(card);
		ju_element->set_player_id(player->GetID());

		if (player->CheckHuPai(card)) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(card);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
		}
		else if (player->CheckBaoHu(card))
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(card);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
		}
	}

	BroadCast(message);
	
	_liuju = true;

	TRACE("curr cards count:{} liuju_count:{}", _cards.size(), g_const->liuju_count());
	return true;
}

std::vector<int32_t> Game::FaPai(size_t card_count)
{
	std::vector<int32_t> cards;

	if (_cards.size() < card_count) return cards;

	for (size_t i = 0; i < card_count; ++i)
	{
		int32_t value = _cards.front();	
		cards.push_back(value);
		_cards.pop_front();
	}
	
	return cards;
}
	
std::shared_ptr<Player> Game::GetNextPlayer(int64_t player_id)
{
	if (!_room) return nullptr;

	int32_t order = GetPlayerOrder(player_id);
	if (order == -1) return nullptr;

	return GetPlayerByOrder((order + 1) % MAX_PLAYER_COUNT);
}

int32_t Game::GetPlayerOrder(int32_t player_id)
{
	if (!_room) return -1;

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];

		if (!player) continue;

		if (player->GetID() == player_id) return i; //序号
	}

	return -1;
}

std::shared_ptr<Player> Game::GetPlayerByOrder(int32_t player_index)
{
	if (!_room) return nullptr;

	if (player_index < 0 || player_index >= MAX_PLAYER_COUNT) return nullptr;

	return _players[player_index];
}

std::shared_ptr<Player> Game::GetPlayer(int64_t player_id)
{
	for (auto player : _players)
	{
		if (!player) continue;

		if (player->GetID() == player_id) return player;
	}
	
	return nullptr;
}

bool Game::IsBanker(int64_t player_id) 
{ 
	if (!_room) return false;
	return _room->IsBanker(player_id); 
}

Asset::PaiElement Game::GetBaopai(int32_t tail_index)
{
	std::vector<int32_t> list(_cards.begin(), _cards.end());

	auto card_index = list.size() - tail_index + 1; 

	return GameInstance.GetCard(list[card_index]);
}

/////////////////////////////////////////////////////
//游戏通用管理类
/////////////////////////////////////////////////////
bool GameManager::Load()
{
	std::unordered_set<pb::Message*> messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_MJ_CARD);

	for (auto message : messages)
	{
		Asset::MJCard* asset_card = dynamic_cast<Asset::MJCard*>(message); 
		if (!asset_card) return false;
		
		for (int k = 0; k < asset_card->group_count(); ++k)
		{
			int32_t cards_count = std::min(asset_card->cards_count(), asset_card->cards_size());

			for (int i = 0; i < cards_count; ++i)
			{
				Asset::PaiElement card;
				card.set_card_type(asset_card->card_type());
				card.set_card_value(asset_card->cards(i).value());

				_cards.emplace(_cards.size() + 1, card); //从1开始的索引

			}
		}
	}

	//if (_cards.size() != CARDS_COUNT) return false;
	return true;
}

void GameManager::OnCreateGame(std::shared_ptr<Game> game)
{
	_games.push_back(game);
}

}
