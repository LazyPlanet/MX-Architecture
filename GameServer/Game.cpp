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
		player->OnGameStart();
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

	for (int i = 0; i < 2; ++i)
	{
		int32_t result = CommonUtil::Random(1, 6);
		saizi.mutable_random_result()->Add(result);
		
		_saizi_random_result.push_back(result);
	}
	_room->BroadCast(saizi);
}

bool Game::OnGameOver(int64_t player_id)
{
	if (!_room) return false;

	for (auto player : _players)
	{
		if (!player) continue;

		player->OnGameOver();
	}
	
	ClearState();

	_room->OnGameOver(player_id); //胡牌

	return true;
}
	
void Game::ClearState()
{
	_baopai.Clear();

	_oper_limit.Clear();

	_oper_list.clear();

	_cards_pool.clear();
	
	_liuju = false;
}

/*
int32_t Game::GetRemainGameCount() 
{ 
	if (!_room) return 0;

	return _room->GetRemainCount();
}
*/

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
	if (!player) return false;

	if (/*_oper_limit.time_out() < CommonTimerInstance.GetTime() && 超时检查*/_oper_limit.player_id() == player->GetID()) 
	{
		return true; //玩家操作：碰、杠、胡牌
	}

	auto player_index = GetPlayerOrder(player->GetID());

	if (_curr_player_index == player_index) 
	{
		return true; //轮到该玩家
	}
	
	LOG(ERROR, "curr_player_index:{} player_index:{} player_id:{} oper_limit_player_id:{}", _curr_player_index, player_index, 
			player->GetID(), _oper_limit.player_id());
	return false;
}
	
void Game::OnPlayerReEnter(std::shared_ptr<Player> player)
{
	if (!player) return;
	
	if (!CanPaiOperate(player)) return; //尚未轮到该玩家操作

	auto player_index = GetPlayerOrder(player->GetID());
	//
	//如果轮到玩家的是胡//杠//碰则直接放弃
	//
	if (_curr_player_index != player_index && _oper_limit.player_id() == player->GetID() && (_oper_limit.oper_type() == Asset::PAI_OPER_TYPE_HUPAI || 
				_oper_limit.oper_type() == Asset::PAI_OPER_TYPE_GANGPAI || _oper_limit.oper_type() == Asset::PAI_OPER_TYPE_PENGPAI))
	{
		Asset::PaiOperation pai_operation; 
		pai_operation.set_oper_type(Asset::PAI_OPER_TYPE_GIVEUP);
		pai_operation.set_position(player->GetPosition());
		pai_operation.mutable_pai()->CopyFrom(_oper_limit.pai());

		player->CmdPaiOperate(&pai_operation);

		DEBUG("玩家:{}重入房间，操作放弃:{}", player->GetID(), _oper_limit.ShortDebugString());

		return;
	}

	//
	//玩家手里如果不是[ 13 10 7 4 1 ]数量的牌，则认为须打牌
	//
	if (!player->CheckCardsInhand()) return;

	auto cards = FaPai(1); 
	auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

	Asset::PaiOperationAlert alert;

	//胡牌检查
	if (player->CheckHuPai(card)) //自摸
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(card);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
	}

	player->OnFaPai(cards); //放入玩家牌里面

	if (player->HasTuoGuan()) return; //托管检查，防止递归
	
	//
	//玩家摸宝之后进行抓牌正好抓到宝胡
	//
	if (player->CheckBaoHu(card)) //宝胡
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_pai()->CopyFrom(card);
		pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
	}

	//
	//听牌检查
	//
	std::vector<Asset::PaiElement> ting_list;
	if (player->CheckTingPai(ting_list))
	{
		for (auto pai : ting_list) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->mutable_pai()->CopyFrom(pai);
			pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
		}
	}
	
	//
	//明杠和暗杠检查
	//
	::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
	if (player->CheckAllGangPai(gang_list)) 
	{
		for (auto gang : gang_list) 
		{
			auto pai_perator = alert.mutable_pais()->Add();
			pai_perator->CopyFrom(gang);
		}
	}
	
	//
	//旋风杠检查
	//
	auto xf_gang = player->CheckXuanFeng();
	if (xf_gang)
	{
		auto pai_perator = alert.mutable_pais()->Add();
		pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
	}

	if (alert.pais().size()) 
	{
		player->SendProtocol(alert); //提示Client

		_oper_limit.set_player_id(player->GetID()); //当前可操作玩家
		_oper_limit.set_from_player_id(player->GetID()); //当前牌来自玩家，自己抓牌
		_oper_limit.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
	}
}

void Game::OnPaiOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	if (!player || !message || !_room) return;
	
	Asset::PaiOperation* pai_operate = dynamic_cast<Asset::PaiOperation*>(message);
	if (!pai_operate) return; 
	
	auto player_index = GetPlayerOrder(player->GetID());

	DEBUG("当前操作玩家ID:{} 所在的位置索引:{} 进行的操作:{} 服务器记录的当前可操作玩家索引:{} 服务器缓存玩家操作:{}", player->GetID(), player_index, 
			pai_operate->ShortDebugString(), _curr_player_index, _oper_limit.ShortDebugString());

	if (!CanPaiOperate(player)) 
	{
		player->AlertMessage(Asset::ERROR_GAME_NO_PERMISSION); //没有权限，没到玩家操作，防止外挂
		DEBUG_ASSERT(false); 
	}

	//if (CommonTimerInstance.GetTime() < _oper_limit.time_out()) ClearOperation(); //已经超时，清理缓存以及等待玩家操作的状态

	//如果不是放弃，才是当前玩家的操作
	if (Asset::PAI_OPER_TYPE_GIVEUP != pai_operate->oper_type())
	{
		_curr_player_index = player_index; //上面检查过，就说明当前该玩家可操作
		BroadCast(message); //广播玩家操作，玩家放弃操作不能广播
	}

	//const auto& pai = _oper_limit.pai(); //缓存的牌
	const auto& pai = pai_operate->pai(); //玩家发上来的牌

	//
	//一个人打牌之后，要检查其余每个玩家手中的牌，且等待他们的操作，直到超时
	//
	switch (pai_operate->oper_type())
	{
		case Asset::PAI_OPER_TYPE_DAPAI: //打牌
		case Asset::PAI_OPER_TYPE_TINGPAI: //听牌
		{
			//
			//加入牌池
			//
			Add2CardsPool(pai);

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
				if (SendCheckRtn()) return;

				OnGameOver(0); 
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

				if (player_next->HasTuoGuan()) _curr_player_index = next_player_index; //托管检查

				player_next->OnFaPai(cards); //放入玩家牌里面

				if (player_next->HasTuoGuan()) return; //托管检查，防止递归
				
				//
				//玩家摸宝之后进行抓牌正好抓到宝胡
				//
				if (player_next->CheckBaoHu(card)) //宝胡
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(card);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
				}

				//
				//听牌检查
				//
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
				
				//
				//明杠和暗杠检查
				//
				::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
				if (player_next->CheckAllGangPai(gang_list)) 
				{
					for (auto gang : gang_list) 
					{
						auto pai_perator = alert.mutable_pais()->Add();
						pai_perator->CopyFrom(gang);
					}
				}
				
				//
				//旋风杠检查
				//
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
			if (player->CheckHuPai(pai)) //玩家点炮
			{
				auto fan_list = player->GetFanList();

				if (player->IsJinbao()) 
				{
					fan_list.emplace(Asset::FAN_TYPE_JIN_BAO);

					_oper_limit.set_from_player_id(player->GetID());
				}

				Calculate(player->GetID(), _oper_limit.from_player_id(), fan_list); //结算
			}
			else if (player->CheckZiMo(pai)) 
			{
				auto fan_list = player->GetFanList();

				_oper_limit.set_from_player_id(player->GetID()); //自摸

				if (player->IsJinbao()) 
				{
					fan_list.emplace(Asset::FAN_TYPE_JIN_BAO);
				}

				Calculate(player->GetID(), _oper_limit.from_player_id(), fan_list); //结算
			}
			else if (player->CheckBaoHu(pai)) //宝胡
			{
				auto fan_list = player->GetFanList();

				fan_list.emplace(Asset::FAN_TYPE_LOU_BAO); 
					
				_oper_limit.set_from_player_id(player->GetID());

				Calculate(player->GetID(), _oper_limit.from_player_id(), fan_list); //结算
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
				
				if (Asset::PAI_OPER_TYPE_GANGPAI == pai_operate->oper_type()) //明杠删除牌池
				{
					auto from_player = GetPlayer(_oper_limit.from_player_id());
					if (from_player) from_player->CardsPoolPop();
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

				std::vector<Asset::PaiElement> pais;
				if (player->CheckTingPai(pais)) //听牌检查
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
				
				auto from_player = GetPlayer(_oper_limit.from_player_id());
				if (from_player) from_player->CardsPoolPop();

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

				std::vector<Asset::PaiElement> pais;
				if (player->CheckTingPai(pais)) //听牌检查
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
				
				auto from_player = GetPlayer(_oper_limit.from_player_id());
				if (from_player) from_player->CardsPoolPop();

				ClearOperation(); //清理缓存以及等待玩家操作的状态
			}
		}
		break;
		
		case Asset::PAI_OPER_TYPE_GIVEUP: //放弃
		{
			if (SendCheckRtn()) 
			{
				return;
			}
			else if (IsLiuJu())
			{
				OnLiuJu();
				return;
			}
			else if (CheckLiuJu())
			{
				if (SendCheckRtn()) return;
				OnLiuJu();
				return;
			}
			
			auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT; //如果有玩家放弃操作，则继续下个玩家

			auto player_next = GetPlayerByOrder(next_player_index);
			if (!player_next) 
			{
				DEBUG_ASSERT(false);
				return; 
			}
			
			DEBUG("oper_limit.player_id:{} player_id:{} next_player_id:{} _curr_player_index:{} next_player_index:{}",
				_oper_limit.player_id(), player->GetID(), player_next->GetID(), _curr_player_index, next_player_index);

			auto cards = FaPai(1); 
			auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

			Asset::PaiOperationAlert alert;

			//
			//胡牌检查
			//
			if (player_next->CheckHuPai(card)) //自摸检查，但该张牌尚未在玩家牌内
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_pai()->CopyFrom(card);
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			}

			player_next->OnFaPai(cards); //放入玩家牌里面

			//
			//玩家摸宝之后进行抓牌正好抓到宝胡
			//
			if (player_next->CheckBaoHu(card)) //宝胡
			{
				auto pai_perator = alert.mutable_pais()->Add();
				pai_perator->mutable_pai()->CopyFrom(card);
				pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			}
			
			std::vector<Asset::PaiElement> ting_list;
			if (player_next->CheckTingPai(ting_list)) //听牌检查
			{
				for (auto pai : ting_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_pai()->CopyFrom(pai);
					pai_perator->mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_TINGPAI);
				}
			}
			
			::google::protobuf::RepeatedField<Asset::PaiOperationAlert_AlertElement> gang_list;
			if (player_next->CheckAllGangPai(gang_list)) //杠检查(明杠和暗杠)
			{
				for (auto gang : gang_list) 
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->CopyFrom(gang);
				}
			}

			//
			//开局状态，当前玩家拥有中发白白，上家打了白板
			//
			//当前玩家选择放弃，此时要提示当前玩家旋风杠.
			//
			if (_oper_limit.player_id() == player_next->GetID() || player->GetID() == player_next->GetID()/*当前操作玩家还是自己*/) //旋风杠检查
			{
				auto xf_gang = player->CheckXuanFeng();
				if (xf_gang)
				{
					auto pai_perator = alert.mutable_pais()->Add();
					pai_perator->mutable_oper_list()->Add((Asset::PAI_OPER_TYPE)xf_gang);
					//player->SendProtocol(alert); //提示Client
				}
				if (alert.pais().size()) 
				{
					player_next->SendProtocol(alert); //提示Client

					_oper_limit.set_player_id(player_next->GetID()); //当前可操作玩家
					_oper_limit.set_from_player_id((player_next->GetID())); //当前牌来自玩家，自己抓牌，所以是自己
					_oper_limit.set_time_out(CommonTimerInstance.GetTime() + 30); //8秒后超时
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
	DEBUG("_oper_limit:{}", _oper_limit.DebugString());
	_oper_limit.Clear(); //清理状态
}
	
bool Game::SanJiaBi(int64_t hupai_player_id)
{
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player || player->GetID() == hupai_player_id) continue;
		if (!player->IsBimen()) return false;
	}

	return true;
}
	
void Game::PaiPushDown()
{
	Asset::PaiPushDown proto;

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) 
		{
			ERROR("player_index:{} has not found, maybe it has disconneced.", i);
			continue;
		}

		auto player_info = proto.mutable_player_list()->Add();
		player_info->set_player_id(player->GetID());
		player_info->set_position(player->GetPosition());

		const auto& cards = player->GetCardsInhand();

		for (auto it = cards.begin(); it != cards.end(); ++it)
		{
			auto pai = player_info->mutable_pai_list()->Add();

			pai->set_card_type((Asset::CARD_TYPE)it->first); //牌类型

			for (auto card_value : it->second)
			{
				pai->mutable_cards()->Add(card_value); //牌值
			}
		}
	}

	BroadCast(proto);
}
	
void Game::Calculate(int64_t hupai_player_id/*胡牌玩家*/, int64_t dianpao_player_id/*点炮玩家*/, std::unordered_set<int32_t>& fan_list)
{
	if (!_room) return;

	DEBUG("玩家胡牌, 胡牌玩家:{} 点炮玩家:{}", hupai_player_id, dianpao_player_id);

	//
	//1.推到牌
	//
	PaiPushDown();

	//
	//2.结算
	//
	if (hupai_player_id != dianpao_player_id) _room->AddDianpao(dianpao_player_id); //不是自摸
	
	const auto options = _room->GetOptions();
	
	const auto fan_asset = dynamic_cast<const Asset::RoomFan*>(AssetInstance.Get(g_const->fan_id()));
	if (!fan_asset) return;

	auto get_multiple = [&](const int32_t fan_type)->int32_t {
		auto it = std::find_if(fan_asset->fans().begin(), fan_asset->fans().end(), [fan_type](const Asset::RoomFan_FanElement& element){
			return fan_type == element.fan_type();
		});
		if (it == fan_asset->fans().end()) return 0;
		return pow(2, it->multiple());
	};

	int32_t base_score = 1;

	//
	//玩家角色性检查(比如，庄家胡牌)
	//
	if (IsBanker(hupai_player_id)) 
	{
		fan_list.emplace(Asset::FAN_TYPE_ZHUANG);
	}

	Asset::GameCalculate message;
	message.set_calculte_type(Asset::CALCULATE_TYPE_HUPAI);
	message.mutable_baopai()->CopyFrom(_baopai);
	auto dianpao_player = GetPlayer(dianpao_player_id);
	if (dianpao_player) message.set_dianpao_player_position(dianpao_player->GetPosition());
	//
	//胡牌积分，三部分
	//
	//1.各个玩家输牌积分
	//
	
	int32_t top_mutiple = options.top_mutiple(); //封顶番数

	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) return;
		
		auto player_id = player->GetID();
		
		auto record = message.mutable_record()->mutable_list()->Add();
		record->set_player_id(player_id);
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());

		if (hupai_player_id == player_id) continue;

		int32_t score = base_score;
		
		//
		//牌型基础分值计算
		//
		for (const auto& fan : fan_list)
		{
			score *= get_multiple(fan);
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type((Asset::FAN_TYPE)fan);
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

		if (dianpao_player_id != hupai_player_id && player_id == dianpao_player_id) 
		{
			score *= get_multiple(Asset::FAN_TYPE_DIAN_PAO); 
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_DIAN_PAO);
			detail->set_score(-score);

			//
			//杠后流泪
			//
			//胡别人补杠后打出的牌
			//
			if (player->IsGangOperation()) //流泪
			{
				score *= get_multiple(Asset::FAN_TYPE_LIU_LEI); 
				
				auto detail = record->mutable_details()->Add();
				detail->set_fan_type(Asset::FAN_TYPE_LIU_LEI);
				detail->set_score(-score);
			}
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_DIAN_PAO, -score);
		}
		
		//
		//庄家
		//
		if (IsBanker(player_id)) 
		{
			score *= get_multiple(Asset::FAN_TYPE_ZHUANG); 
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_ZHUANG);
			detail->set_score(-score);
		}

		if (player->IsBimen()) 
		{
			score *= get_multiple(Asset::FAN_TYPE_BIMEN); //闭门翻番
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_BIMEN);
			detail->set_score(-score);
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_BIMEN, -score);
		}
		
		if (player->IsMingPiao()) 
		{
			score *= get_multiple(Asset::FAN_TYPE_PIAO_WEIHU); //明飘未胡
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_PIAO_WEIHU);
			detail->set_score(-score);
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_PIAO_WEIHU, -score);
		}
		
		if (SanJiaBi(hupai_player_id)) 
		{
			score *= get_multiple(Asset::FAN_TYPE_SAN_JIA_BI_MEN); //三家闭门
			
			auto detail = record->mutable_details()->Add();
			detail->set_fan_type(Asset::FAN_TYPE_SAN_JIA_BI_MEN);
			detail->set_score(-score);
			
			DEBUG("player_id:{} fan:{} score:{}", player_id, Asset::FAN_TYPE_SAN_JIA_BI_MEN, -score);
		}

		//
		//输牌玩家番数上限封底
		//
		if (top_mutiple > 0) score = std::min(top_mutiple, score); //封顶

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
		LOG(ERROR, "hupai_player_id:{} not found in message list", hupai_player_id);
		return;
	}

	auto total_score = 0;
	std::unordered_map<int64_t, int32_t> player_score; //各个玩家输积分//不包括杠

	for (const auto& list_element : message.record().list())
	{
		if (list_element.player_id() == hupai_player_id) continue;

		total_score -= list_element.score();
		player_score.emplace(list_element.player_id(), -list_element.score());
		
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
			}
			else
			{
				it_score->set_score(it_score->score() + (-element.score())); //输牌玩家存储的是负数
			}
		}
	}

	record->set_score(total_score); //胡牌玩家赢的总积分
	
	//
	//3.杠牌积分，一个部分
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
	//杠也要包赔
	//
	auto it_baosanjia = std::find(options.extend_type().begin(), options.extend_type().end(), Asset::ROOM_EXTEND_TYPE_BAOSANJIA);
	auto baosanjia = (it_baosanjia != options.extend_type().end()); //是否支持

	if (baosanjia && dianpao_player_id != 0 && dianpao_player_id != hupai_player_id) 
	{
		auto it_dianpao = get_record(dianpao_player_id);
		if (it_dianpao == message.mutable_record()->mutable_list()->end()) 
		{
			DEBUG_ASSERT(false && "dianpao_player_id has not found"); //理论不会出现
			return;
		}

		int32_t baofen_total = 0; //包积分，实则包赔两个玩家的积分

		for (auto player : _players)
		{
			if (!player)
			{
				DEBUG_ASSERT(false && "player in game has not found"); //理论不会出现
				continue;
			}

			auto player_id = player->GetID();

			if (player_id == hupai_player_id || player_id == dianpao_player_id) continue; //和胡牌玩家无关

			auto it_player = get_record(player_id);
			if (it_player == message.mutable_record()->mutable_list()->end()) 
			{
				DEBUG_ASSERT(false && "player has not found"); //理论不会出现
				continue;
			}

			it_player->set_score(it_player->score() + player_score[player_id]); //返回积分

			baofen_total += player_score[player_id]; 
		}
			
		auto detail = it_dianpao->mutable_details()->Add();
		detail->set_fan_type(Asset::FAN_TYPE_BAOSANJIA);
		detail->set_score(-baofen_total);

		it_dianpao->set_score(-baofen_total + it_dianpao->score()); //总积分
	}

	//
	//如果仅仅是平胡，则显示
	//
	for (int32_t i = 0; i < message.record().list().size(); ++i)
	{
		bool is_pinghu = true;

		if (message.record().list(i).details().size() == 0)
		{
			is_pinghu = true;
		}
		else
		{
			for (int32_t j = 0; j < message.record().list(i).details().size(); ++j)
			{
				if (message.record().list(i).details(j).fan_type() != Asset::FAN_TYPE_MING_GANG 
						&& message.record().list(i).details(j).fan_type() != Asset::FAN_TYPE_AN_GANG
						&& message.record().list(i).details(j).fan_type() != Asset::FAN_TYPE_XUAN_FENG_GANG) //只有杠是平胡
				{
					is_pinghu = false;
					break;
				}
			}
		}

		if (is_pinghu)
		{
			auto pinghu = message.mutable_record()->mutable_list(i)->mutable_details()->Add();
			pinghu->set_score(1);
			pinghu->set_fan_type(Asset::FAN_TYPE_PINGHU); 
		}
	}

	//
	//最大番数
	//
	auto max_fan_it = std::max_element(record->details().begin(), record->details().end(), 
			[&](const Asset::GameRecord_GameElement_DetailElement& detail1, const Asset::GameRecord_GameElement_DetailElement& detail2){
				return get_multiple(detail1.fan_type()) < get_multiple(detail2.fan_type()) ;
			});
	if (max_fan_it != record->details().end()) message.set_max_fan_type(max_fan_it->fan_type());

	//
	//好友房//匹配房记录消耗
	//
	auto room_type = _room->GetType();

	if (Asset::ROOM_TYPE_FRIEND == room_type)   
	{
		_room->AddGameRecord(message.record()); //本局记录
	}
	else
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return;

		for (auto player : _players)
		{
			if (!player) continue;

			auto record = get_record(player->GetID()); 
			if (record == message.mutable_record()->mutable_list()->end()) continue;
			
			auto total_score = record->score();
			auto consume_count = total_score * room_limit->base_count();
			
			auto beans_count = player->GetHuanledou();
			if (beans_count < consume_count) consume_count = beans_count; //如果玩家欢乐豆不足，则扣成0

			auto consume_real = player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_GAME, consume_count); //消耗
			if (consume_count != consume_real) continue;
		}
	}
	
	BroadCast(message);
				
	OnGameOver(hupai_player_id); //结算之后才是真正结束
	
	LOG(INFO, "胡牌结算:{}", message.ShortDebugString());
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

void Game::Add2CardsPool(Asset::PaiElement pai) 
{ 
	DEBUG("加入牌池数据:{}", pai.ShortDebugString());

	_cards_pool.push_back(pai); 
}

void Game::Add2CardsPool(Asset::CARD_TYPE card_type, int32_t card_value) 
{
	Asset::PaiElement pai;
	pai.set_card_type(card_type);
	pai.set_card_value(card_value);

	Add2CardsPool(pai);
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

	if (operation.oper_list().size() == 0) return false;

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

	if (it != _oper_list.end()) _oper_list.erase(it);

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

		auto rtn_check = player->CheckPai(pai, from_player_id); //不能包括宝胡
		if (rtn_check.size() == 0) continue; //不能吃、碰、杠和胡牌

		for (auto value : rtn_check)
			DEBUG("operation player can do: cur_player_index:{} next_player_index:{} player_id:{} value:{}", cur_index, next_player_index, player->GetID(), value);
		
		auto it_chi = std::find(rtn_check.begin(), rtn_check.end(), Asset::PAI_OPER_TYPE_CHIPAI);
		if (it_chi != rtn_check.end() && cur_index != next_player_index) rtn_check.erase(it_chi); //只有下家能吃牌
		
		if (rtn_check.size() == 0) continue; 

		//
		//缓存所有操作
		//
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

std::vector<int32_t> Game::TailPai(int32_t card_count)
{
	std::vector<int32_t> cards;
	
	if (_cards.size() < (size_t)card_count) return cards;

	for (int32_t i = 0; i < card_count; ++i)
	{
		if (_random_result - 1 != i || _random_result_list.find(i) == _random_result_list.end()) //随机的宝牌不再从后面抓
		{
			int32_t value = _cards.back();	
			cards.push_back(value);
		}

		_cards.pop_back();
	}

	if (cards.size() == 0)
	{
		int32_t value = _cards.back();	
		cards.push_back(value);

		_cards.pop_back();
	}

	return cards;
}
	
bool Game::CheckLiuJu()
{
	if (!_room) return false;

	if (_cards.size() > size_t(g_const->liuju_count() + 4)) return false;
				
	_liuju = true;

	//
	//1.流局分张
	//
	Asset::LiuJu message;
	auto next_player_index = (_curr_player_index + 1) % MAX_PLAYER_COUNT;

	for (int32_t i = next_player_index; i < MAX_PLAYER_COUNT + next_player_index; ++i)
	{
		auto cur_index = i % MAX_PLAYER_COUNT;

		auto player = GetPlayerByOrder(cur_index);
		if (!player)
		{
			DEBUG_ASSERT(false);
			return false;
		}
		
		auto cards = FaPai(1); 
		//player->OnFaPai(cards); //放入玩家牌内
		
		auto card = GameInstance.GetCard(cards[0]); //玩家待抓的牌

		//
		//各个玩家分张
		//
		auto ju_element = message.mutable_elements()->Add();
		ju_element->mutable_pai()->CopyFrom(card); 
		ju_element->set_player_id(player->GetID()); 
		
		//
		//缓存所有操作
		//
		Asset::PaiOperationList pai_operation;
		pai_operation.set_player_id(player->GetID());
		pai_operation.set_from_player_id(player->GetID());
		pai_operation.mutable_pai()->CopyFrom(card);

		if (player->CheckZiMo(card) || player->CheckHuPai(card)) 
		{
			pai_operation.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			_oper_list.push_back(pai_operation);
		}
		else if (player->CheckBaoHu(card))
		{
			pai_operation.mutable_oper_list()->Add(Asset::PAI_OPER_TYPE_HUPAI);
			_oper_list.push_back(pai_operation);
		}
	}
	
	BroadCast(message);
	
	//
	//2.胡牌检查
	//
	
	if (_oper_list.size()) return true; //玩家要进行操作

	OnLiuJu();

	return true;
}
	
void Game::OnLiuJu()
{
	if (!_room) return;
		
	//
	//推到牌
	//
	PaiPushDown();
	
	//
	//记录本次积分
	//

	Asset::GameCalculate game_calculate;
	game_calculate.set_calculte_type(Asset::CALCULATE_TYPE_LIUJU);
	game_calculate.mutable_baopai()->CopyFrom(_baopai);

	for (auto player : _players)
	{
		if (!player) continue;
		
		auto record = game_calculate.mutable_record()->mutable_list()->Add();
		record->set_player_id(player->GetID());
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag()); //理论上这种不用存盘，读取发给CLIENT的时候重新获取
	}

	BroadCast(game_calculate);
	
	_room->AddGameRecord(game_calculate.record()); //本局记录

	LOG(INFO, "流局结算:{}", game_calculate.ShortDebugString());
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
	
void Game::FaPaiAndCommonCheck()
{
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

Asset::PaiElement Game::GetBaoPai(int32_t tail_index)
{
	std::vector<int32_t> list(_cards.begin(), _cards.end());

	auto card_index = list.size() - tail_index; 

	DEBUG("生成宝牌 list.size():{} tail_index:{} card_index:{}", list.size(), tail_index, card_index);

	return GameInstance.GetCard(list[card_index]);
}

int32_t Game::GetRemainBaopai()
{
	auto count = std::count_if(_cards_pool.begin(), _cards_pool.end(), [&](const Asset::PaiElement& pai){
			return _baopai.card_type() == pai.card_type() && _baopai.card_value() == pai.card_value();
			});
	DEBUG("获取剩余宝牌{}数量，当前牌池数量:{}", _baopai.ShortDebugString(), count);
	return 3 - count; //墙上一张
}

//
//刷新宝牌，则通知全部听牌玩家
//
void Game::OnRefreshBaopai(int64_t player_id, int32_t random_result)
{
	Asset::RandomSaizi proto;
	proto.set_reason_type(Asset::RandomSaizi_REASON_TYPE_REASON_TYPE_TINGPAI);
	proto.mutable_random_result()->Add(random_result);
	proto.set_has_rand_saizi(true);

	DEBUG("刷新宝牌，玩家:{} 随机值:{}", player_id, random_result);

	for (auto player : _players)
	{
		if (!player) continue;

		if (player->IsTingPai())
		{
			player->ResetLookAtBaopai(); //玩家可以重新查看宝牌

			proto.mutable_pai()->CopyFrom(_baopai);
		}
		else
		{
			proto.mutable_pai()->Clear();
		}

		player->SendProtocol(proto); 
	}
}
	
void Game::OnTingPai(std::shared_ptr<Player> player)
{
	if (!player) return;
	
	do {
		_random_result = CommonUtil::Random(1, 6);
		_baopai = GetBaoPai(_random_result);
		_random_result_list.emplace(_random_result - 1);
	} while(GetRemainBaopai() <= 0); //直到产生还有剩余的宝牌
}

//
//游戏通用管理类
//
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
